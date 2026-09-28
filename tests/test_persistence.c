#include "opendoor/config.h"
#include "opendoor/persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static void test_flat_config_round_trip(void) {
    const char *source =
        "# Wanted ports\n"
        " API_PORT = 3000 \n"
        "HTTP_PORT=80\n"
        "LAST_PORT=65535\n";
    OdAssignments assignments;
    OdError error;
    CHECK(od_config_parse(source, strlen(source), &assignments, &error) == OD_OK);
    CHECK(assignments.count == 3U);
    if (assignments.count == 3U) {
        CHECK(strcmp(assignments.items[0].variable, "API_PORT") == 0);
        CHECK(assignments.items[0].port == 3000U);
        CHECK(assignments.items[1].port == 80U);
        CHECK(assignments.items[2].port == 65535U);
    }

    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_config_render(&assignments, &rendered, &length, &error) == OD_OK);
    CHECK(rendered != NULL);
    if (rendered != NULL) {
        CHECK(strcmp(rendered,
                     "API_PORT=3000\nHTTP_PORT=80\nLAST_PORT=65535\n") == 0);
        CHECK(length == strlen(rendered));
    }
    free(rendered);
    od_assignments_free(&assignments);
}

static void test_flat_config_rejects_invalid_input(void) {
    static const char *const invalid[] = {
        "bad=3000\n",
        "API_PORT=0\n",
        "API_PORT=65536\n",
        "API_PORT=3000x\n",
        "API_PORT=3000\nAPI_PORT=3001\n",
        "API_PORT\n"
    };
    for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        OdAssignments assignments;
        OdError error;
        CHECK(od_config_parse(invalid[index], strlen(invalid[index]),
                              &assignments, &error) == OD_ERROR_INVALID);
    }

    static const char embedded_nul[] =
        "API_PORT=3000\n\0WEB_PORT=4000\n";
    OdAssignments assignments;
    OdError error;
    CHECK(od_config_parse(embedded_nul, sizeof(embedded_nul) - 1U,
                          &assignments, &error) == OD_ERROR_INVALID);
}

static void test_plain_overwrite_and_load(void) {
    char path[] = "/tmp/opendoor-config-XXXXXX";
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    CHECK(close(descriptor) == 0);

    OdAssignments assignments;
    OdError error;
    CHECK(od_config_parse("API_PORT=3000\n", 14U,
                          &assignments, &error) == OD_OK);
    CHECK(od_config_write(path, &assignments, &error) == OD_OK);
    assignments.items[0].port = 3007U;
    CHECK(od_config_write(path, &assignments, &error) == OD_OK);
    od_assignments_free(&assignments);

    OdAssignments loaded;
    CHECK(od_config_load(path, &loaded, &error) == OD_OK);
    CHECK(loaded.count == 1U);
    if (loaded.count == 1U) CHECK(loaded.items[0].port == 3007U);
    od_assignments_free(&loaded);
    CHECK(unlink(path) == 0);
}

int main(void) {
    test_flat_config_round_trip();
    test_flat_config_rejects_invalid_input();
    test_plain_overwrite_and_load();
    if (failures != 0) {
        fprintf(stderr, "%d config checks failed\n", failures);
        return 1;
    }
    puts("config checks passed");
    return 0;
}
