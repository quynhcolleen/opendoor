#include "opendoor/discovery.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OD_PORT_COUNT 65536U
#define OD_DISCOVERY_FILE_MAX (4U * 1024U * 1024U)

static char *copy_bytes(const char *value, size_t length) {
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    if (length > 0U) memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static char *copy_string(const char *value) {
    return copy_bytes(value, strlen(value));
}

static uint64_t hash_bytes(const char *data, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)(unsigned char)data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static char *joined_path(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left_length > 0U && left[left_length - 1U] != '/';
    if (left_length > SIZE_MAX - right_length - (separator ? 2U : 1U)) return NULL;
    size_t capacity = left_length + right_length + (separator ? 2U : 1U);
    char *path = malloc(capacity);
    if (path == NULL) return NULL;
    int written = snprintf(path, capacity, separator ? "%s/%s" : "%s%s", left, right);
    if (written < 0 || (size_t)written >= capacity) {
        free(path);
        return NULL;
    }
    return path;
}

static char *display_path(const char *relative) {
    size_t length = strlen(relative);
    if (length > SIZE_MAX - 3U) return NULL;
    char *path = malloc(length + 3U);
    if (path == NULL) return NULL;
    int written = snprintf(path, length + 3U, "./%s", relative);
    if (written < 0 || (size_t)written >= length + 3U) {
        free(path);
        return NULL;
    }
    return path;
}

static char *display_folder(const char *relative_path) {
    const char *slash = strrchr(relative_path, '/');
    if (slash == NULL) return copy_string("./");
    size_t length = (size_t)(slash - relative_path);
    if (length > SIZE_MAX - 3U) return NULL;
    char *folder = malloc(length + 3U);
    if (folder == NULL) return NULL;
    folder[0] = '.';
    folder[1] = '/';
    if (length > 0U) memcpy(folder + 2U, relative_path, length);
    folder[length + 2U] = '\0';
    return folder;
}

static bool skipped_directory(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 ||
           strcmp(name, "vendor") == 0 || strcmp(name, "build") == 0 ||
           strncmp(name, "build-", 6U) == 0 || strcmp(name, "dist") == 0;
}

static bool has_suffix(const char *value, const char *suffix) {
    size_t value_length = strlen(value);
    size_t suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           strcmp(value + value_length - suffix_length, suffix) == 0;
}

static bool env_filename(const char *name) {
    if (strcmp(name, ".env") != 0 && strncmp(name, ".env.", 5U) != 0) return false;
    return !has_suffix(name, ".example") && !has_suffix(name, ".sample") &&
           !has_suffix(name, ".template") &&
           !has_suffix(name, ".opendoor.bak") &&
           strstr(name, ".opendoor.tmp.") == NULL;
}

static bool compose_filename(const char *name) {
    return strcmp(name, "docker-compose.yml") == 0 ||
           strcmp(name, "compose.yaml") == 0;
}

static bool parse_port_number(const char *text, size_t length, uint16_t *port) {
    if (text == NULL || length == 0U || port == NULL) return false;
    unsigned long numeric = 0UL;
    for (size_t index = 0U; index < length; ++index) {
        if (isdigit((unsigned char)text[index]) == 0) return false;
        numeric = numeric * 10UL + (unsigned long)(text[index] - '0');
        if (numeric > 65535UL) return false;
    }
    if (numeric == 0UL) return false;
    *port = (uint16_t)numeric;
    return true;
}

static void declaration_free(OdPortDeclaration *item) {
    if (item == NULL) return;
    free(item->absolute_path);
    free(item->relative_path);
    free(item->relative_folder);
    free(item->environment_key);
    free(item->line_text);
    free(item->manual_reason);
    *item = (OdPortDeclaration){0};
}

void od_project_discovery_free(OdProjectDiscovery *result) {
    if (result == NULL) return;
    for (size_t index = 0U; index < result->count; ++index) {
        declaration_free(&result->items[index]);
    }
    free(result->items);
    for (size_t index = 0U; index < result->environment_assignment_count; ++index) {
        free(result->environment_assignments[index].relative_path);
        free(result->environment_assignments[index].relative_folder);
        free(result->environment_assignments[index].environment_key);
    }
    free(result->environment_assignments);
    for (size_t index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index]);
    }
    free(result->warnings);
    *result = (OdProjectDiscovery){0};
}

static OdStatus add_warning(OdProjectDiscovery *result,
                            const char *path,
                            const char *detail,
                            OdError *error) {
    size_t length = strlen(path) + strlen(detail) + 3U;
    char *warning = malloc(length);
    if (warning == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store discovery warning");
        return OD_ERROR_MEMORY;
    }
    (void)snprintf(warning, length, "%s: %s", path, detail);
    char **warnings = realloc(result->warnings,
                              (result->warning_count + 1U) * sizeof(*warnings));
    if (warnings == NULL) {
        free(warning);
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow discovery warnings");
        return OD_ERROR_MEMORY;
    }
    result->warnings = warnings;
    result->warnings[result->warning_count++] = warning;
    return OD_OK;
}

static OdStatus append_env_declaration(OdProjectDiscovery *result,
                                       const char *absolute_path,
                                       const char *relative_path,
                                       const char *line_text,
                                       size_t line_length,
                                       const char *key,
                                       size_t key_length,
                                       uint16_t port,
                                       size_t line_number,
                                       size_t column,
                                       size_t byte_offset,
                                       size_t byte_length,
                                       size_t file_size,
                                       uint64_t file_hash,
                                       OdError *error) {
    char *relative_display = display_path(relative_path);
    if (relative_display == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store discovery path");
        return OD_ERROR_MEMORY;
    }
    OdPortDeclaration *items = realloc(
        result->items, (result->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(relative_display);
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow discovered ports");
        return OD_ERROR_MEMORY;
    }
    result->items = items;
    OdPortDeclaration *item = &result->items[result->count];
    *item = (OdPortDeclaration){
        .source_kind = OD_SOURCE_ENV,
        .declaration_kind = OD_DECLARATION_LITERAL,
        .write_kind = OD_WRITE_ENV_LITERAL,
        .port = port,
        .line = line_number,
        .column = column,
        .byte_offset = byte_offset,
        .byte_length = byte_length,
        .file_size = file_size,
        .file_hash = file_hash,
        .definition_index = SIZE_MAX,
        .relative_path = relative_display
    };
    item->absolute_path = copy_string(absolute_path);
    item->relative_folder = display_folder(relative_path);
    item->environment_key = copy_bytes(key, key_length);
    item->line_text = copy_bytes(line_text, line_length);
    item->manual_reason = copy_string("");
    if (item->absolute_path == NULL || item->relative_folder == NULL ||
        item->environment_key == NULL || item->line_text == NULL ||
        item->manual_reason == NULL) {
        declaration_free(item);
        od_error_set(error, OD_ERROR_MEMORY, "unable to store discovered port");
        return OD_ERROR_MEMORY;
    }
    ++result->count;
    return OD_OK;
}

static OdStatus append_environment_assignment(OdProjectDiscovery *result,
                                              const char *relative_path,
                                              const char *key,
                                              size_t key_length,
                                              size_t value_offset,
                                              bool direct_port,
                                              OdError *error) {
    OdEnvironmentAssignment *items = realloc(
        result->environment_assignments,
        (result->environment_assignment_count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY,
                     "unable to track environment assignments");
        return OD_ERROR_MEMORY;
    }
    result->environment_assignments = items;
    OdEnvironmentAssignment *assignment =
        &items[result->environment_assignment_count];
    *assignment = (OdEnvironmentAssignment){
        .value_offset = value_offset,
        .direct_port = direct_port
    };
    assignment->relative_path = display_path(relative_path);
    assignment->relative_folder = display_folder(relative_path);
    assignment->environment_key = copy_bytes(key, key_length);
    if (assignment->relative_path == NULL ||
        assignment->relative_folder == NULL ||
        assignment->environment_key == NULL) {
        free(assignment->relative_path);
        free(assignment->relative_folder);
        free(assignment->environment_key);
        *assignment = (OdEnvironmentAssignment){0};
        od_error_set(error, OD_ERROR_MEMORY,
                     "unable to store environment assignment");
        return OD_ERROR_MEMORY;
    }
    ++result->environment_assignment_count;
    return OD_OK;
}

static bool identifier_start(unsigned char character) {
    return character == '_' || isalpha(character) != 0;
}

static bool identifier_continue(unsigned char character) {
    return character == '_' || isalnum(character) != 0;
}

static bool port_key_segment(const char *key, size_t length) {
    size_t segment_start = 0U;
    for (size_t cursor = 0U; cursor <= length; ++cursor) {
        if (cursor < length && key[cursor] != '_') continue;
        size_t segment_length = cursor - segment_start;
        if (segment_length == 4U &&
            toupper((unsigned char)key[segment_start]) == 'P' &&
            toupper((unsigned char)key[segment_start + 1U]) == 'O' &&
            toupper((unsigned char)key[segment_start + 2U]) == 'R' &&
            toupper((unsigned char)key[segment_start + 3U]) == 'T') {
            return true;
        }
        segment_start = cursor + 1U;
    }
    return false;
}

static bool case_insensitive_key(const char *key,
                                 size_t length,
                                 const char *expected) {
    size_t expected_length = strlen(expected);
    if (length != expected_length) return false;
    for (size_t index = 0U; index < length; ++index) {
        if (toupper((unsigned char)key[index]) !=
            toupper((unsigned char)expected[index])) {
            return false;
        }
    }
    return true;
}

static bool address_key_allowlisted(const char *key, size_t length) {
    return case_insensitive_key(key, length, "ADDR") ||
           case_insensitive_key(key, length, "LISTEN") ||
           case_insensitive_key(key, length, "BIND");
}

static bool valid_env_host(const char *text, size_t length) {
    if (length == 0U) return true;
    size_t label_start = 0U;
    for (size_t cursor = 0U; cursor <= length; ++cursor) {
        if (cursor < length && text[cursor] != '.') {
            unsigned char character = (unsigned char)text[cursor];
            if (isalnum(character) == 0 && character != '-') return false;
            continue;
        }
        if (cursor == label_start ||
            isalnum((unsigned char)text[label_start]) == 0 ||
            isalnum((unsigned char)text[cursor - 1U]) == 0) {
            return false;
        }
        label_start = cursor + 1U;
    }
    return true;
}

static OdStatus parse_env_text(OdProjectDiscovery *result,
                               const char *absolute_path,
                               const char *relative_path,
                               const char *text,
                               size_t length,
                               OdError *error) {
    uint64_t file_hash = hash_bytes(text, length);
    size_t line_number = 1U;
    size_t line_start = 0U;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1U] == '\r') --content_end;
        size_t cursor = line_start;
        while (cursor < content_end &&
               (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
        if (cursor < content_end && text[cursor] != '#' &&
            identifier_start((unsigned char)text[cursor])) {
            size_t key_start = cursor++;
            while (cursor < content_end &&
                   identifier_continue((unsigned char)text[cursor])) ++cursor;
            size_t key_end = cursor;
            while (cursor < content_end &&
                   (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
            if (cursor < content_end && text[cursor] == '=') {
                ++cursor;
                while (cursor < content_end &&
                       (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                char quote = '\0';
                if (cursor < content_end && (text[cursor] == '\'' || text[cursor] == '"')) {
                    quote = text[cursor++];
                }
                bool named_port = port_key_segment(text + key_start,
                                                   key_end - key_start);
                bool address_key = address_key_allowlisted(text + key_start,
                                                           key_end - key_start);
                size_t port_start = SIZE_MAX;
                if (named_port || !address_key) {
                    port_start = cursor;
                    while (cursor < content_end &&
                           isdigit((unsigned char)text[cursor]) != 0) {
                        ++cursor;
                    }
                } else if (address_key) {
                    size_t host_start = cursor;
                    while (cursor < content_end && text[cursor] != ':' &&
                           text[cursor] != quote && text[cursor] != ' ' &&
                           text[cursor] != '\t' && text[cursor] != '#') {
                        ++cursor;
                    }
                    if (cursor < content_end && text[cursor] == ':' &&
                        valid_env_host(text + host_start, cursor - host_start)) {
                        port_start = ++cursor;
                        while (cursor < content_end &&
                               isdigit((unsigned char)text[cursor]) != 0) {
                            ++cursor;
                        }
                    }
                }
                size_t port_end = cursor;
                bool complete = port_start != SIZE_MAX && port_end > port_start;
                if (quote != '\0') {
                    complete = complete && cursor < content_end && text[cursor] == quote;
                    if (complete) ++cursor;
                }
                while (cursor < content_end &&
                       (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                complete = complete && (cursor == content_end || text[cursor] == '#');
                bool direct_port = false;
                if (complete) {
                    uint16_t port = 0U;
                    if (parse_port_number(text + port_start,
                                          port_end - port_start, &port)) {
                        direct_port = !address_key;
                        OdStatus status = append_env_declaration(
                            result, absolute_path, relative_path, text + line_start,
                            content_end - line_start, text + key_start,
                            key_end - key_start, port, line_number,
                            port_start - line_start + 1U, port_start,
                            port_end - port_start, length, file_hash, error);
                        if (status != OD_OK) return status;
                    }
                }
                OdStatus assignment_status = append_environment_assignment(
                    result, relative_path, text + key_start, key_end - key_start,
                    direct_port ? port_start : SIZE_MAX, direct_port, error);
                if (assignment_status != OD_OK) return assignment_status;
            }
        }
        line_start = line_end < length ? line_end + 1U : length;
        ++line_number;
    }
    return OD_OK;
}

typedef struct {
    bool reference;
    uint16_t port;
    uint16_t fallback_port;
    size_t host_start;
    size_t host_length;
    size_t key_start;
    size_t key_length;
    size_t fallback_start;
    size_t fallback_length;
} ComposeMapping;

static bool parse_compose_scalar(const char *scalar,
                                 size_t length,
                                 ComposeMapping *mapping) {
    if (scalar == NULL || mapping == NULL || length == 0U) return false;
    *mapping = (ComposeMapping){0};
    size_t cursor = 0U;
    mapping->host_start = 0U;
    if (scalar[cursor] == '$') {
        mapping->reference = true;
        ++cursor;
        if (cursor < length && scalar[cursor] == '{') {
            ++cursor;
            if (cursor >= length ||
                !identifier_start((unsigned char)scalar[cursor])) return false;
            mapping->key_start = cursor++;
            while (cursor < length &&
                   identifier_continue((unsigned char)scalar[cursor])) ++cursor;
            mapping->key_length = cursor - mapping->key_start;
            if (cursor < length && scalar[cursor] == '}') {
                ++cursor;
            } else {
                size_t separator_length = 0U;
                if (cursor + 1U < length && scalar[cursor] == ':' &&
                    scalar[cursor + 1U] == '-') {
                    separator_length = 2U;
                } else if (cursor < length && scalar[cursor] == '-') {
                    separator_length = 1U;
                } else {
                    return false;
                }
                cursor += separator_length;
                mapping->fallback_start = cursor;
                while (cursor < length &&
                       isdigit((unsigned char)scalar[cursor]) != 0) ++cursor;
                mapping->fallback_length = cursor - mapping->fallback_start;
                if (mapping->fallback_length == 0U || cursor >= length ||
                    scalar[cursor] != '}' ||
                    !parse_port_number(scalar + mapping->fallback_start,
                                       mapping->fallback_length,
                                       &mapping->fallback_port)) {
                    return false;
                }
                mapping->port = mapping->fallback_port;
                ++cursor;
            }
        } else {
            if (cursor >= length ||
                !identifier_start((unsigned char)scalar[cursor])) return false;
            mapping->key_start = cursor++;
            while (cursor < length &&
                   identifier_continue((unsigned char)scalar[cursor])) ++cursor;
            mapping->key_length = cursor - mapping->key_start;
        }
        mapping->host_length = cursor;
    } else {
        size_t port_start = cursor;
        while (cursor < length && isdigit((unsigned char)scalar[cursor]) != 0) {
            ++cursor;
        }
        mapping->host_length = cursor - port_start;
        if (!parse_port_number(scalar + port_start, mapping->host_length,
                               &mapping->port)) return false;
    }
    if (cursor >= length || scalar[cursor] != ':') return false;
    ++cursor;
    size_t container_start = cursor;
    while (cursor < length && isdigit((unsigned char)scalar[cursor]) != 0) ++cursor;
    uint16_t container_port = 0U;
    if (!parse_port_number(scalar + container_start, cursor - container_start,
                           &container_port)) return false;
    if (cursor < length) {
        if (length - cursor == 4U &&
            (memcmp(scalar + cursor, "/tcp", 4U) == 0 ||
             memcmp(scalar + cursor, "/udp", 4U) == 0)) {
            cursor = length;
        } else {
            return false;
        }
    }
    return cursor == length;
}

static OdStatus append_compose_declaration(OdProjectDiscovery *result,
                                           const char *absolute_path,
                                           const char *relative_path,
                                           const char *line_text,
                                           size_t line_length,
                                           const char *scalar,
                                           size_t scalar_file_offset,
                                           const ComposeMapping *mapping,
                                           size_t line_number,
                                           size_t line_start,
                                           size_t file_size,
                                           uint64_t file_hash,
                                           OdError *error) {
    size_t token_start = mapping->host_start;
    size_t token_length = mapping->host_length;
    if (mapping->reference && mapping->fallback_length > 0U) {
        token_start = mapping->fallback_start;
        token_length = mapping->fallback_length;
    }
    char *relative_display = display_path(relative_path);
    if (relative_display == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store Compose path");
        return OD_ERROR_MEMORY;
    }
    OdPortDeclaration *items = realloc(
        result->items, (result->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(relative_display);
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow discovered ports");
        return OD_ERROR_MEMORY;
    }
    result->items = items;
    OdPortDeclaration *item = &result->items[result->count];
    *item = (OdPortDeclaration){
        .source_kind = OD_SOURCE_COMPOSE,
        .declaration_kind = mapping->reference ?
            OD_DECLARATION_ENV_REFERENCE : OD_DECLARATION_LITERAL,
        .write_kind = mapping->reference ?
            OD_WRITE_MANUAL_ONLY : OD_WRITE_COMPOSE_LITERAL,
        .port = mapping->port,
        .fallback_port = mapping->fallback_port,
        .line = line_number,
        .column = scalar_file_offset + token_start - line_start + 1U,
        .byte_offset = scalar_file_offset + token_start,
        .byte_length = token_length,
        .file_size = file_size,
        .file_hash = file_hash,
        .definition_index = SIZE_MAX,
        .relative_path = relative_display
    };
    item->absolute_path = copy_string(absolute_path);
    item->relative_folder = display_folder(relative_path);
    item->environment_key = mapping->reference ?
        copy_bytes(scalar + mapping->key_start, mapping->key_length) :
        copy_string("");
    item->line_text = copy_bytes(line_text, line_length);
    item->manual_reason = copy_string(mapping->reference ?
        "environment reference is manual-only" : "");
    if (item->absolute_path == NULL || item->relative_folder == NULL ||
        item->environment_key == NULL || item->line_text == NULL ||
        item->manual_reason == NULL) {
        declaration_free(item);
        od_error_set(error, OD_ERROR_MEMORY, "unable to store Compose port");
        return OD_ERROR_MEMORY;
    }
    ++result->count;
    return OD_OK;
}

static OdStatus append_manual_declaration(OdProjectDiscovery *result,
                                          OdPortSourceKind source_kind,
                                          OdPortDeclarationKind declaration_kind,
                                          const char *absolute_path,
                                          const char *relative_path,
                                          const char *text,
                                          size_t file_size,
                                          uint64_t file_hash,
                                          uint16_t port,
                                          const char *environment_key,
                                          size_t key_length,
                                          size_t token_offset,
                                          size_t token_length,
                                          const char *reason,
                                          OdError *error) {
    char *relative_display = display_path(relative_path);
    if (relative_display == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store manual declaration path");
        return OD_ERROR_MEMORY;
    }
    size_t line_start = token_offset;
    while (line_start > 0U && text[line_start - 1U] != '\n') --line_start;
    size_t line_end = token_offset;
    while (line_end < file_size && text[line_end] != '\n') ++line_end;
    if (line_end > line_start && text[line_end - 1U] == '\r') --line_end;
    size_t line_number = 1U;
    for (size_t index = 0U; index < line_start; ++index) {
        if (text[index] == '\n') ++line_number;
    }
    OdPortDeclaration *items = realloc(
        result->items, (result->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(relative_display);
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow discovered ports");
        return OD_ERROR_MEMORY;
    }
    result->items = items;
    OdPortDeclaration *item = &result->items[result->count];
    *item = (OdPortDeclaration){
        .source_kind = source_kind,
        .declaration_kind = declaration_kind,
        .write_kind = OD_WRITE_MANUAL_ONLY,
        .port = port,
        .line = line_number,
        .column = token_offset - line_start + 1U,
        .byte_offset = token_offset,
        .byte_length = token_length,
        .file_size = file_size,
        .file_hash = file_hash,
        .definition_index = SIZE_MAX,
        .relative_path = relative_display
    };
    item->absolute_path = copy_string(absolute_path);
    item->relative_folder = display_folder(relative_path);
    item->environment_key = environment_key == NULL ? copy_string("") :
        copy_bytes(environment_key, key_length);
    item->line_text = copy_bytes(text + line_start, line_end - line_start);
    item->manual_reason = copy_string(reason);
    if (item->absolute_path == NULL || item->relative_folder == NULL ||
        item->environment_key == NULL || item->line_text == NULL ||
        item->manual_reason == NULL) {
        declaration_free(item);
        od_error_set(error, OD_ERROR_MEMORY, "unable to store manual declaration");
        return OD_ERROR_MEMORY;
    }
    ++result->count;
    return OD_OK;
}

static bool exact_ports_key(const char *text, size_t start, size_t end) {
    size_t cursor = start;
    if (end - cursor < 6U || memcmp(text + cursor, "ports:", 6U) != 0) {
        return false;
    }
    cursor += 6U;
    while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
    return cursor == end || text[cursor] == '#';
}

static bool compose_list_scalar(const char *text,
                                size_t content_start,
                                size_t content_end,
                                size_t *scalar_start,
                                size_t *scalar_end,
                                bool *malformed_quote) {
    *malformed_quote = false;
    size_t cursor = content_start;
    if (cursor >= content_end || text[cursor] != '-') return false;
    ++cursor;
    if (cursor < content_end && text[cursor] != ' ' && text[cursor] != '\t') {
        return false;
    }
    while (cursor < content_end && (text[cursor] == ' ' || text[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= content_end || text[cursor] == '#') return false;
    char quote = '\0';
    if (text[cursor] == '\'' || text[cursor] == '"') quote = text[cursor++];
    *scalar_start = cursor;
    if (quote != '\0') {
        while (cursor < content_end && text[cursor] != quote) ++cursor;
        if (cursor >= content_end) {
            *malformed_quote = true;
            return false;
        }
        *scalar_end = cursor++;
        while (cursor < content_end && (text[cursor] == ' ' || text[cursor] == '\t')) {
            ++cursor;
        }
        return cursor == content_end || text[cursor] == '#';
    }
    while (cursor < content_end && text[cursor] != ' ' && text[cursor] != '\t' &&
           text[cursor] != '#') ++cursor;
    *scalar_end = cursor;
    while (cursor < content_end && (text[cursor] == ' ' || text[cursor] == '\t')) {
        ++cursor;
    }
    return cursor == content_end || text[cursor] == '#';
}

static bool compose_block_scalar_header(const char *text,
                                        size_t content_start,
                                        size_t content_end) {
    size_t value_start = SIZE_MAX;
    char quote = '\0';
    for (size_t cursor = content_start; cursor < content_end; ++cursor) {
        char character = text[cursor];
        if (quote != '\0') {
            if (character == quote &&
                (quote == '\'' || cursor == content_start ||
                 text[cursor - 1U] != '\\')) {
                quote = '\0';
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '#') {
            break;
        } else if (character == ':') {
            value_start = cursor + 1U;
            break;
        }
    }
    if (value_start == SIZE_MAX && text[content_start] == '-') {
        value_start = content_start + 1U;
    }
    if (value_start == SIZE_MAX) return false;
    while (value_start < content_end &&
           (text[value_start] == ' ' || text[value_start] == '\t')) {
        ++value_start;
    }
    if (value_start >= content_end ||
        (text[value_start] != '|' && text[value_start] != '>')) {
        return false;
    }
    ++value_start;
    while (value_start < content_end &&
           (text[value_start] == '+' || text[value_start] == '-' ||
            isdigit((unsigned char)text[value_start]) != 0)) {
        ++value_start;
    }
    while (value_start < content_end &&
           (text[value_start] == ' ' || text[value_start] == '\t')) {
        ++value_start;
    }
    return value_start == content_end || text[value_start] == '#';
}

static OdStatus parse_compose_text(OdProjectDiscovery *result,
                                   const char *absolute_path,
                                   const char *relative_path,
                                   const char *text,
                                   size_t length,
                                   OdError *error) {
    uint64_t file_hash = hash_bytes(text, length);
    bool in_ports = false;
    size_t ports_indent = 0U;
    bool in_block_scalar = false;
    size_t block_scalar_indent = 0U;
    size_t line_number = 1U;
    size_t line_start = 0U;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1U] == '\r') --content_end;
        size_t cursor = line_start;
        while (cursor < content_end && text[cursor] == ' ') ++cursor;
        size_t indent = cursor - line_start;
        bool content = cursor < content_end && text[cursor] != '#';
        if (in_block_scalar) {
            if (!content || indent > block_scalar_indent) {
                line_start = line_end < length ? line_end + 1U : length;
                ++line_number;
                continue;
            }
            in_block_scalar = false;
        }
        if (content && compose_block_scalar_header(text, cursor, content_end)) {
            in_ports = false;
            in_block_scalar = true;
            block_scalar_indent = indent;
            line_start = line_end < length ? line_end + 1U : length;
            ++line_number;
            continue;
        }
        if (in_ports && content && indent <= ports_indent) in_ports = false;
        if (!in_ports && content && exact_ports_key(text, cursor, content_end)) {
            in_ports = true;
            ports_indent = indent;
        } else if (in_ports && content && indent > ports_indent) {
            size_t scalar_start = 0U;
            size_t scalar_end = 0U;
            bool malformed_quote = false;
            if (compose_list_scalar(text, cursor, content_end, &scalar_start,
                                    &scalar_end, &malformed_quote)) {
                ComposeMapping mapping;
                if (parse_compose_scalar(text + scalar_start,
                                         scalar_end - scalar_start, &mapping)) {
                    OdStatus status = append_compose_declaration(
                        result, absolute_path, relative_path, text + line_start,
                        content_end - line_start, text + scalar_start, scalar_start,
                        &mapping, line_number, line_start, length, file_hash, error);
                    if (status != OD_OK) return status;
                }
            }
        }
        line_start = line_end < length ? line_end + 1U : length;
        ++line_number;
    }
    return OD_OK;
}

typedef struct {
    char *bytes;
    size_t *source_offsets;
    size_t length;
    size_t capacity;
} JsonString;

typedef struct {
    const char *text;
    size_t length;
    size_t cursor;
    unsigned depth;
    OdProjectDiscovery *result;
    const char *absolute_path;
    const char *relative_path;
    uint64_t file_hash;
    OdStatus status;
    OdError *error;
} JsonParser;

static void json_string_free(JsonString *string) {
    if (string == NULL) return;
    free(string->bytes);
    free(string->source_offsets);
    *string = (JsonString){0};
}

static bool json_string_append(JsonString *string,
                               unsigned char byte,
                               size_t source_offset) {
    if (string->length == string->capacity) {
        size_t capacity = string->capacity == 0U ? 32U : string->capacity * 2U;
        if (capacity < string->capacity) return false;
        char *bytes = realloc(string->bytes, capacity + 1U);
        if (bytes == NULL) return false;
        string->bytes = bytes;
        size_t *offsets = realloc(string->source_offsets,
                                  capacity * sizeof(*offsets));
        if (offsets == NULL) return false;
        string->source_offsets = offsets;
        string->capacity = capacity;
    }
    string->bytes[string->length] = (char)byte;
    string->source_offsets[string->length++] = source_offset;
    string->bytes[string->length] = '\0';
    return true;
}

static void json_fail(JsonParser *parser, const char *message) {
    if (parser->status != OD_OK) return;
    parser->status = OD_ERROR_INVALID;
    od_error_set(parser->error, OD_ERROR_INVALID, "%s", message);
}

static void json_memory_fail(JsonParser *parser) {
    if (parser->status != OD_OK) return;
    parser->status = OD_ERROR_MEMORY;
    od_error_set(parser->error, OD_ERROR_MEMORY, "unable to parse package.json");
}

static void json_skip_space(JsonParser *parser) {
    while (parser->cursor < parser->length &&
           (parser->text[parser->cursor] == ' ' ||
            parser->text[parser->cursor] == '\t' ||
            parser->text[parser->cursor] == '\r' ||
            parser->text[parser->cursor] == '\n')) {
        ++parser->cursor;
    }
}

static int json_hex_digit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static bool json_hex_quad(JsonParser *parser, uint32_t *value) {
    if (parser->length - parser->cursor < 4U) return false;
    uint32_t result = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        int digit = json_hex_digit(parser->text[parser->cursor + index]);
        if (digit < 0) return false;
        result = result * 16U + (uint32_t)digit;
    }
    parser->cursor += 4U;
    *value = result;
    return true;
}

static bool json_append_codepoint(JsonString *string,
                                  uint32_t codepoint,
                                  size_t source_offset) {
    if (codepoint <= 0x7fU) {
        return json_string_append(string, (unsigned char)codepoint, source_offset);
    }
    if (codepoint <= 0x7ffU) {
        return json_string_append(string,
                                  (unsigned char)(0xc0U | (codepoint >> 6U)),
                                  source_offset) &&
               json_string_append(string,
                                  (unsigned char)(0x80U | (codepoint & 0x3fU)),
                                  source_offset);
    }
    if (codepoint <= 0xffffU) {
        return json_string_append(string,
                                  (unsigned char)(0xe0U | (codepoint >> 12U)),
                                  source_offset) &&
               json_string_append(string,
                                  (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3fU)),
                                  source_offset) &&
               json_string_append(string,
                                  (unsigned char)(0x80U | (codepoint & 0x3fU)),
                                  source_offset);
    }
    return json_string_append(string,
                              (unsigned char)(0xf0U | (codepoint >> 18U)),
                              source_offset) &&
           json_string_append(string,
                              (unsigned char)(0x80U | ((codepoint >> 12U) & 0x3fU)),
                              source_offset) &&
           json_string_append(string,
                              (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3fU)),
                              source_offset) &&
           json_string_append(string,
                              (unsigned char)(0x80U | (codepoint & 0x3fU)),
                              source_offset);
}

static bool json_parse_string(JsonParser *parser, JsonString *output) {
    *output = (JsonString){0};
    if (parser->cursor >= parser->length || parser->text[parser->cursor] != '"') {
        json_fail(parser, "expected JSON string");
        return false;
    }
    ++parser->cursor;
    while (parser->cursor < parser->length) {
        size_t source_offset = parser->cursor;
        unsigned char character = (unsigned char)parser->text[parser->cursor++];
        if (character == '"') return true;
        if (character < 0x20U) {
            json_fail(parser, "invalid control byte in JSON string");
            break;
        }
        if (character != '\\') {
            if (!json_string_append(output, character, source_offset)) {
                json_memory_fail(parser);
                break;
            }
            continue;
        }
        if (parser->cursor >= parser->length) {
            json_fail(parser, "unterminated JSON escape");
            break;
        }
        char escape = parser->text[parser->cursor++];
        unsigned char decoded = 0U;
        bool simple = true;
        switch (escape) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': simple = false; break;
            default:
                json_fail(parser, "invalid JSON escape");
                break;
        }
        if (parser->status != OD_OK) break;
        if (simple) {
            if (!json_string_append(output, decoded, source_offset)) {
                json_memory_fail(parser);
                break;
            }
            continue;
        }
        uint32_t codepoint = 0U;
        if (!json_hex_quad(parser, &codepoint)) {
            json_fail(parser, "invalid JSON unicode escape");
            break;
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
            if (parser->length - parser->cursor < 6U ||
                parser->text[parser->cursor] != '\\' ||
                parser->text[parser->cursor + 1U] != 'u') {
                json_fail(parser, "invalid JSON surrogate pair");
                break;
            }
            parser->cursor += 2U;
            uint32_t low = 0U;
            if (!json_hex_quad(parser, &low) || low < 0xdc00U || low > 0xdfffU) {
                json_fail(parser, "invalid JSON surrogate pair");
                break;
            }
            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                        (low - 0xdc00U);
        } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
            json_fail(parser, "invalid JSON surrogate pair");
            break;
        }
        if (!json_append_codepoint(output, codepoint, source_offset)) {
            json_memory_fail(parser);
            break;
        }
    }
    if (parser->status == OD_OK) json_fail(parser, "unterminated JSON string");
    json_string_free(output);
    return false;
}

static bool json_parse_value(JsonParser *parser);

static bool json_parse_number(JsonParser *parser) {
    size_t cursor = parser->cursor;
    if (cursor < parser->length && parser->text[cursor] == '-') ++cursor;
    if (cursor >= parser->length) return false;
    if (parser->text[cursor] == '0') {
        ++cursor;
    } else if (parser->text[cursor] >= '1' && parser->text[cursor] <= '9') {
        do {
            ++cursor;
        } while (cursor < parser->length &&
                 isdigit((unsigned char)parser->text[cursor]) != 0);
    } else {
        return false;
    }
    if (cursor < parser->length && parser->text[cursor] == '.') {
        ++cursor;
        size_t digits = cursor;
        while (cursor < parser->length &&
               isdigit((unsigned char)parser->text[cursor]) != 0) ++cursor;
        if (cursor == digits) return false;
    }
    if (cursor < parser->length &&
        (parser->text[cursor] == 'e' || parser->text[cursor] == 'E')) {
        ++cursor;
        if (cursor < parser->length &&
            (parser->text[cursor] == '+' || parser->text[cursor] == '-')) ++cursor;
        size_t digits = cursor;
        while (cursor < parser->length &&
               isdigit((unsigned char)parser->text[cursor]) != 0) ++cursor;
        if (cursor == digits) return false;
    }
    parser->cursor = cursor;
    return true;
}

static bool json_parse_array(JsonParser *parser) {
    ++parser->cursor;
    json_skip_space(parser);
    if (parser->cursor < parser->length && parser->text[parser->cursor] == ']') {
        ++parser->cursor;
        return true;
    }
    for (;;) {
        if (!json_parse_value(parser)) return false;
        json_skip_space(parser);
        if (parser->cursor < parser->length && parser->text[parser->cursor] == ']') {
            ++parser->cursor;
            return true;
        }
        if (parser->cursor >= parser->length || parser->text[parser->cursor] != ',') {
            json_fail(parser, "expected comma in JSON array");
            return false;
        }
        ++parser->cursor;
        json_skip_space(parser);
    }
}

static bool json_parse_object(JsonParser *parser) {
    ++parser->cursor;
    json_skip_space(parser);
    if (parser->cursor < parser->length && parser->text[parser->cursor] == '}') {
        ++parser->cursor;
        return true;
    }
    for (;;) {
        JsonString key;
        if (!json_parse_string(parser, &key)) return false;
        json_string_free(&key);
        json_skip_space(parser);
        if (parser->cursor >= parser->length || parser->text[parser->cursor] != ':') {
            json_fail(parser, "expected colon in JSON object");
            return false;
        }
        ++parser->cursor;
        json_skip_space(parser);
        if (!json_parse_value(parser)) return false;
        json_skip_space(parser);
        if (parser->cursor < parser->length && parser->text[parser->cursor] == '}') {
            ++parser->cursor;
            return true;
        }
        if (parser->cursor >= parser->length || parser->text[parser->cursor] != ',') {
            json_fail(parser, "expected comma in JSON object");
            return false;
        }
        ++parser->cursor;
        json_skip_space(parser);
    }
}

static bool json_parse_value(JsonParser *parser) {
    if (++parser->depth > 128U) {
        json_fail(parser, "package.json nesting is too deep");
        --parser->depth;
        return false;
    }
    json_skip_space(parser);
    bool valid = false;
    if (parser->cursor < parser->length) {
        char character = parser->text[parser->cursor];
        if (character == '"') {
            JsonString string;
            valid = json_parse_string(parser, &string);
            json_string_free(&string);
        } else if (character == '{') {
            valid = json_parse_object(parser);
        } else if (character == '[') {
            valid = json_parse_array(parser);
        } else if (character == '-' || isdigit((unsigned char)character) != 0) {
            valid = json_parse_number(parser);
        } else {
            static const char *const literals[] = {"true", "false", "null"};
            for (size_t index = 0U; index < 3U; ++index) {
                size_t literal_length = strlen(literals[index]);
                if (parser->length - parser->cursor >= literal_length &&
                    memcmp(parser->text + parser->cursor, literals[index],
                           literal_length) == 0) {
                    parser->cursor += literal_length;
                    valid = true;
                    break;
                }
            }
        }
    }
    if (!valid && parser->status == OD_OK) json_fail(parser, "invalid JSON value");
    --parser->depth;
    return valid;
}

static bool command_space(char character) {
    return character == ' ' || character == '\t' ||
           character == '\r' || character == '\n';
}

static bool command_horizontal_space(char character) {
    return character == ' ' || character == '\t';
}

static bool command_separator(char character) {
    return character == ';' || character == '|' || character == '&' ||
           character == '\r' || character == '\n';
}

static bool command_source_span_contiguous(const JsonString *command,
                                           size_t start,
                                           size_t end) {
    if (start >= end) return false;
    for (size_t index = start + 1U; index < end; ++index) {
        if (command->source_offsets[index] !=
            command->source_offsets[start] + index - start) {
            return false;
        }
    }
    return true;
}

static bool package_assignment_prefix_end(const JsonString *command,
                                          size_t *assignment_end) {
    size_t cursor = 0U;
    while (cursor < command->length &&
           command_horizontal_space(command->bytes[cursor])) {
        ++cursor;
    }
    bool found_assignment = false;
    while (cursor < command->length) {
        if (!identifier_start((unsigned char)command->bytes[cursor])) {
            return found_assignment && !command_separator(command->bytes[cursor]);
        }
        ++cursor;
        while (cursor < command->length &&
               identifier_continue((unsigned char)command->bytes[cursor])) {
            ++cursor;
        }
        if (cursor >= command->length || command->bytes[cursor] != '=') {
            return found_assignment;
        }
        ++cursor;
        while (cursor < command->length && !command_space(command->bytes[cursor])) {
            if (command_separator(command->bytes[cursor])) break;
            ++cursor;
        }
        found_assignment = true;
        *assignment_end = cursor;
        if (cursor >= command->length || command_separator(command->bytes[cursor])) {
            return false;
        }
        while (cursor < command->length &&
               command_horizontal_space(command->bytes[cursor])) {
            ++cursor;
        }
        if (cursor >= command->length) return false;
    }
    return false;
}

static bool scan_package_assignment_prefix(JsonParser *parser,
                                           const JsonString *command) {
    size_t assignment_end = 0U;
    if (!package_assignment_prefix_end(command, &assignment_end)) return true;
    size_t cursor = 0U;
    while (cursor < assignment_end &&
           command_horizontal_space(command->bytes[cursor])) {
        ++cursor;
    }
    while (cursor < assignment_end) {
        size_t key_start = cursor++;
        while (cursor < assignment_end &&
               identifier_continue((unsigned char)command->bytes[cursor])) {
            ++cursor;
        }
        size_t key_end = cursor;
        ++cursor;
        size_t value_start = cursor;
        while (cursor < assignment_end &&
               !command_horizontal_space(command->bytes[cursor])) {
            ++cursor;
        }
        size_t value_end = cursor;
        uint16_t port = 0U;
        if (port_key_segment(command->bytes + key_start, key_end - key_start) &&
            parse_port_number(command->bytes + value_start,
                              value_end - value_start, &port) &&
            command_source_span_contiguous(command, value_start, value_end)) {
            OdStatus status = append_manual_declaration(
                parser->result, OD_SOURCE_PACKAGE_JSON, OD_DECLARATION_LITERAL,
                parser->absolute_path, parser->relative_path, parser->text,
                parser->length, parser->file_hash, port,
                command->bytes + key_start, key_end - key_start,
                command->source_offsets[value_start], value_end - value_start,
                "package.json scripts are manual-only", parser->error);
            if (status != OD_OK) {
                parser->status = status;
                return false;
            }
        }
        while (cursor < assignment_end &&
               command_horizontal_space(command->bytes[cursor])) {
            ++cursor;
        }
    }
    return true;
}

static bool scan_package_command(JsonParser *parser, const JsonString *command) {
    if (!scan_package_assignment_prefix(parser, command)) return false;
    size_t cursor = 0U;
    while (cursor < command->length) {
        size_t flag_length = 0U;
        if ((cursor == 0U || command_space(command->bytes[cursor - 1U])) &&
            command->length - cursor >= 6U &&
            memcmp(command->bytes + cursor, "--port", 6U) == 0) {
            flag_length = 6U;
        } else if ((cursor == 0U || command_space(command->bytes[cursor - 1U])) &&
                   command->length - cursor >= 2U &&
                   memcmp(command->bytes + cursor, "-p", 2U) == 0) {
            flag_length = 2U;
        }
        if (flag_length == 0U || cursor + flag_length >= command->length ||
            !command_space(command->bytes[cursor + flag_length])) {
            ++cursor;
            continue;
        }
        size_t number_start = cursor + flag_length;
        while (number_start < command->length &&
               command_space(command->bytes[number_start])) ++number_start;
        size_t number_end = number_start;
        while (number_end < command->length &&
               isdigit((unsigned char)command->bytes[number_end]) != 0) ++number_end;
        uint16_t port = 0U;
        bool boundary = number_end == command->length ||
                        command_space(command->bytes[number_end]);
        bool contiguous = command_source_span_contiguous(command, number_start,
                                                         number_end);
        if (boundary && contiguous &&
            parse_port_number(command->bytes + number_start,
                              number_end - number_start, &port)) {
            OdStatus status = append_manual_declaration(
                parser->result, OD_SOURCE_PACKAGE_JSON, OD_DECLARATION_LITERAL,
                parser->absolute_path, parser->relative_path, parser->text,
                parser->length, parser->file_hash, port, NULL, 0U,
                command->source_offsets[number_start], number_end - number_start,
                "package.json scripts are manual-only", parser->error);
            if (status != OD_OK) {
                parser->status = status;
                return false;
            }
        }
        cursor = number_end > cursor ? number_end : cursor + 1U;
    }
    return true;
}

static bool json_parse_scripts_object(JsonParser *parser) {
    if (parser->cursor >= parser->length || parser->text[parser->cursor] != '{') {
        return json_parse_value(parser);
    }
    ++parser->cursor;
    json_skip_space(parser);
    if (parser->cursor < parser->length && parser->text[parser->cursor] == '}') {
        ++parser->cursor;
        return true;
    }
    for (;;) {
        JsonString key;
        if (!json_parse_string(parser, &key)) return false;
        json_string_free(&key);
        json_skip_space(parser);
        if (parser->cursor >= parser->length || parser->text[parser->cursor] != ':') {
            json_fail(parser, "expected colon in package scripts");
            return false;
        }
        ++parser->cursor;
        json_skip_space(parser);
        if (parser->cursor < parser->length && parser->text[parser->cursor] == '"') {
            JsonString command;
            if (!json_parse_string(parser, &command)) return false;
            bool scanned = scan_package_command(parser, &command);
            json_string_free(&command);
            if (!scanned) return false;
        } else if (!json_parse_value(parser)) {
            return false;
        }
        json_skip_space(parser);
        if (parser->cursor < parser->length && parser->text[parser->cursor] == '}') {
            ++parser->cursor;
            return true;
        }
        if (parser->cursor >= parser->length || parser->text[parser->cursor] != ',') {
            json_fail(parser, "expected comma in package scripts");
            return false;
        }
        ++parser->cursor;
        json_skip_space(parser);
    }
}

static OdStatus parse_package_text(OdProjectDiscovery *result,
                                   const char *absolute_path,
                                   const char *relative_path,
                                   const char *text,
                                   size_t length,
                                   OdError *error) {
    JsonParser parser = {
        .text = text,
        .length = length,
        .result = result,
        .absolute_path = absolute_path,
        .relative_path = relative_path,
        .file_hash = hash_bytes(text, length),
        .status = OD_OK,
        .error = error
    };
    json_skip_space(&parser);
    if (parser.cursor >= parser.length || parser.text[parser.cursor] != '{') {
        json_fail(&parser, "package.json root must be an object");
        return parser.status;
    }
    ++parser.cursor;
    json_skip_space(&parser);
    if (parser.cursor < parser.length && parser.text[parser.cursor] == '}') {
        ++parser.cursor;
    } else {
        for (;;) {
            JsonString key;
            if (!json_parse_string(&parser, &key)) break;
            bool scripts = key.length == 7U &&
                           memcmp(key.bytes, "scripts", 7U) == 0;
            json_string_free(&key);
            json_skip_space(&parser);
            if (parser.cursor >= parser.length || parser.text[parser.cursor] != ':') {
                json_fail(&parser, "expected colon in package.json root");
                break;
            }
            ++parser.cursor;
            json_skip_space(&parser);
            bool valid = scripts ? json_parse_scripts_object(&parser) :
                                   json_parse_value(&parser);
            if (!valid) break;
            json_skip_space(&parser);
            if (parser.cursor < parser.length && parser.text[parser.cursor] == '}') {
                ++parser.cursor;
                break;
            }
            if (parser.cursor >= parser.length || parser.text[parser.cursor] != ',') {
                json_fail(&parser, "expected comma in package.json root");
                break;
            }
            ++parser.cursor;
            json_skip_space(&parser);
        }
    }
    json_skip_space(&parser);
    if (parser.status == OD_OK && parser.cursor != parser.length) {
        json_fail(&parser, "trailing content in package.json");
    }
    return parser.status;
}

static OdStatus read_regular_file(const char *path,
                                  char **text,
                                  size_t *length,
                                  OdError *error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to open file: %s", strerror(errno));
        return OD_ERROR_IO;
    }
    struct stat information;
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0 ||
        (uintmax_t)information.st_size > OD_DISCOVERY_FILE_MAX) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_INVALID, "file is not a bounded regular file");
        return OD_ERROR_INVALID;
    }
    size_t size = (size_t)information.st_size;
    char *buffer = malloc(size + 1U);
    if (buffer == NULL) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read discovery file");
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    while (used < size) {
        ssize_t count = read(descriptor, buffer + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(buffer);
            (void)close(descriptor);
            od_error_set(error, OD_ERROR_IO, "unable to read discovery file");
            return OD_ERROR_IO;
        }
        used += (size_t)count;
    }
    if (close(descriptor) != 0) {
        free(buffer);
        od_error_set(error, OD_ERROR_IO, "unable to close discovery file");
        return OD_ERROR_IO;
    }
    buffer[size] = '\0';
    *text = buffer;
    *length = size;
    return OD_OK;
}

static OdStatus validate_env_text(const char *text, size_t length, OdError *error) {
    size_t line_start = 0U;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1U] == '\r') --content_end;
        size_t cursor = line_start;
        while (cursor < content_end &&
               (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
        if (cursor < content_end && text[cursor] != '#' &&
            identifier_start((unsigned char)text[cursor])) {
            ++cursor;
            while (cursor < content_end &&
                   identifier_continue((unsigned char)text[cursor])) ++cursor;
            while (cursor < content_end &&
                   (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
            if (cursor < content_end && text[cursor] == '=') {
                ++cursor;
                while (cursor < content_end &&
                       (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                if (cursor < content_end &&
                    (text[cursor] == '\'' || text[cursor] == '"')) {
                    char quote = text[cursor++];
                    while (cursor < content_end && text[cursor] != quote) ++cursor;
                    if (cursor >= content_end) {
                        od_error_set(error, OD_ERROR_INVALID,
                                     "unterminated quoted .env value");
                        return OD_ERROR_INVALID;
                    }
                }
            }
        }
        line_start = line_end < length ? line_end + 1U : length;
    }
    od_error_clear(error);
    return OD_OK;
}

static OdStatus validate_compose_text(const char *text,
                                      size_t length,
                                      OdError *error) {
    bool in_ports = false;
    size_t ports_indent = 0U;
    bool in_block_scalar = false;
    size_t block_scalar_indent = 0U;
    size_t line_start = 0U;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1U] == '\r') --content_end;
        size_t cursor = line_start;
        while (cursor < content_end && text[cursor] == ' ') ++cursor;
        bool has_tab_indent = cursor < content_end && text[cursor] == '\t';
        size_t indent = cursor - line_start;
        bool content = cursor < content_end && text[cursor] != '#';
        if (in_block_scalar) {
            if (!content || indent > block_scalar_indent) {
                line_start = line_end < length ? line_end + 1U : length;
                continue;
            }
            in_block_scalar = false;
        }
        if (content && compose_block_scalar_header(text, cursor, content_end)) {
            in_ports = false;
            in_block_scalar = true;
            block_scalar_indent = indent;
            line_start = line_end < length ? line_end + 1U : length;
            continue;
        }
        if (in_ports && has_tab_indent) {
            od_error_set(error, OD_ERROR_INVALID,
                         "tabs are not supported in Compose ports indentation");
            return OD_ERROR_INVALID;
        }
        if (in_ports && content && indent <= ports_indent) {
            if (text[cursor] == '-') {
                od_error_set(error, OD_ERROR_INVALID,
                             "Compose ports entries must be indented");
                return OD_ERROR_INVALID;
            }
            in_ports = false;
        }
        if (!in_ports && content && exact_ports_key(text, cursor, content_end)) {
            in_ports = true;
            ports_indent = indent;
        } else if (in_ports && content && indent > ports_indent &&
                   text[cursor] == '-') {
            size_t scalar_start = 0U;
            size_t scalar_end = 0U;
            bool malformed_quote = false;
            (void)compose_list_scalar(text, cursor, content_end, &scalar_start,
                                      &scalar_end, &malformed_quote);
            if (malformed_quote) {
                od_error_set(error, OD_ERROR_INVALID,
                             "unterminated quoted Compose ports mapping");
                return OD_ERROR_INVALID;
            }
        }
        line_start = line_end < length ? line_end + 1U : length;
    }
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_validate_discovery_text(OdPortSourceKind source_kind,
                                    const char *text,
                                    size_t length,
                                    OdError *error) {
    if (text == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "discovery text is required");
        return OD_ERROR_INVALID;
    }
    switch (source_kind) {
        case OD_SOURCE_ENV:
            return validate_env_text(text, length, error);
        case OD_SOURCE_COMPOSE:
            return validate_compose_text(text, length, error);
        case OD_SOURCE_PACKAGE_JSON:
        case OD_SOURCE_MAKEFILE:
            od_error_set(error, OD_ERROR_UNSUPPORTED,
                         "validation is only supported for .env and Compose files");
            return OD_ERROR_UNSUPPORTED;
    }
    od_error_set(error, OD_ERROR_UNSUPPORTED, "unsupported discovery source");
    return OD_ERROR_UNSUPPORTED;
}

static OdStatus inspect_env_file(OdProjectDiscovery *result,
                                 const char *absolute_path,
                                 const char *relative_path,
                                 OdError *error) {
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(absolute_path, &text, &length, error);
    if (status != OD_OK) {
        char detail[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(detail, sizeof(detail), "%s", error->message);
        return add_warning(result, relative_path, detail, error);
    }
    status = parse_env_text(result, absolute_path, relative_path,
                            text, length, error);
    free(text);
    return status;
}

static OdStatus inspect_compose_file(OdProjectDiscovery *result,
                                     const char *absolute_path,
                                     const char *relative_path,
                                     OdError *error) {
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(absolute_path, &text, &length, error);
    if (status == OD_OK) {
        status = validate_compose_text(text, length, error);
    }
    if (status != OD_OK) {
        char detail[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(detail, sizeof(detail), "%s", error->message);
        free(text);
        return add_warning(result, relative_path, detail, error);
    }
    status = parse_compose_text(result, absolute_path, relative_path,
                                text, length, error);
    free(text);
    return status;
}

static OdStatus inspect_package_file(OdProjectDiscovery *result,
                                     const char *absolute_path,
                                     const char *relative_path,
                                     OdError *error) {
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(absolute_path, &text, &length, error);
    if (status != OD_OK) {
        char detail[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(detail, sizeof(detail), "%s", error->message);
        return add_warning(result, relative_path, detail, error);
    }
    size_t initial_count = result->count;
    status = parse_package_text(result, absolute_path, relative_path,
                                text, length, error);
    if (status == OD_ERROR_INVALID) {
        for (size_t index = initial_count; index < result->count; ++index) {
            declaration_free(&result->items[index]);
        }
        result->count = initial_count;
        char detail[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(detail, sizeof(detail), "%s", error->message);
        free(text);
        return add_warning(result, relative_path, detail, error);
    }
    free(text);
    return status;
}

static bool parse_direct_reference(const char *text,
                                   size_t start,
                                   size_t end,
                                   size_t *key_start,
                                   size_t *key_length) {
    if (start >= end || text[start] != '$') return false;
    size_t cursor = start + 1U;
    bool braced = cursor < end && text[cursor] == '{';
    if (braced) ++cursor;
    if (cursor >= end || !identifier_start((unsigned char)text[cursor])) return false;
    *key_start = cursor++;
    while (cursor < end && identifier_continue((unsigned char)text[cursor])) ++cursor;
    *key_length = cursor - *key_start;
    if (braced) {
        if (cursor >= end || text[cursor] != '}') return false;
        ++cursor;
    }
    return cursor == end;
}

static OdStatus append_make_reference(OdProjectDiscovery *result,
                                      const char *absolute_path,
                                      const char *relative_path,
                                      const char *text,
                                      size_t length,
                                      uint64_t file_hash,
                                      size_t token_start,
                                      size_t token_end,
                                      size_t key_start,
                                      size_t key_length,
                                      OdError *error) {
    return append_manual_declaration(
        result, OD_SOURCE_MAKEFILE, OD_DECLARATION_ENV_REFERENCE,
        absolute_path, relative_path, text, length, file_hash, 0U,
        text + key_start, key_length, token_start, token_end - token_start,
        "Makefile environment reference is manual-only", error);
}

static OdStatus parse_make_text(OdProjectDiscovery *result,
                                const char *absolute_path,
                                const char *relative_path,
                                const char *text,
                                size_t length,
                                OdError *error) {
    uint64_t file_hash = hash_bytes(text, length);
    size_t line_start = 0U;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > line_start && text[content_end - 1U] == '\r') --content_end;
        for (size_t index = line_start; index < content_end; ++index) {
            if (text[index] == '#') {
                content_end = index;
                break;
            }
        }
        while (content_end > line_start &&
               (text[content_end - 1U] == ' ' || text[content_end - 1U] == '\t')) {
            --content_end;
        }
        size_t cursor = line_start;
        while (cursor < content_end &&
               (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
        if (cursor < content_end && identifier_start((unsigned char)text[cursor])) {
            size_t declaration_key_start = cursor++;
            while (cursor < content_end &&
                   identifier_continue((unsigned char)text[cursor])) ++cursor;
            size_t declaration_key_end = cursor;
            while (cursor < content_end &&
                   (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
            size_t operator_length = 0U;
            if (cursor < content_end && text[cursor] == '=') {
                operator_length = 1U;
            } else if (cursor + 1U < content_end &&
                       (text[cursor] == '?' || text[cursor] == ':') &&
                       text[cursor + 1U] == '=') {
                operator_length = 2U;
            }
            if (operator_length > 0U &&
                port_key_segment(text + declaration_key_start,
                                 declaration_key_end - declaration_key_start)) {
                cursor += operator_length;
                while (cursor < content_end &&
                       (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                size_t value_start = cursor;
                size_t value_end = content_end;
                while (value_end > value_start &&
                       (text[value_end - 1U] == ' ' || text[value_end - 1U] == '\t')) {
                    --value_end;
                }
                uint16_t port = 0U;
                if (parse_port_number(text + value_start, value_end - value_start,
                                      &port)) {
                    OdStatus status = append_manual_declaration(
                        result, OD_SOURCE_MAKEFILE, OD_DECLARATION_LITERAL,
                        absolute_path, relative_path, text, length, file_hash,
                        port, text + declaration_key_start,
                        declaration_key_end - declaration_key_start,
                        value_start, value_end - value_start,
                        "Makefile declarations are manual-only", error);
                    if (status != OD_OK) return status;
                } else {
                    size_t key_start = 0U;
                    size_t key_length = 0U;
                    if (parse_direct_reference(text, value_start, value_end,
                                               &key_start, &key_length)) {
                        OdStatus status = append_make_reference(
                            result, absolute_path, relative_path, text, length,
                            file_hash, value_start, value_end, key_start,
                            key_length, error);
                        if (status != OD_OK) return status;
                    }
                }
            }
        }
        cursor = line_start;
        while (cursor < content_end) {
            size_t flag_length = 0U;
            bool boundary = cursor == line_start || text[cursor - 1U] == ' ' ||
                            text[cursor - 1U] == '\t';
            if (boundary && content_end - cursor >= 6U &&
                memcmp(text + cursor, "--port", 6U) == 0) {
                flag_length = 6U;
            } else if (boundary && content_end - cursor >= 2U &&
                       memcmp(text + cursor, "-p", 2U) == 0) {
                flag_length = 2U;
            }
            if (flag_length == 0U || cursor + flag_length >= content_end ||
                (text[cursor + flag_length] != ' ' &&
                 text[cursor + flag_length] != '\t')) {
                ++cursor;
                continue;
            }
            size_t value_start = cursor + flag_length;
            while (value_start < content_end &&
                   (text[value_start] == ' ' || text[value_start] == '\t')) {
                ++value_start;
            }
            size_t value_end = value_start;
            while (value_end < content_end && text[value_end] != ' ' &&
                   text[value_end] != '\t') ++value_end;
            uint16_t port = 0U;
            if (parse_port_number(text + value_start, value_end - value_start,
                                  &port)) {
                OdStatus status = append_manual_declaration(
                    result, OD_SOURCE_MAKEFILE, OD_DECLARATION_LITERAL,
                    absolute_path, relative_path, text, length, file_hash,
                    port, NULL, 0U, value_start, value_end - value_start,
                    "Makefile command flags are manual-only", error);
                if (status != OD_OK) return status;
            } else {
                size_t key_start = 0U;
                size_t key_length = 0U;
                if (parse_direct_reference(text, value_start, value_end,
                                           &key_start, &key_length)) {
                    OdStatus status = append_make_reference(
                        result, absolute_path, relative_path, text, length,
                        file_hash, value_start, value_end, key_start,
                        key_length, error);
                    if (status != OD_OK) return status;
                }
            }
            cursor = value_end > cursor ? value_end : cursor + 1U;
        }
        line_start = line_end < length ? line_end + 1U : length;
    }
    return OD_OK;
}

static OdStatus inspect_make_file(OdProjectDiscovery *result,
                                  const char *absolute_path,
                                  const char *relative_path,
                                  OdError *error) {
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(absolute_path, &text, &length, error);
    if (status != OD_OK) {
        char detail[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(detail, sizeof(detail), "%s", error->message);
        return add_warning(result, relative_path, detail, error);
    }
    status = parse_make_text(result, absolute_path, relative_path,
                             text, length, error);
    free(text);
    return status;
}

static OdStatus walk_project(OdProjectDiscovery *result,
                             const char *root,
                             const char *relative_directory,
                             OdError *error) {
    char *absolute_directory = relative_directory[0] == '\0' ?
        copy_string(root) : joined_path(root, relative_directory);
    if (absolute_directory == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to build discovery directory path");
        return OD_ERROR_MEMORY;
    }
    DIR *directory = opendir(absolute_directory);
    if (directory == NULL) {
        OdStatus warning_status = add_warning(result, relative_directory[0] == '\0' ?
                                               "." : relative_directory,
                                               strerror(errno), error);
        free(absolute_directory);
        return warning_status;
    }
    OdStatus status = OD_OK;
    struct dirent *entry;
    while (status == OD_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *relative_path = relative_directory[0] == '\0' ?
            copy_string(entry->d_name) : joined_path(relative_directory, entry->d_name);
        char *absolute_path = relative_path == NULL ? NULL : joined_path(root, relative_path);
        if (relative_path == NULL || absolute_path == NULL) {
            free(relative_path);
            free(absolute_path);
            od_error_set(error, OD_ERROR_MEMORY, "unable to build discovery path");
            status = OD_ERROR_MEMORY;
            break;
        }
        struct stat information;
        if (lstat(absolute_path, &information) != 0) {
            status = add_warning(result, relative_path, strerror(errno), error);
        } else if (S_ISDIR(information.st_mode) && !skipped_directory(entry->d_name)) {
            status = walk_project(result, root, relative_path, error);
        } else if (S_ISREG(information.st_mode) && env_filename(entry->d_name)) {
            status = inspect_env_file(result, absolute_path, relative_path, error);
        } else if (S_ISREG(information.st_mode) && compose_filename(entry->d_name)) {
            status = inspect_compose_file(result, absolute_path, relative_path, error);
        } else if (S_ISREG(information.st_mode) &&
                   strcmp(entry->d_name, "package.json") == 0) {
            status = inspect_package_file(result, absolute_path, relative_path, error);
        } else if (S_ISREG(information.st_mode) &&
                   strcmp(entry->d_name, "Makefile") == 0) {
            status = inspect_make_file(result, absolute_path, relative_path, error);
        }
        free(relative_path);
        free(absolute_path);
    }
    if (closedir(directory) != 0 && status == OD_OK) {
        od_error_set(error, OD_ERROR_IO, "unable to close discovery directory");
        status = OD_ERROR_IO;
    }
    free(absolute_directory);
    return status;
}

static int compare_declarations(const void *left, const void *right) {
    const OdPortDeclaration *first = left;
    const OdPortDeclaration *second = right;
    int path_order = strcmp(first->relative_path, second->relative_path);
    if (path_order != 0) return path_order;
    if (first->line < second->line) return -1;
    if (first->line > second->line) return 1;
    if (first->port < second->port) return -1;
    if (first->port > second->port) return 1;
    return 0;
}

static bool matching_env_assignment(const OdEnvironmentAssignment *assignment,
                                    const OdPortDeclaration *reference) {
    return assignment->environment_key != NULL &&
           reference->environment_key != NULL &&
           strcmp(assignment->relative_folder, reference->relative_folder) == 0 &&
           strcmp(assignment->environment_key, reference->environment_key) == 0;
}

static size_t find_direct_environment_declaration(
    const OdProjectDiscovery *result,
    const OdEnvironmentAssignment *assignment) {
    for (size_t index = 0U; index < result->count; ++index) {
        const OdPortDeclaration *declaration = &result->items[index];
        if (declaration->source_kind == OD_SOURCE_ENV &&
            declaration->environment_key != NULL &&
            strcmp(declaration->relative_path, assignment->relative_path) == 0 &&
            strcmp(declaration->environment_key,
                   assignment->environment_key) == 0 &&
            declaration->byte_offset == assignment->value_offset) {
            return index;
        }
    }
    return SIZE_MAX;
}

static OdStatus resolve_environment_references(OdProjectDiscovery *result,
                                               OdError *error) {
    for (size_t reference_index = 0U; reference_index < result->count;
         ++reference_index) {
        OdPortDeclaration *reference = &result->items[reference_index];
        if ((reference->source_kind != OD_SOURCE_COMPOSE &&
             reference->source_kind != OD_SOURCE_MAKEFILE) ||
            reference->declaration_kind != OD_DECLARATION_ENV_REFERENCE ||
            reference->environment_key == NULL ||
            reference->environment_key[0] == '\0') {
            continue;
        }
        size_t folder_length = strlen(reference->relative_folder);
        size_t exact_length = folder_length +
            (strcmp(reference->relative_folder, "./") == 0 ? 5U : 6U);
        char *exact_path = malloc(exact_length);
        if (exact_path == NULL) {
            od_error_set(error, OD_ERROR_MEMORY,
                         "unable to resolve Compose environment reference");
            return OD_ERROR_MEMORY;
        }
        int written;
        if (strcmp(reference->relative_folder, "./") == 0) {
            written = snprintf(exact_path, exact_length, "./.env");
        } else {
            written = snprintf(exact_path, exact_length, "%s/.env",
                               reference->relative_folder);
        }
        if (written < 0 || (size_t)written >= exact_length) {
            free(exact_path);
            od_error_set(error, OD_ERROR_MEMORY,
                         "unable to build sibling .env path");
            return OD_ERROR_MEMORY;
        }
        size_t exact_count = 0U;
        const OdEnvironmentAssignment *exact_assignment = NULL;
        size_t variant_count = 0U;
        const OdEnvironmentAssignment *variant_assignment = NULL;
        for (size_t assignment_index = 0U;
             assignment_index < result->environment_assignment_count;
             ++assignment_index) {
            const OdEnvironmentAssignment *assignment =
                &result->environment_assignments[assignment_index];
            if (!matching_env_assignment(assignment, reference)) continue;
            if (strcmp(assignment->relative_path, exact_path) == 0) {
                ++exact_count;
                exact_assignment = assignment;
            } else {
                ++variant_count;
                variant_assignment = assignment;
            }
        }
        free(exact_path);
        const OdEnvironmentAssignment *selected_assignment = NULL;
        if (exact_count == 1U && exact_assignment->direct_port) {
            selected_assignment = exact_assignment;
        } else if (exact_count == 0U && variant_count == 1U &&
                   variant_assignment->direct_port) {
            selected_assignment = variant_assignment;
        }
        size_t selected = selected_assignment == NULL ? SIZE_MAX :
            find_direct_environment_declaration(result, selected_assignment);
        const char *reason = "environment reference has no unique direct sibling definition";
        if (selected != SIZE_MAX) {
            reference->definition_index = selected;
            reference->port = result->items[selected].port;
            reference->declaration_kind = OD_DECLARATION_ENV_REFERENCE;
            reason = "environment reference is manual-only; change its direct .env definition";
        } else {
            reference->definition_index = SIZE_MAX;
            reference->declaration_kind = OD_DECLARATION_UNRESOLVABLE;
        }
        char *replacement_reason = copy_string(reason);
        if (replacement_reason == NULL) {
            od_error_set(error, OD_ERROR_MEMORY,
                         "unable to store environment reference result");
            return OD_ERROR_MEMORY;
        }
        free(reference->manual_reason);
        reference->manual_reason = replacement_reason;
    }
    return OD_OK;
}

static OdStatus prune_unreferenced_non_port_env_literals(
    OdProjectDiscovery *result,
    OdError *error) {
    if (result->count == 0U) return OD_OK;
    bool *referenced = calloc(result->count, sizeof(*referenced));
    size_t *new_index = malloc(result->count * sizeof(*new_index));
    if (referenced == NULL || new_index == NULL) {
        free(referenced);
        free(new_index);
        od_error_set(error, OD_ERROR_MEMORY,
                     "unable to filter environment declarations");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < result->count; ++index) {
        new_index[index] = SIZE_MAX;
        size_t definition = result->items[index].definition_index;
        if (definition < result->count) referenced[definition] = true;
    }

    size_t write_index = 0U;
    for (size_t read_index = 0U; read_index < result->count; ++read_index) {
        OdPortDeclaration *declaration = &result->items[read_index];
        bool named_port = declaration->environment_key != NULL &&
            port_key_segment(declaration->environment_key,
                             strlen(declaration->environment_key));
        bool keep = declaration->source_kind != OD_SOURCE_ENV || named_port ||
                    (declaration->environment_key != NULL &&
                     address_key_allowlisted(declaration->environment_key,
                                             strlen(declaration->environment_key))) ||
                    referenced[read_index];
        if (!keep) {
            declaration_free(declaration);
            continue;
        }
        new_index[read_index] = write_index;
        if (write_index != read_index) {
            result->items[write_index] = *declaration;
            *declaration = (OdPortDeclaration){0};
        }
        ++write_index;
    }
    result->count = write_index;
    for (size_t index = 0U; index < result->count; ++index) {
        size_t definition = result->items[index].definition_index;
        if (definition != SIZE_MAX) result->items[index].definition_index = new_index[definition];
    }
    free(referenced);
    free(new_index);
    return OD_OK;
}

OdStatus od_discover_project_ports(const char *project_root,
                                   OdProjectDiscovery *result,
                                   OdError *error) {
    if (project_root == NULL || result == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "project root and discovery output are required");
        return OD_ERROR_INVALID;
    }
    *result = (OdProjectDiscovery){0};
    char *canonical_root = realpath(project_root, NULL);
    if (canonical_root == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "unable to resolve project root: %s",
                     strerror(errno));
        return OD_ERROR_INVALID;
    }
    struct stat information;
    if (stat(canonical_root, &information) != 0 || !S_ISDIR(information.st_mode)) {
        free(canonical_root);
        od_error_set(error, OD_ERROR_INVALID, "project root is not a directory");
        return OD_ERROR_INVALID;
    }
    OdStatus status = walk_project(result, canonical_root, "", error);
    free(canonical_root);
    if (status != OD_OK) {
        od_project_discovery_free(result);
        return status;
    }
    if (result->count > 1U) {
        qsort(result->items, result->count, sizeof(*result->items),
              compare_declarations);
    }
    status = resolve_environment_references(result, error);
    if (status != OD_OK) {
        od_project_discovery_free(result);
        return status;
    }
    status = prune_unreferenced_non_port_env_literals(result, error);
    if (status != OD_OK) {
        od_project_discovery_free(result);
        return status;
    }
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_discover_occupied_ports(const OdScanSnapshot *snapshot,
                                    uint16_t **ports,
                                    size_t *count,
                                    OdError *error) {
    if (snapshot == NULL || ports == NULL || count == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "scan snapshot and port output are required");
        return OD_ERROR_INVALID;
    }
    *ports = NULL;
    *count = 0U;
    bool *present = calloc(OD_PORT_COUNT, sizeof(*present));
    if (present == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to index occupied ports");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        uint16_t port = snapshot->endpoints[index].local_port;
        if (port != 0U && !present[port]) {
            present[port] = true;
            ++*count;
        }
    }
    if (*count > 0U) {
        *ports = malloc(*count * sizeof(**ports));
        if (*ports == NULL) {
            free(present);
            *count = 0U;
            od_error_set(error, OD_ERROR_MEMORY, "unable to store occupied ports");
            return OD_ERROR_MEMORY;
        }
    }
    size_t output = 0U;
    for (unsigned port = 1U; port <= 65535U; ++port) {
        if (present[port]) (*ports)[output++] = (uint16_t)port;
    }
    free(present);
    od_error_clear(error);
    return OD_OK;
}
