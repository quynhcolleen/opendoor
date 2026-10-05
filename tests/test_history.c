#include "opendoor/history.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static const char apply_json[] =
    "{\"v\":1,\"id\":42,\"timestamp\":\"2026-10-05T08:14:22.123Z\","
    "\"kind\":\"apply\",\"reverts\":null,\"file\":\"./services/api/.env\","
    "\"source\":\"env\",\"write\":\"env_literal\",\"key\":\"PORT\","
    "\"line\":3,\"column\":6,\"offset\":27,\"length\":4,"
    "\"old_port\":3000,\"new_port\":3001}";

static char *replace_text(const char *text, const char *from, const char *to) {
    const char *position = strstr(text, from);
    CHECK(position != NULL);
    if (position == NULL) return NULL;
    size_t prefix = (size_t)(position - text);
    size_t suffix = strlen(position + strlen(from));
    char *changed = malloc(prefix + strlen(to) + suffix + 1U);
    CHECK(changed != NULL);
    if (changed == NULL) return NULL;
    memcpy(changed, text, prefix);
    memcpy(changed + prefix, to, strlen(to));
    memcpy(changed + prefix + strlen(to), position + strlen(from), suffix + 1U);
    return changed;
}

static char *json_with_id(uint64_t id) {
    char identity[64];
    (void)snprintf(identity, sizeof(identity), "\"id\":%" PRIu64, id);
    return replace_text(apply_json, "\"id\":42", identity);
}

static void write_bytes(int descriptor, const char *text, size_t length) {
    size_t used = 0U;
    while (used < length) {
        ssize_t count = write(descriptor, text + used, length - used);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        if (count <= 0) return;
        used += (size_t)count;
    }
}

static int create_log(const char *root, char *directory, char *path) {
    (void)snprintf(directory, 512U, "%s/.opendoor", root);
    (void)snprintf(path, 512U, "%s/.opendoor/history.log", root);
    CHECK(mkdir(directory, 0700) == 0);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(descriptor >= 0);
    return descriptor;
}

static void write_line(int descriptor, const char *text) {
    write_bytes(descriptor, text, strlen(text));
    write_bytes(descriptor, "\n", 1U);
}

static void test_flat_record_round_trip(void) {
    OdHistoryRecord record = {0};
    OdError error;
    CHECK(od_history_record_parse(apply_json, strlen(apply_json), &record,
                                  &error) == OD_OK);
    CHECK(record.version == 1U && record.id == 42U);
    CHECK(record.kind == OD_HISTORY_APPLY && !record.has_reverts);
    CHECK(record.source_kind == OD_SOURCE_ENV);
    CHECK(record.write_kind == OD_WRITE_ENV_LITERAL);
    CHECK(record.line == 3U && record.column == 6U);
    CHECK(record.byte_offset == 27U && record.byte_length == 4U);
    CHECK(record.old_port == 3000U && record.new_port == 3001U);
    CHECK(record.timestamp != NULL &&
          strcmp(record.timestamp, "2026-10-05T08:14:22.123Z") == 0);
    CHECK(record.relative_path != NULL &&
          strcmp(record.relative_path, "./services/api/.env") == 0);
    CHECK(record.environment_key != NULL &&
          strcmp(record.environment_key, "PORT") == 0);
    CHECK(record.availability == OD_HISTORY_UNCHECKED);
    CHECK(record.reason != NULL && record.reason[0] != '\0');
    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_history_record_render(&record, &rendered, &length, &error) == OD_OK);
    CHECK(rendered != NULL && strcmp(rendered, apply_json) == 0);
    CHECK(length == strlen(apply_json));
    free(rendered);
    od_history_record_free(&record);
    CHECK(record.timestamp == NULL && record.relative_path == NULL);
    od_history_record_free(&record);
}

static void test_revert_and_nullable_key_round_trip(void) {
    const char *json =
        "{\"v\":1,\"id\":43,\"timestamp\":\"2026-10-05T08:15:22.007Z\","
        "\"kind\":\"revert\",\"reverts\":42,\"file\":\"./compose.yaml\","
        "\"source\":\"compose\",\"write\":\"compose_literal\",\"key\":null,"
        "\"line\":5,\"column\":10,\"offset\":52,\"length\":4,"
        "\"old_port\":3001,\"new_port\":3000}";
    OdHistoryRecord record = {0};
    OdError error;
    CHECK(od_history_record_parse(json, strlen(json), &record, &error) == OD_OK);
    CHECK(record.kind == OD_HISTORY_REVERT && record.has_reverts);
    CHECK(record.reverts == 42U && record.environment_key == NULL);
    CHECK(record.source_kind == OD_SOURCE_COMPOSE);
    CHECK(record.write_kind == OD_WRITE_COMPOSE_LITERAL);
    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_history_record_render(&record, &rendered, &length, &error) == OD_OK);
    CHECK(rendered != NULL && strcmp(rendered, json) == 0);
    free(rendered);
    od_history_record_free(&record);
}

static void test_escaping_unicode_and_unknown_fields(void) {
    char *json = replace_text(apply_json, "./services/api/.env",
                             "./a\\\"b\\\\c\\t\\n/\\u00e9\\ud83d\\ude80/.env");
    if (json == NULL) return;
    char *extended = replace_text(json, "\"v\":1",
        "\"future\":{\"nested\":[true,false,null,-2.5e+3,{\"escaped\":\"\\\\\\\"\"}]},\"v\":1");
    free(json);
    if (extended == NULL) return;
    OdHistoryRecord record = {0};
    OdError error;
    CHECK(od_history_record_parse(extended, strlen(extended), &record, &error) == OD_OK);
    CHECK(record.relative_path != NULL &&
          strcmp(record.relative_path, "./a\"b\\c\t\n/é🚀/.env") == 0);
    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_history_record_render(&record, &rendered, &length, &error) == OD_OK);
    CHECK(rendered != NULL && strstr(rendered, "a\\\"b\\\\c\\t\\n") != NULL);
    CHECK(rendered != NULL && strstr(rendered, "future") == NULL);
    OdHistoryRecord reparsed = {0};
    if (rendered != NULL) {
        CHECK(od_history_record_parse(rendered, length, &reparsed, &error) == OD_OK);
        CHECK(reparsed.relative_path != NULL &&
              strcmp(reparsed.relative_path, "./a\"b\\c\t\n/é🚀/.env") == 0);
    }
    od_history_record_free(&reparsed);
    od_history_record_free(&record);
    free(rendered);
    free(extended);
}

static void test_invalid_records_are_rejected(void) {
    static const struct { const char *from; const char *to; } cases[] = {
        {"\"id\":42", "\"id\":0"},
        {"\"id\":42", "\"id\":-1"},
        {"\"id\":42", "\"id\":42.0"},
        {"\"id\":42", "\"id\":042"},
        {"\"id\":42", "\"id\":18446744073709551616"},
        {"\"id\":42", "\"id\":42,\"id\":43"},
        {"\"id\":42,", ""},
        {"2026-10-05T08:14:22.123Z", "2026-02-30T08:14:22.123Z"},
        {"2026-10-05T08:14:22.123Z", "2026-10-05T08:14:22Z"},
        {"2026-10-05T08:14:22.123Z", "2026-10-05T08:14:22.123+00:00"},
        {"\"kind\":\"apply\"", "\"kind\":\"batch\""},
        {"\"reverts\":null", "\"reverts\":41"},
        {"\"kind\":\"apply\"", "\"kind\":\"revert\""},
        {"./services/api/.env", "/tmp/.env"},
        {"./services/api/.env", "./services/../.env"},
        {"\"source\":\"env\"", "\"source\":\"unknown\""},
        {"\"write\":\"env_literal\"", "\"write\":\"manual_only\""},
        {"\"write\":\"env_literal\"", "\"write\":\"compose_literal\""},
        {"\"key\":\"PORT\"", "\"key\":false"},
        {"PORT", "\\u0000"},
        {"PORT", "\\ud800"},
        {"PORT", "\\udc00"},
        {"PORT", "\\x50"},
        {"PORT", "\t"},
        {"PORT", "\xc0\xaf"},
        {"\"line\":3", "\"line\":0"},
        {"\"column\":6", "\"column\":0"},
        {"\"offset\":27", "\"offset\":18446744073709551615"},
        {"\"length\":4", "\"length\":0"},
        {"\"old_port\":3000", "\"old_port\":0"},
        {"\"new_port\":3001", "\"new_port\":65536"},
        {"\"v\":1", "\"extra\":[1,],\"v\":1"},
        {"\"v\":1", "\"extra\":{\"a\":},\"v\":1"}
    };
    OdError error;
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char *json = replace_text(apply_json, cases[index].from, cases[index].to);
        if (json == NULL) continue;
        OdHistoryRecord record = {0};
        CHECK(od_history_record_parse(json, strlen(json), &record,
                                      &error) == OD_ERROR_INVALID);
        CHECK(record.relative_path == NULL && record.timestamp == NULL);
        od_history_record_free(&record);
        free(json);
    }
    OdHistoryRecord record = {0};
    char *unsupported = replace_text(apply_json, "\"v\":1", "\"v\":2");
    if (unsupported != NULL) {
        CHECK(od_history_record_parse(unsupported, strlen(unsupported), &record,
                                      &error) == OD_ERROR_UNSUPPORTED);
        free(unsupported);
    }
    CHECK(od_history_record_parse(apply_json, strlen(apply_json) - 1U, &record,
                                  &error) == OD_ERROR_INVALID);
    CHECK(od_history_record_parse("{} trailing", 11U, &record, &error) == OD_ERROR_INVALID);
}

static void test_unknown_fields_accept_all_bounded_json_values(void) {
    char *json = replace_text(apply_json, "\"v\":1",
        "\"future\":\"\\u0000\",\"v\\u0000shadow\":2,\"v\":1");
    OdHistoryRecord record = {0};
    OdError error;
    if (json != NULL) {
        CHECK(od_history_record_parse(json, strlen(json), &record, &error) == OD_OK);
        CHECK(record.version == 1U && record.id == 42U);
        od_history_record_free(&record);
        free(json);
    }
    char nested[512];
    memcpy(nested, "\"future\":", 9U);
    memset(nested + 9U, '[', 128U);
    memcpy(nested + 137U, "null", 4U);
    memset(nested + 141U, ']', 128U);
    memcpy(nested + 269U, ",\"v\":1", 7U);
    json = replace_text(apply_json, "\"v\":1", nested);
    if (json != NULL) {
        CHECK(od_history_record_parse(json, strlen(json), &record, &error) == OD_OK);
        CHECK(record.id == 42U);
        od_history_record_free(&record);
        free(json);
    }
}

static char *padded_json(size_t length) {
    size_t base = strlen(apply_json) - 1U;
    const char *prefix = ",\"padding\":\"";
    size_t padding = length - base - strlen(prefix) - 2U;
    char *json = malloc(length + 1U);
    CHECK(json != NULL);
    if (json == NULL) return NULL;
    memcpy(json, apply_json, base);
    memcpy(json + base, prefix, strlen(prefix));
    memset(json + base + strlen(prefix), 'x', padding);
    memcpy(json + length - 2U, "\"}", 3U);
    return json;
}

static void test_record_size_boundary_and_render_validation(void) {
    char *json = padded_json(65536U);
    OdHistoryRecord record = {0};
    OdError error;
    if (json != NULL) {
        CHECK(od_history_record_parse(json, 65536U, &record, &error) == OD_OK);
        free(json);
    }
    record.id = 0U;
    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_history_record_render(&record, &rendered, &length,
                                   &error) == OD_ERROR_INVALID);
    CHECK(rendered == NULL && length == 0U);
    od_history_record_free(&record);
    json = padded_json(65537U);
    if (json != NULL) {
        CHECK(od_history_record_parse(json, 65537U, &record,
                                      &error) == OD_ERROR_INVALID);
        free(json);
    }
    CHECK(od_history_record_parse(apply_json, strlen(apply_json), &record, &error) == OD_OK);
    free(record.relative_path);
    record.relative_path = malloc(65538U);
    CHECK(record.relative_path != NULL);
    if (record.relative_path != NULL) {
        record.relative_path[0] = '.';
        record.relative_path[1] = '/';
        memset(record.relative_path + 2U, 'x', 65535U);
        record.relative_path[65537U] = '\0';
        CHECK(od_history_record_render(&record, &rendered, &length, &error) == OD_ERROR_INVALID);
        CHECK(rendered == NULL && length == 0U);
    }
    od_history_record_free(&record);
    json = json_with_id(UINT64_MAX);
    if (json != NULL) {
        CHECK(od_history_record_parse(json, strlen(json), &record, &error) == OD_OK);
        CHECK(record.id == UINT64_MAX);
        CHECK(od_history_record_render(&record, &rendered, &length, &error) == OD_OK);
        CHECK(rendered != NULL && strcmp(rendered, json) == 0);
        od_history_record_free(&record);
        free(rendered);
        free(json);
    }
}

static void test_missing_storage_is_empty_without_creation(void) {
    char root[] = "/tmp/opendoor-history-empty-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char directory[512];
    char path[512];
    (void)snprintf(directory, sizeof(directory), "%s/.opendoor", root);
    (void)snprintf(path, sizeof(path), "%s/.opendoor/history.log", root);
    OdHistory history = {0};
    OdError error;
    CHECK(od_history_load(root, &history, &error) == OD_OK);
    CHECK(history.count == 0U && history.warning_count == 0U);
    struct stat information;
    CHECK(lstat(directory, &information) != 0 && errno == ENOENT);
    od_history_free(&history);
    CHECK(mkdir(directory, 0700) == 0);
    CHECK(od_history_load(root, &history, &error) == OD_OK);
    CHECK(history.count == 0U && history.warning_count == 0U);
    CHECK(lstat(path, &information) != 0 && errno == ENOENT);
    od_history_free(&history);
    CHECK(rmdir(directory) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_bad_lines_warn_and_do_not_hide_later_records(void) {
    char root[] = "/tmp/opendoor-history-load-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char directory[512];
    char path[512];
    int descriptor = create_log(root, directory, path);
    if (descriptor < 0) return;
    write_line(descriptor, apply_json);
    write_line(descriptor, "not json");
    char *unsupported = replace_text(apply_json, "\"v\":1", "\"v\":2");
    if (unsupported != NULL) write_line(descriptor, unsupported);
    write_line(descriptor, apply_json);
    char *oversized = padded_json(65537U);
    if (oversized != NULL) write_line(descriptor, oversized);
    char *later = json_with_id(43U);
    if (later != NULL) write_line(descriptor, later);
    char *torn = json_with_id(44U);
    if (torn != NULL) write_bytes(descriptor, torn, strlen(torn));
    CHECK(close(descriptor) == 0);
    struct stat before;
    CHECK(stat(path, &before) == 0);
    OdHistory history = {0};
    OdError error;
    CHECK(od_history_load(root, &history, &error) == OD_OK);
    CHECK(history.count == 2U && history.warning_count == 5U);
    if (history.count == 2U) {
        CHECK(history.items[0].id == 43U && history.items[1].id == 42U);
    }
    for (size_t index = 0U; index < history.warning_count; ++index) {
        CHECK(history.warnings[index] != NULL && history.warnings[index][0] != '\0');
        CHECK(strstr(history.warnings[index], "history.log:") != NULL);
    }
    if (history.warning_count == 5U) {
        CHECK(strstr(history.warnings[1], "unsupported") != NULL);
        CHECK(strstr(history.warnings[2], "duplicate") != NULL);
        CHECK(strstr(history.warnings[3], "64 KiB") != NULL);
        CHECK(strstr(history.warnings[4], "torn") != NULL);
    }
    struct stat after;
    CHECK(stat(path, &after) == 0 && after.st_size == before.st_size);
    od_history_free(&history);
    CHECK(history.items == NULL && history.warnings == NULL && history.count == 0U);
    free(unsupported);
    free(oversized);
    free(later);
    free(torn);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_all_valid_records_load_newest_first(void) {
    char root[] = "/tmp/opendoor-history-retain-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char directory[512];
    char path[512];
    int descriptor = create_log(root, directory, path);
    if (descriptor < 0) return;
    for (uint64_t id = 1U; id <= 1001U; ++id) {
        char *json = json_with_id(id);
        if (json != NULL) write_line(descriptor, json);
        free(json);
    }
    CHECK(close(descriptor) == 0);
    OdHistory history = {0};
    OdError error;
    CHECK(od_history_load(root, &history, &error) == OD_OK);
    CHECK(history.count == 1001U && history.warning_count == 0U);
    if (history.count == 1001U) {
        for (size_t index = 0U; index < history.count; ++index) {
            CHECK(history.items[index].id == 1001U - index);
        }
    }
    od_history_free(&history);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_storage_symlinks_and_nonregular_logs_are_refused(void) {
    char root[] = "/tmp/opendoor-history-safe-XXXXXX";
    char outside[] = "/tmp/opendoor-history-outside-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(mkdtemp(outside) != NULL);
    char directory[512];
    char path[512];
    char outside_log[512];
    (void)snprintf(directory, sizeof(directory), "%s/.opendoor", root);
    (void)snprintf(path, sizeof(path), "%s/.opendoor/history.log", root);
    (void)snprintf(outside_log, sizeof(outside_log), "%s/history.log", outside);
    int descriptor = open(outside_log, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    CHECK(descriptor >= 0);
    if (descriptor >= 0) {
        write_line(descriptor, apply_json);
        CHECK(close(descriptor) == 0);
    }
    CHECK(symlink(outside, directory) == 0);
    OdHistory history = {0};
    OdError error;
    CHECK(od_history_load(root, &history, &error) == OD_ERROR_IO);
    CHECK(history.count == 0U && history.items == NULL);
    CHECK(unlink(directory) == 0);
    CHECK(mkdir(directory, 0700) == 0);
    CHECK(symlink(outside_log, path) == 0);
    CHECK(od_history_load(root, &history, &error) == OD_ERROR_IO);
    CHECK(unlink(path) == 0);
    CHECK(mkdir(path, 0700) == 0);
    CHECK(od_history_load(root, &history, &error) == OD_ERROR_IO);
    CHECK(rmdir(path) == 0);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(od_history_load(root, &history, &error) == OD_ERROR_IO);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    CHECK(unlink(outside_log) == 0);
    CHECK(rmdir(outside) == 0);
    CHECK(rmdir(root) == 0);
}

int main(void) {
    test_flat_record_round_trip();
    test_revert_and_nullable_key_round_trip();
    test_escaping_unicode_and_unknown_fields();
    test_invalid_records_are_rejected();
    test_unknown_fields_accept_all_bounded_json_values();
    test_record_size_boundary_and_render_validation();
    test_missing_storage_is_empty_without_creation();
    test_bad_lines_warn_and_do_not_hide_later_records();
    test_all_valid_records_load_newest_first();
    test_storage_symlinks_and_nonregular_logs_are_refused();
    if (failures != 0) {
        fprintf(stderr, "%d history checks failed\n", failures);
        return 1;
    }
    puts("history checks passed");
    return 0;
}
