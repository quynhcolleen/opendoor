#include "opendoor/config.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OD_CONFIG_MAX_BYTES (4U * 1024U * 1024U)

static char *trim(char *value) {
    while (*value != '\0' && isspace((unsigned char)*value)) ++value;
    size_t length = strlen(value);
    while (length > 0U && isspace((unsigned char)value[length - 1U])) {
        value[--length] = '\0';
    }
    return value;
}

static bool valid_variable(const char *variable) {
    if (variable[0] != '_' && (variable[0] < 'A' || variable[0] > 'Z')) return false;
    for (size_t index = 1U; variable[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)variable[index];
        if (character != '_' && !isdigit(character) &&
            (character < 'A' || character > 'Z')) return false;
    }
    return true;
}

OdStatus od_config_parse(const char *text,
                         size_t length,
                         OdAssignments *assignments,
                         OdError *error) {
    if (text == NULL || assignments == NULL || length > OD_CONFIG_MAX_BYTES ||
        memchr(text, '\0', length) != NULL) {
        od_error_set(error, OD_ERROR_INVALID,
                     "wanted-ports input is missing, too large, or contains NUL data");
        return OD_ERROR_INVALID;
    }
    od_assignments_init(assignments);
    char *buffer = malloc(length + 1U);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse wanted ports");
        return OD_ERROR_MEMORY;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';

    OdStatus status = OD_OK;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\n", &save);
         line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &save)) {
        char *content = trim(line);
        if (content[0] == '\0' || content[0] == '#') continue;
        char *equals = strchr(content, '=');
        if (equals == NULL) {
            od_error_set(error, OD_ERROR_INVALID, "invalid wanted-port line: %s", content);
            status = OD_ERROR_INVALID;
            break;
        }
        *equals = '\0';
        char *variable = trim(content);
        char *value = trim(equals + 1);
        if (!valid_variable(variable) || value[0] == '\0') {
            od_error_set(error, OD_ERROR_INVALID, "invalid wanted-port key: %s", variable);
            status = OD_ERROR_INVALID;
            break;
        }
        errno = 0;
        char *end = NULL;
        unsigned long numeric = strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' ||
            numeric == 0UL || numeric > 65535UL) {
            od_error_set(error, OD_ERROR_INVALID,
                         "%s must contain a port from 1 to 65535", variable);
            status = OD_ERROR_INVALID;
            break;
        }
        status = od_assignments_add(assignments, variable, (uint16_t)numeric, error);
    }
    free(buffer);
    if (status != OD_OK) {
        od_assignments_free(assignments);
    } else {
        od_error_clear(error);
    }
    return status;
}

OdStatus od_config_load(const char *path,
                        OdAssignments *assignments,
                        OdError *error) {
    if (path == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "wanted-ports path is required");
        return OD_ERROR_INVALID;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        od_error_set(error, OD_ERROR_IO, "unable to open %s: %s", path, strerror(errno));
        return OD_ERROR_IO;
    }
    char *buffer = malloc(OD_CONFIG_MAX_BYTES + 1U);
    if (buffer == NULL) {
        (void)fclose(file);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read wanted ports");
        return OD_ERROR_MEMORY;
    }
    size_t length = fread(buffer, 1U, OD_CONFIG_MAX_BYTES + 1U, file);
    if (ferror(file) != 0) {
        free(buffer);
        (void)fclose(file);
        od_error_set(error, OD_ERROR_IO, "unable to read %s", path);
        return OD_ERROR_IO;
    }
    if (length > OD_CONFIG_MAX_BYTES) {
        free(buffer);
        (void)fclose(file);
        od_error_set(error, OD_ERROR_INVALID, "%s exceeds the 4 MiB limit", path);
        return OD_ERROR_INVALID;
    }
    if (fclose(file) != 0) {
        free(buffer);
        od_error_set(error, OD_ERROR_IO, "unable to close %s", path);
        return OD_ERROR_IO;
    }
    OdStatus status = od_config_parse(buffer, length, assignments, error);
    free(buffer);
    return status;
}

OdStatus od_config_render(const OdAssignments *assignments,
                          char **text,
                          size_t *length,
                          OdError *error) {
    if (assignments == NULL || text == NULL || length == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "wanted ports and output are required");
        return OD_ERROR_INVALID;
    }
    size_t capacity = 1U;
    for (size_t index = 0U; index < assignments->count; ++index) {
        const OdAssignment *assignment = &assignments->items[index];
        if (assignment->variable == NULL || !valid_variable(assignment->variable) ||
            assignment->port == 0U) {
            od_error_set(error, OD_ERROR_INVALID,
                         "invalid wanted port at index %zu", index);
            return OD_ERROR_INVALID;
        }
        size_t addition = strlen(assignment->variable) + 8U;
        if (capacity > SIZE_MAX - addition) {
            od_error_set(error, OD_ERROR_MEMORY, "wanted-ports output is too large");
            return OD_ERROR_MEMORY;
        }
        capacity += addition;
    }
    char *output = malloc(capacity);
    if (output == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to render wanted ports");
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    for (size_t index = 0U; index < assignments->count; ++index) {
        int written = snprintf(output + used, capacity - used, "%s=%u\n",
                               assignments->items[index].variable,
                               (unsigned)assignments->items[index].port);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(output);
            od_error_set(error, OD_ERROR_INVALID, "unable to render wanted ports");
            return OD_ERROR_INVALID;
        }
        used += (size_t)written;
    }
    output[used] = '\0';
    *text = output;
    *length = used;
    od_error_clear(error);
    return OD_OK;
}
