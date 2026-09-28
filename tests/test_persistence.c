#include "opendoor/config.h"
#include "opendoor/persistence.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void write_text_file(const char *path, const char *text, mode_t mode) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    CHECK(descriptor >= 0);
    if (descriptor < 0) return;
    size_t length = strlen(text);
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        if (count <= 0) break;
        offset += (size_t)count;
    }
    CHECK(close(descriptor) == 0);
}

static char *read_text_file(const char *path, size_t *length) {
    struct stat information;
    CHECK(stat(path, &information) == 0);
    if (stat(path, &information) != 0 || information.st_size < 0) return NULL;
    char *text = malloc((size_t)information.st_size + 1U);
    CHECK(text != NULL);
    if (text == NULL) return NULL;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(descriptor >= 0);
    if (descriptor < 0) {
        free(text);
        return NULL;
    }
    size_t used = 0U;
    while (used < (size_t)information.st_size) {
        ssize_t count = read(descriptor, text + used,
                             (size_t)information.st_size - used);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        if (count <= 0) break;
        used += (size_t)count;
    }
    CHECK(close(descriptor) == 0);
    text[used] = '\0';
    *length = used;
    return text;
}

static uint64_t hash_text(const char *text, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)(unsigned char)text[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static OdResolutionItem resolution_item(const char *path,
                                        const char *relative_path,
                                        const char *text,
                                        const char *token,
                                        uint16_t old_port,
                                        uint16_t new_port,
                                        OdPortSourceKind source_kind,
                                        OdPortWriteKind write_kind,
                                        bool automatic) {
    const char *position = strstr(text, token);
    CHECK(position != NULL);
    return (OdResolutionItem){
        .old_port = old_port,
        .new_port = new_port,
        .automatic = automatic,
        .source_kind = source_kind,
        .write_kind = write_kind,
        .byte_offset = position == NULL ? 0U : (size_t)(position - text),
        .byte_length = strlen(token),
        .file_size = strlen(text),
        .file_hash = hash_text(text, strlen(text)),
        .absolute_path = (char *)path,
        .relative_path = (char *)relative_path,
        .manual_reason = automatic ? "" : "manual-only"
    };
}

static void backup_path(char *output, size_t capacity, const char *path) {
    int written = snprintf(output, capacity, "%s.opendoor.bak", path);
    CHECK(written >= 0 && (size_t)written < capacity);
}

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

static void test_transaction_patches_only_eligible_spans(void) {
    char root[] = "/tmp/opendoor-persistence-apply-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    char compose_path[512];
    char package_path[512];
    char make_path[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    (void)snprintf(compose_path, sizeof(compose_path), "%s/compose.yaml", root);
    (void)snprintf(package_path, sizeof(package_path), "%s/package.json", root);
    (void)snprintf(make_path, sizeof(make_path), "%s/Makefile", root);
    const char *env_before =
        "# keep this comment\n"
        "API_PORT = 3000 # API\n"
        "OTHER_PORT=4000\n";
    const char *compose_before =
        "services:\n"
        "  api:\n"
        "    ports:\n"
        "      - \"8080:80/tcp\" # preserve quote and comment\n";
    const char *package_before =
        "{\"scripts\":{\"dev\":\"vite --port 5173\"}}\n";
    const char *make_before = "serve:\n\tvite --port 9000\n";
    write_text_file(env_path, env_before, 0640);
    write_text_file(compose_path, compose_before, 0600);
    write_text_file(package_path, package_before, 0644);
    write_text_file(make_path, make_before, 0644);
    CHECK(chmod(env_path, 0640) == 0);
    CHECK(chmod(compose_path, 0600) == 0);

    OdResolutionItem items[] = {
        resolution_item(env_path, "./.env", env_before, "3000", 3000U, 3001U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true),
        resolution_item(compose_path, "./compose.yaml", compose_before, "8080",
                        8080U, 8081U, OD_SOURCE_COMPOSE,
                        OD_WRITE_COMPOSE_LITERAL, true),
        resolution_item(package_path, "./package.json", package_before, "5173",
                        5173U, 5174U, OD_SOURCE_PACKAGE_JSON,
                        OD_WRITE_MANUAL_ONLY, false),
        resolution_item(make_path, "./Makefile", make_before, "9000",
                        9000U, 9001U, OD_SOURCE_MAKEFILE,
                        OD_WRITE_MANUAL_ONLY, false),
        resolution_item(compose_path, "./compose.yaml", compose_before, "8080",
                        8080U, 8081U, OD_SOURCE_COMPOSE,
                        OD_WRITE_MANUAL_ONLY, false)
    };
    OdResolution resolution = {
        .items = items,
        .count = sizeof(items) / sizeof(items[0]),
        .automatic_count = 2U,
        .manual_count = 3U,
        .project_root = root
    };
    size_t updated = 99U;
    OdError error;
    CHECK(od_apply_resolution(&resolution, &updated, &error) == OD_OK);
    CHECK(updated == 2U);

    size_t length = 0U;
    char *text = read_text_file(env_path, &length);
    CHECK(text != NULL && strcmp(text,
          "# keep this comment\nAPI_PORT = 3001 # API\nOTHER_PORT=4000\n") == 0);
    free(text);
    text = read_text_file(compose_path, &length);
    CHECK(text != NULL && strcmp(text,
          "services:\n  api:\n    ports:\n      - \"8081:80/tcp\" # preserve quote and comment\n") == 0);
    free(text);
    text = read_text_file(package_path, &length);
    CHECK(text != NULL && strcmp(text, package_before) == 0);
    free(text);
    text = read_text_file(make_path, &length);
    CHECK(text != NULL && strcmp(text, make_before) == 0);
    free(text);

    char env_backup[544];
    char compose_backup[544];
    backup_path(env_backup, sizeof(env_backup), env_path);
    backup_path(compose_backup, sizeof(compose_backup), compose_path);
    text = read_text_file(env_backup, &length);
    CHECK(text != NULL && strcmp(text, env_before) == 0);
    free(text);
    text = read_text_file(compose_backup, &length);
    CHECK(text != NULL && strcmp(text, compose_before) == 0);
    free(text);
    struct stat information;
    CHECK(stat(env_path, &information) == 0 &&
          (information.st_mode & 0777) == 0640);
    CHECK(stat(compose_path, &information) == 0 &&
          (information.st_mode & 0777) == 0600);

    CHECK(unlink(compose_backup) == 0);
    CHECK(unlink(env_backup) == 0);
    CHECK(unlink(make_path) == 0);
    CHECK(unlink(package_path) == 0);
    CHECK(unlink(compose_path) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_transaction_rejects_stale_spans_before_backups(void) {
    char root[] = "/tmp/opendoor-persistence-stale-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char first_path[512];
    char second_path[512];
    (void)snprintf(first_path, sizeof(first_path), "%s/.env", root);
    (void)snprintf(second_path, sizeof(second_path), "%s/compose.yaml", root);
    const char *first = "PORT=3000\n";
    const char *second = "services:\n  app:\n    ports:\n      - \"4000:80\"\n";
    write_text_file(first_path, first, 0600);
    write_text_file(second_path, second, 0600);
    OdResolutionItem items[] = {
        resolution_item(first_path, "./.env", first, "3000", 3000U, 3001U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true),
        resolution_item(second_path, "./compose.yaml", second, "4000",
                        4000U, 4001U, OD_SOURCE_COMPOSE,
                        OD_WRITE_COMPOSE_LITERAL, true)
    };
    items[1].file_hash ^= UINT64_C(1);
    OdResolution resolution = {.items = items, .count = 2U, .automatic_count = 2U};
    resolution.project_root = root;
    size_t updated = 7U;
    OdError error;
    CHECK(od_apply_resolution(&resolution, &updated, &error) == OD_ERROR_CHANGED);
    CHECK(updated == 0U);
    char backup[544];
    backup_path(backup, sizeof(backup), first_path);
    CHECK(access(backup, F_OK) != 0 && errno == ENOENT);
    backup_path(backup, sizeof(backup), second_path);
    CHECK(access(backup, F_OK) != 0 && errno == ENOENT);
    size_t length = 0U;
    char *text = read_text_file(first_path, &length);
    CHECK(text != NULL && strcmp(text, first) == 0);
    free(text);
    text = read_text_file(second_path, &length);
    CHECK(text != NULL && strcmp(text, second) == 0);
    free(text);
    CHECK(unlink(second_path) == 0);
    CHECK(unlink(first_path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_same_file_edits_preserve_crlf_and_no_final_newline(void) {
    char root[] = "/tmp/opendoor-persistence-multi-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    const char *before = "FIRST=9999\r\nSECOND=10000";
    write_text_file(env_path, before, 0600);
    OdResolutionItem items[] = {
        resolution_item(env_path, "./.env", before, "9999", 9999U, 10000U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true),
        resolution_item(env_path, "./.env", before, "10000", 10000U, 80U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true)
    };
    items[1].byte_offset = (size_t)(strstr(before, "10000") - before);
    OdResolution resolution = {.items = items, .count = 2U, .automatic_count = 2U};
    resolution.project_root = root;
    size_t updated = 0U;
    OdError error;
    CHECK(od_apply_resolution(&resolution, &updated, &error) == OD_OK);
    CHECK(updated == 2U);
    size_t length = 0U;
    char *text = read_text_file(env_path, &length);
    CHECK(text != NULL && strcmp(text, "FIRST=10000\r\nSECOND=80") == 0);
    CHECK(length == strlen("FIRST=10000\r\nSECOND=80"));
    free(text);
    char backup[544];
    backup_path(backup, sizeof(backup), env_path);
    CHECK(unlink(backup) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

typedef struct {
    size_t written_calls;
    const char *sabotage_path;
} RollbackContext;

static OdStatus corrupt_second_written_file(OdPortSourceKind source_kind,
                                            const char *path,
                                            const char *text,
                                            size_t length,
                                            OdPatchValidationPhase phase,
                                            void *opaque,
                                            OdError *error) {
    OdStatus status = od_validate_discovery_text(source_kind, text, length, error);
    if (status != OD_OK) return status;
    if (phase == OD_PATCH_VALIDATE_WRITTEN) {
        RollbackContext *context = opaque;
        ++context->written_calls;
        if (context->written_calls == 2U) {
            if (context->sabotage_path != NULL) {
                CHECK(unlink(context->sabotage_path) == 0);
                CHECK(mkdir(context->sabotage_path, 0700) == 0);
            }
            write_text_file(path, "corrupted by test", 0600);
            od_error_set(error, OD_ERROR_INVALID, "intentional validation failure");
            return OD_ERROR_INVALID;
        }
    }
    return OD_OK;
}

static void test_transaction_rolls_back_every_file_on_validation_failure(void) {
    char root[] = "/tmp/opendoor-persistence-rollback-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    char compose_path[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    (void)snprintf(compose_path, sizeof(compose_path), "%s/compose.yaml", root);
    const char *env_before = "PORT=3000\n";
    const char *compose_before =
        "services:\n  app:\n    ports:\n      - \"4000:80\"\n";
    write_text_file(env_path, env_before, 0600);
    write_text_file(compose_path, compose_before, 0640);
    OdResolutionItem items[] = {
        resolution_item(env_path, "./.env", env_before, "3000", 3000U, 3001U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true),
        resolution_item(compose_path, "./compose.yaml", compose_before, "4000",
                        4000U, 4001U, OD_SOURCE_COMPOSE,
                        OD_WRITE_COMPOSE_LITERAL, true)
    };
    OdResolution resolution = {.items = items, .count = 2U, .automatic_count = 2U};
    resolution.project_root = root;
    RollbackContext context = {0};
    size_t updated = 5U;
    OdError error;
    CHECK(od_apply_resolution_with_validator(
              &resolution, corrupt_second_written_file, &context,
              &updated, &error) == OD_ERROR_INVALID);
    CHECK(updated == 0U);
    CHECK(strstr(error.message, "no automatic conflicts were applied") != NULL);
    size_t length = 0U;
    char *text = read_text_file(env_path, &length);
    CHECK(text != NULL && strcmp(text, env_before) == 0);
    free(text);
    text = read_text_file(compose_path, &length);
    CHECK(text != NULL && strcmp(text, compose_before) == 0);
    free(text);
    char env_backup[544];
    char compose_backup[544];
    backup_path(env_backup, sizeof(env_backup), env_path);
    backup_path(compose_backup, sizeof(compose_backup), compose_path);
    CHECK(unlink(compose_backup) == 0);
    CHECK(unlink(env_backup) == 0);
    CHECK(unlink(compose_path) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_successful_apply_backups_are_not_rediscovered(void) {
    char root[] = "/tmp/opendoor-persistence-rescan-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char env_path[512];
    (void)snprintf(env_path, sizeof(env_path), "%s/.env", root);
    const char *before = "PORT=3000\n";
    write_text_file(env_path, before, 0600);
    OdResolutionItem item = resolution_item(
        env_path, "./.env", before, "3000", 3000U, 3001U,
        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true);
    OdResolution resolution = {
        .items = &item,
        .count = 1U,
        .automatic_count = 1U,
        .project_root = root
    };
    size_t updated = 0U;
    OdError error;
    CHECK(od_apply_resolution(&resolution, &updated, &error) == OD_OK);
    CHECK(updated == 1U);

    OdProjectDiscovery discovery;
    CHECK(od_discover_project_ports(root, &discovery, &error) == OD_OK);
    CHECK(discovery.count == 1U);
    if (discovery.count == 1U) {
        CHECK(discovery.items[0].port == 3001U);
        CHECK(strcmp(discovery.items[0].relative_path, "./.env") == 0);
    }
    od_project_discovery_free(&discovery);

    char backup[544];
    backup_path(backup, sizeof(backup), env_path);
    CHECK(unlink(backup) == 0);
    CHECK(unlink(env_path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_transaction_rejects_parent_symlink_swap(void) {
    char root[] = "/tmp/opendoor-persistence-root-XXXXXX";
    char outside[] = "/tmp/opendoor-persistence-outside-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(mkdtemp(outside) != NULL);
    char inside[512];
    char moved[512];
    char original_path[512];
    char outside_path[512];
    (void)snprintf(inside, sizeof(inside), "%s/service", root);
    (void)snprintf(moved, sizeof(moved), "%s/service-original", root);
    (void)snprintf(original_path, sizeof(original_path), "%s/service/.env", root);
    (void)snprintf(outside_path, sizeof(outside_path), "%s/.env", outside);
    CHECK(mkdir(inside, 0700) == 0);
    const char *before = "PORT=3000\n";
    write_text_file(original_path, before, 0600);
    write_text_file(outside_path, before, 0600);
    OdResolutionItem item = resolution_item(
        original_path, "./service/.env", before, "3000", 3000U, 3001U,
        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true);
    OdResolution resolution = {
        .items = &item,
        .count = 1U,
        .automatic_count = 1U,
        .project_root = root
    };
    CHECK(rename(inside, moved) == 0);
    CHECK(symlink(outside, inside) == 0);

    size_t updated = 9U;
    OdError error;
    CHECK(od_apply_resolution(&resolution, &updated, &error) != OD_OK);
    CHECK(updated == 0U);
    size_t length = 0U;
    char *text = read_text_file(outside_path, &length);
    CHECK(text != NULL && strcmp(text, before) == 0);
    free(text);
    char outside_backup[544];
    backup_path(outside_backup, sizeof(outside_backup), outside_path);
    CHECK(access(outside_backup, F_OK) != 0 && errno == ENOENT);

    CHECK(unlink(inside) == 0);
    (void)snprintf(original_path, sizeof(original_path), "%s/service-original/.env", root);
    CHECK(unlink(original_path) == 0);
    CHECK(rmdir(moved) == 0);
    CHECK(unlink(outside_path) == 0);
    CHECK(rmdir(outside) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_failed_rollback_does_not_claim_nothing_changed(void) {
    char root[] = "/tmp/opendoor-persistence-rollback-failure-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char first_path[512];
    char second_path[512];
    (void)snprintf(first_path, sizeof(first_path), "%s/.env", root);
    (void)snprintf(second_path, sizeof(second_path), "%s/compose.yaml", root);
    const char *first = "PORT=3000\n";
    const char *second = "services:\n  app:\n    ports:\n      - \"4000:80\"\n";
    write_text_file(first_path, first, 0600);
    write_text_file(second_path, second, 0600);
    OdResolutionItem items[] = {
        resolution_item(first_path, "./.env", first, "3000", 3000U, 3001U,
                        OD_SOURCE_ENV, OD_WRITE_ENV_LITERAL, true),
        resolution_item(second_path, "./compose.yaml", second, "4000",
                        4000U, 4001U, OD_SOURCE_COMPOSE,
                        OD_WRITE_COMPOSE_LITERAL, true)
    };
    OdResolution resolution = {
        .items = items,
        .count = 2U,
        .automatic_count = 2U,
        .project_root = root
    };
    RollbackContext context = {.sabotage_path = first_path};
    size_t updated = 5U;
    OdError error;
    CHECK(od_apply_resolution_with_validator(
              &resolution, corrupt_second_written_file, &context,
              &updated, &error) == OD_ERROR_INVALID);
    CHECK(updated == 0U);
    CHECK(strstr(error.message, "rollback incomplete") != NULL);
    CHECK(strstr(error.message, "no automatic conflicts were applied") == NULL);

    char first_backup[544];
    char second_backup[544];
    backup_path(first_backup, sizeof(first_backup), first_path);
    backup_path(second_backup, sizeof(second_backup), second_path);
    CHECK(rmdir(first_path) == 0);
    CHECK(unlink(second_path) == 0);
    CHECK(unlink(second_backup) == 0);
    CHECK(unlink(first_backup) == 0);
    CHECK(rmdir(root) == 0);
}

int main(void) {
    test_flat_config_round_trip();
    test_flat_config_rejects_invalid_input();
    test_plain_overwrite_and_load();
    test_transaction_patches_only_eligible_spans();
    test_transaction_rejects_stale_spans_before_backups();
    test_same_file_edits_preserve_crlf_and_no_final_newline();
    test_transaction_rolls_back_every_file_on_validation_failure();
    test_successful_apply_backups_are_not_rediscovered();
    test_transaction_rejects_parent_symlink_swap();
    test_failed_rollback_does_not_claim_nothing_changed();
    if (failures != 0) {
        fprintf(stderr, "%d config checks failed\n", failures);
        return 1;
    }
    puts("config checks passed");
    return 0;
}
