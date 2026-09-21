#include "opendoor/config.h"

#include "tomlc17.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OD_CONFIG_MAX_BYTES (4U * 1024U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuilder;

static char *copy_string(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static OdStatus builder_reserve(TextBuilder *builder, size_t extra, OdError *error) {
    if (extra > SIZE_MAX - builder->length - 1U) {
        od_error_set(error, OD_ERROR_MEMORY, "rendered configuration is too large");
        return OD_ERROR_MEMORY;
    }
    size_t required = builder->length + extra + 1U;
    if (required <= builder->capacity) {
        return OD_OK;
    }
    size_t capacity = builder->capacity == 0U ? 512U : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }
    char *data = realloc(builder->data, capacity);
    if (data == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to render configuration");
        return OD_ERROR_MEMORY;
    }
    builder->data = data;
    builder->capacity = capacity;
    return OD_OK;
}

static OdStatus builder_append(TextBuilder *builder, const char *value, OdError *error) {
    size_t length = strlen(value);
    OdStatus status = builder_reserve(builder, length, error);
    if (status != OD_OK) {
        return status;
    }
    memcpy(builder->data + builder->length, value, length + 1U);
    builder->length += length;
    return OD_OK;
}

static OdStatus builder_appendf(TextBuilder *builder, OdError *error, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list measured;
    va_copy(measured, arguments);
    int count = vsnprintf(NULL, 0U, format, measured);
    va_end(measured);
    if (count < 0) {
        va_end(arguments);
        od_error_set(error, OD_ERROR_INVALID, "unable to format configuration");
        return OD_ERROR_INVALID;
    }
    OdStatus status = builder_reserve(builder, (size_t)count, error);
    if (status == OD_OK) {
        (void)vsnprintf(builder->data + builder->length,
                        builder->capacity - builder->length,
                        format,
                        arguments);
        builder->length += (size_t)count;
    }
    va_end(arguments);
    return status;
}

static OdStatus builder_append_quoted(TextBuilder *builder, const char *value, OdError *error) {
    OdStatus status = builder_append(builder, "\"", error);
    for (size_t index = 0U; status == OD_OK && value[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character == '"') {
            status = builder_append(builder, "\\\"", error);
        } else if (character == '\\') {
            status = builder_append(builder, "\\\\", error);
        } else if (character == '\n') {
            status = builder_append(builder, "\\n", error);
        } else if (character == '\r') {
            status = builder_append(builder, "\\r", error);
        } else if (character == '\t') {
            status = builder_append(builder, "\\t", error);
        } else if (character < 0x20U || character == 0x7fU) {
            status = builder_appendf(builder, error, "\\u%04x", (unsigned)character);
        } else {
            char bytes[2] = {(char)character, '\0'};
            status = builder_append(builder, bytes, error);
        }
    }
    if (status == OD_OK) {
        status = builder_append(builder, "\"", error);
    }
    return status;
}

static void string_list_free(OdStringList *list) {
    for (size_t index = 0U; index < list->count; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    *list = (OdStringList){0};
}

static OdStatus parse_string_list(toml_datum_t table,
                                  const char *key,
                                  OdStringList *list,
                                  OdError *error) {
    *list = (OdStringList){0};
    toml_datum_t datum = toml_get(table, key);
    if (datum.type == TOML_UNKNOWN) {
        return OD_OK;
    }
    if (datum.type != TOML_ARRAY || datum.u.arr.size < 0) {
        od_error_set(error, OD_ERROR_INVALID, "%s must be an array of strings", key);
        return OD_ERROR_INVALID;
    }
    size_t count = (size_t)datum.u.arr.size;
    if (count == 0U) {
        return OD_OK;
    }
    list->items = calloc(count, sizeof(*list->items));
    if (list->items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate %s", key);
        return OD_ERROR_MEMORY;
    }
    list->count = count;
    for (size_t index = 0U; index < count; ++index) {
        toml_datum_t item = datum.u.arr.elem[index];
        if (item.type != TOML_STRING) {
            od_error_set(error, OD_ERROR_INVALID, "%s[%zu] must be a string", key, index);
            string_list_free(list);
            return OD_ERROR_INVALID;
        }
        list->items[index] = copy_string(item.u.s);
        if (list->items[index] == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to copy %s[%zu]", key, index);
            string_list_free(list);
            return OD_ERROR_MEMORY;
        }
    }
    return OD_OK;
}

static OdStatus require_string(toml_datum_t table,
                               const char *key,
                               char **destination,
                               OdError *error) {
    toml_datum_t datum = toml_get(table, key);
    if (datum.type != TOML_STRING || datum.u.s[0] == '\0') {
        od_error_set(error, OD_ERROR_INVALID, "%s must be a non-empty string", key);
        return OD_ERROR_INVALID;
    }
    *destination = copy_string(datum.u.s);
    if (*destination == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy %s", key);
        return OD_ERROR_MEMORY;
    }
    return OD_OK;
}

static OdStatus require_port(toml_datum_t table,
                             const char *key,
                             uint16_t *destination,
                             OdError *error) {
    toml_datum_t datum = toml_get(table, key);
    if (datum.type != TOML_INT64 || datum.u.int64 < 1 || datum.u.int64 > 65535) {
        od_error_set(error, OD_ERROR_INVALID, "%s must be a port from 1 to 65535", key);
        return OD_ERROR_INVALID;
    }
    *destination = (uint16_t)datum.u.int64;
    return OD_OK;
}

static OdStatus parse_protocols(toml_datum_t table, unsigned *protocols, OdError *error) {
    toml_datum_t datum = toml_get(table, "protocols");
    if (datum.type != TOML_ARRAY || datum.u.arr.size <= 0) {
        od_error_set(error, OD_ERROR_INVALID, "protocols must be a non-empty array");
        return OD_ERROR_INVALID;
    }
    *protocols = 0U;
    for (int32_t index = 0; index < datum.u.arr.size; ++index) {
        toml_datum_t item = datum.u.arr.elem[index];
        if (item.type != TOML_STRING) {
            od_error_set(error, OD_ERROR_INVALID, "protocols must contain strings");
            return OD_ERROR_INVALID;
        }
        if (strcmp(item.u.s, "tcp") == 0) {
            *protocols |= OD_PROTOCOL_TCP;
        } else if (strcmp(item.u.s, "udp") == 0) {
            *protocols |= OD_PROTOCOL_UDP;
        } else {
            od_error_set(error, OD_ERROR_INVALID, "unsupported protocol: %s", item.u.s);
            return OD_ERROR_INVALID;
        }
    }
    return OD_OK;
}

static OdStatus parse_service(toml_datum_t table, OdService *service, OdError *error) {
    *service = (OdService){0};
    service->managed = true;
    OdStatus status = require_string(table, "id", &service->id, error);
    if (status == OD_OK) status = require_string(table, "name", &service->name, error);
    if (status == OD_OK) status = require_string(table, "group", &service->group, error);
    if (status == OD_OK) status = require_string(table, "variable", &service->variable, error);
    if (status == OD_OK) status = require_port(table, "preferred_port", &service->preferred_port, error);
    if (status == OD_OK) status = parse_protocols(table, &service->protocols, error);
    if (status == OD_OK) status = parse_string_list(table, "sources", &service->sources, error);
    if (status != OD_OK) od_service_clear(service);
    return status;
}

static OdStatus parse_profile_tree(toml_datum_t root, OdProfile *profile, OdError *error) {
    toml_datum_t schema = toml_get(root, "schema_version");
    if (schema.type != TOML_INT64 || schema.u.int64 < 0 || schema.u.int64 > UINT_MAX) {
        od_error_set(error, OD_ERROR_INVALID, "schema_version must be an integer");
        return OD_ERROR_INVALID;
    }
    profile->schema_version = (unsigned)schema.u.int64;
    if (profile->schema_version != 1U) {
        od_error_set(error, OD_ERROR_UNSUPPORTED, "unsupported schema version %u", profile->schema_version);
        return OD_ERROR_UNSUPPORTED;
    }
    OdStatus status = require_string(root, "project_name", &profile->project_name, error);
    if (status == OD_OK) status = require_string(root, "assignment_file", &profile->assignment_file, error);
    if (status == OD_OK) status = require_port(root, "port_min", &profile->port_min, error);
    if (status == OD_OK) status = require_port(root, "port_max", &profile->port_max, error);

    toml_datum_t discovery = toml_get(root, "discovery");
    if (status == OD_OK && discovery.type != TOML_UNKNOWN && discovery.type != TOML_TABLE) {
        od_error_set(error, OD_ERROR_INVALID, "discovery must be a table");
        status = OD_ERROR_INVALID;
    }
    if (status == OD_OK && discovery.type == TOML_TABLE) {
        status = parse_string_list(discovery, "compose_files", &profile->compose_files, error);
        if (status == OD_OK) status = parse_string_list(discovery, "env_files", &profile->env_files, error);
        if (status == OD_OK) status = parse_string_list(discovery, "package_files", &profile->package_files, error);
    }

    toml_datum_t services = toml_get(root, "service");
    if (status == OD_OK && services.type != TOML_UNKNOWN && services.type != TOML_ARRAY) {
        od_error_set(error, OD_ERROR_INVALID, "service must be an array of tables");
        status = OD_ERROR_INVALID;
    }
    if (status == OD_OK && services.type == TOML_ARRAY) {
        for (int32_t index = 0; status == OD_OK && index < services.u.arr.size; ++index) {
            if (services.u.arr.elem[index].type != TOML_TABLE) {
                od_error_set(error, OD_ERROR_INVALID, "service[%d] must be a table", index);
                status = OD_ERROR_INVALID;
                break;
            }
            OdService service;
            status = parse_service(services.u.arr.elem[index], &service, error);
            if (status == OD_OK) {
                status = od_profile_add_service(profile, &service, error);
                od_service_clear(&service);
            }
        }
    }
    if (status == OD_OK) status = od_profile_validate(profile, error);
    return status;
}

OdStatus od_profile_parse(const char *text, size_t length, OdProfile *profile, OdError *error) {
    if (text == NULL || profile == NULL || length > (size_t)INT_MAX || length > OD_CONFIG_MAX_BYTES) {
        od_error_set(error, OD_ERROR_INVALID, "profile input is missing or too large");
        return OD_ERROR_INVALID;
    }
    od_profile_init(profile);
    toml_result_t parsed = toml_parse(text, (int)length);
    if (!parsed.ok) {
        od_error_set(error, OD_ERROR_INVALID, "invalid TOML: %s", parsed.errmsg);
        toml_free(parsed);
        return OD_ERROR_INVALID;
    }
    OdStatus status = parse_profile_tree(parsed.toptab, profile, error);
    toml_free(parsed);
    if (status != OD_OK) od_profile_free(profile);
    return status;
}

static OdStatus read_file(const char *path, char **text, size_t *length, OdError *error) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        od_error_set(error, OD_ERROR_IO, "unable to open %s", path);
        return OD_ERROR_IO;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        od_error_set(error, OD_ERROR_IO, "unable to inspect %s", path);
        return OD_ERROR_IO;
    }
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > OD_CONFIG_MAX_BYTES || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        od_error_set(error, OD_ERROR_INVALID, "%s is too large or unreadable", path);
        return OD_ERROR_INVALID;
    }
    size_t size = (size_t)end;
    char *buffer = malloc(size + 1U);
    if (buffer == NULL) {
        fclose(file);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read %s", path);
        return OD_ERROR_MEMORY;
    }
    size_t read_count = fread(buffer, 1U, size, file);
    int close_result = fclose(file);
    if (read_count != size || close_result != 0) {
        free(buffer);
        od_error_set(error, OD_ERROR_IO, "unable to read %s", path);
        return OD_ERROR_IO;
    }
    buffer[size] = '\0';
    *text = buffer;
    *length = size;
    return OD_OK;
}

OdStatus od_profile_load(const char *path, OdProfile *profile, OdError *error) {
    if (path == NULL || profile == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "profile path is required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_file(path, &text, &length, error);
    if (status == OD_OK) status = od_profile_parse(text, length, profile, error);
    free(text);
    return status;
}

static OdStatus render_string_list(TextBuilder *builder,
                                   const char *key,
                                   const OdStringList *list,
                                   OdError *error) {
    OdStatus status = builder_appendf(builder, error, "%s = [", key);
    for (size_t index = 0U; status == OD_OK && index < list->count; ++index) {
        if (index != 0U) status = builder_append(builder, ", ", error);
        if (status == OD_OK) status = builder_append_quoted(builder, list->items[index], error);
    }
    if (status == OD_OK) status = builder_append(builder, "]\n", error);
    return status;
}

OdStatus od_profile_render(const OdProfile *profile, char **text, size_t *length, OdError *error) {
    if (profile == NULL || text == NULL || length == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "profile and output are required");
        return OD_ERROR_INVALID;
    }
    OdStatus status = od_profile_validate(profile, error);
    TextBuilder builder = {0};
    if (status == OD_OK) status = builder_appendf(&builder, error, "schema_version = %u\nproject_name = ", profile->schema_version);
    if (status == OD_OK) status = builder_append_quoted(&builder, profile->project_name, error);
    if (status == OD_OK) status = builder_append(&builder, "\nassignment_file = ", error);
    if (status == OD_OK) status = builder_append_quoted(&builder, profile->assignment_file, error);
    if (status == OD_OK) {
        status = builder_appendf(&builder, error, "\nport_min = %u\nport_max = %u\n\n[discovery]\n",
                                 (unsigned)profile->port_min, (unsigned)profile->port_max);
    }
    if (status == OD_OK) status = render_string_list(&builder, "compose_files", &profile->compose_files, error);
    if (status == OD_OK) status = render_string_list(&builder, "env_files", &profile->env_files, error);
    if (status == OD_OK) status = render_string_list(&builder, "package_files", &profile->package_files, error);
    for (size_t index = 0U; status == OD_OK && index < profile->service_count; ++index) {
        const OdService *service = &profile->services[index];
        status = builder_append(&builder, "\n[[service]]\nid = ", error);
        if (status == OD_OK) status = builder_append_quoted(&builder, service->id, error);
        if (status == OD_OK) status = builder_append(&builder, "\nname = ", error);
        if (status == OD_OK) status = builder_append_quoted(&builder, service->name, error);
        if (status == OD_OK) status = builder_append(&builder, "\ngroup = ", error);
        if (status == OD_OK) status = builder_append_quoted(&builder, service->group, error);
        if (status == OD_OK) status = builder_append(&builder, "\nvariable = ", error);
        if (status == OD_OK) status = builder_append_quoted(&builder, service->variable, error);
        if (status == OD_OK) {
            status = builder_appendf(&builder, error, "\npreferred_port = %u\nprotocols = [",
                                     (unsigned)service->preferred_port);
        }
        if (status == OD_OK && (service->protocols & OD_PROTOCOL_TCP) != 0U) status = builder_append(&builder, "\"tcp\"", error);
        if (status == OD_OK && (service->protocols & OD_PROTOCOL_UDP) != 0U) {
            status = builder_append(&builder,
                                    (service->protocols & OD_PROTOCOL_TCP) != 0U ? ", \"udp\"" : "\"udp\"",
                                    error);
        }
        if (status == OD_OK) status = builder_append(&builder, "]\n", error);
        if (status == OD_OK) status = render_string_list(&builder, "sources", &service->sources, error);
    }
    if (status != OD_OK) {
        free(builder.data);
        return status;
    }
    *text = builder.data;
    *length = builder.length;
    od_error_clear(error);
    return OD_OK;
}

static OdStatus optional_boolean(toml_datum_t root, const char *key, bool *value, OdError *error) {
    toml_datum_t datum = toml_get(root, key);
    if (datum.type == TOML_UNKNOWN) return OD_OK;
    if (datum.type != TOML_BOOLEAN) {
        od_error_set(error, OD_ERROR_INVALID, "%s must be a boolean", key);
        return OD_ERROR_INVALID;
    }
    *value = datum.u.boolean;
    return OD_OK;
}

OdStatus od_settings_parse(const char *text, size_t length, OdSettings *settings, OdError *error) {
    if (text == NULL || settings == NULL || length > (size_t)INT_MAX || length > OD_CONFIG_MAX_BYTES) {
        od_error_set(error, OD_ERROR_INVALID, "settings input is missing or too large");
        return OD_ERROR_INVALID;
    }
    od_settings_defaults(settings);
    toml_result_t parsed = toml_parse(text, (int)length);
    if (!parsed.ok) {
        od_error_set(error, OD_ERROR_INVALID, "invalid TOML: %s", parsed.errmsg);
        toml_free(parsed);
        return OD_ERROR_INVALID;
    }
    toml_datum_t schema = toml_get(parsed.toptab, "schema_version");
    OdStatus status = OD_OK;
    if (schema.type != TOML_INT64 || schema.u.int64 != 1) {
        status = schema.type == TOML_INT64 ? OD_ERROR_UNSUPPORTED : OD_ERROR_INVALID;
        od_error_set(error, status, "unsupported or missing settings schema_version");
    }
    toml_datum_t theme = toml_get(parsed.toptab, "theme");
    if (status == OD_OK && theme.type != TOML_UNKNOWN) {
        if (theme.type != TOML_STRING || theme.u.str.len <= 0 || (size_t)theme.u.str.len >= sizeof(settings->theme)) {
            od_error_set(error, OD_ERROR_INVALID, "theme must be a short non-empty string");
            status = OD_ERROR_INVALID;
        } else {
            (void)strcpy(settings->theme, theme.u.s);
        }
    }
    toml_datum_t unicode = toml_get(parsed.toptab, "unicode");
    if (status == OD_OK && unicode.type != TOML_UNKNOWN) {
        if (unicode.type != TOML_STRING) {
            od_error_set(error, OD_ERROR_INVALID, "unicode must be auto, always, or never");
            status = OD_ERROR_INVALID;
        } else if (strcmp(unicode.u.s, "auto") == 0) {
            settings->unicode_mode = OD_UNICODE_AUTO;
        } else if (strcmp(unicode.u.s, "always") == 0) {
            settings->unicode_mode = OD_UNICODE_ALWAYS;
        } else if (strcmp(unicode.u.s, "never") == 0) {
            settings->unicode_mode = OD_UNICODE_NEVER;
        } else {
            od_error_set(error, OD_ERROR_INVALID, "unicode must be auto, always, or never");
            status = OD_ERROR_INVALID;
        }
    }
    if (status == OD_OK) status = optional_boolean(parsed.toptab, "reduced_motion", &settings->reduced_motion, error);
    if (status == OD_OK) status = optional_boolean(parsed.toptab, "mouse", &settings->mouse, error);
    if (status == OD_OK) status = optional_boolean(parsed.toptab, "auto_refresh", &settings->auto_refresh, error);
    toml_datum_t refresh = toml_get(parsed.toptab, "refresh_seconds");
    if (status == OD_OK && refresh.type != TOML_UNKNOWN) {
        if (refresh.type != TOML_INT64 || refresh.u.int64 < 1 || refresh.u.int64 > 3600) {
            od_error_set(error, OD_ERROR_INVALID, "refresh_seconds must be from 1 to 3600");
            status = OD_ERROR_INVALID;
        } else {
            settings->refresh_seconds = (unsigned)refresh.u.int64;
        }
    }
    toml_free(parsed);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

OdStatus od_settings_load(const char *path, OdSettings *settings, OdError *error) {
    if (path == NULL || settings == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "settings path is required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_file(path, &text, &length, error);
    if (status == OD_OK) status = od_settings_parse(text, length, settings, error);
    free(text);
    return status;
}

OdStatus od_settings_render(const OdSettings *settings,
                            char **text,
                            size_t *length,
                            OdError *error) {
    if (settings == NULL || text == NULL || length == NULL ||
        settings->schema_version != 1U || settings->theme[0] == '\0' ||
        settings->unicode_mode > OD_UNICODE_NEVER ||
        settings->refresh_seconds < 1U || settings->refresh_seconds > 3600U) {
        od_error_set(error, OD_ERROR_INVALID, "settings values are invalid");
        return OD_ERROR_INVALID;
    }
    static const char *const unicode_names[] = {"auto", "always", "never"};
    TextBuilder builder = {0};
    OdStatus status = builder_appendf(
        &builder, error,
        "schema_version = 1\n"
        "theme = \"%s\"\n"
        "unicode = \"%s\"\n"
        "reduced_motion = %s\n"
        "mouse = %s\n"
        "auto_refresh = %s\n"
        "refresh_seconds = %u\n",
        settings->theme, unicode_names[settings->unicode_mode],
        settings->reduced_motion ? "true" : "false",
        settings->mouse ? "true" : "false",
        settings->auto_refresh ? "true" : "false",
        settings->refresh_seconds);
    if (status != OD_OK) {
        free(builder.data);
        return status;
    }
    *text = builder.data;
    *length = builder.length;
    od_error_clear(error);
    return OD_OK;
}
