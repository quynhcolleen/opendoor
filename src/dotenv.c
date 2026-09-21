#include "opendoor/dotenv.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OD_DOTENV_MAX_BYTES (4U * 1024U * 1024U)

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static char *trim(char *value) {
    while (*value != '\0' && isspace((unsigned char)*value)) {
        ++value;
    }
    size_t length = strlen(value);
    while (length > 0U && isspace((unsigned char)value[length - 1U])) {
        value[--length] = '\0';
    }
    return value;
}

static bool valid_variable(const char *variable) {
    if (!(variable[0] == '_' || (variable[0] >= 'A' && variable[0] <= 'Z'))) {
        return false;
    }
    for (size_t index = 1U; variable[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)variable[index];
        if (!(character == '_' || isdigit(character) ||
              (character >= 'A' && character <= 'Z'))) {
            return false;
        }
    }
    return true;
}

void od_assignments_init(OdAssignments *assignments) {
    if (assignments != NULL) {
        *assignments = (OdAssignments){0};
    }
}

void od_assignments_free(OdAssignments *assignments) {
    if (assignments == NULL) {
        return;
    }
    for (size_t index = 0U; index < assignments->count; ++index) {
        free(assignments->items[index].variable);
    }
    free(assignments->items);
    *assignments = (OdAssignments){0};
}

const OdAssignment *od_assignments_find(const OdAssignments *assignments, const char *variable) {
    if (assignments == NULL || variable == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < assignments->count; ++index) {
        if (strcmp(assignments->items[index].variable, variable) == 0) {
            return &assignments->items[index];
        }
    }
    return NULL;
}

static OdStatus append_assignment(OdAssignments *assignments,
                                  const char *variable,
                                  uint16_t port,
                                  OdError *error) {
    if (od_assignments_find(assignments, variable) != NULL) {
        od_error_set(error, OD_ERROR_INVALID, "duplicate assignment: %s", variable);
        return OD_ERROR_INVALID;
    }
    OdAssignment *items = realloc(assignments->items,
                                  (assignments->count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store assignment");
        return OD_ERROR_MEMORY;
    }
    assignments->items = items;
    items[assignments->count].variable = copy_string(variable);
    items[assignments->count].port = port;
    if (items[assignments->count].variable == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy assignment variable");
        return OD_ERROR_MEMORY;
    }
    ++assignments->count;
    return OD_OK;
}

static OdStatus parse_assignments(const char *text,
                                  size_t length,
                                  OdAssignments *assignments,
                                  bool require_marker,
                                  OdError *error) {
    if (text == NULL || assignments == NULL || length > OD_DOTENV_MAX_BYTES) {
        od_error_set(error, OD_ERROR_INVALID, "assignment input is missing or too large");
        return OD_ERROR_INVALID;
    }
    od_assignments_init(assignments);
    char *buffer = malloc(length + 1U);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse assignments");
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
        if (content[0] == '\0' || content[0] == '#') {
            continue;
        }
        char *equals = strchr(content, '=');
        if (equals == NULL) {
            od_error_set(error, OD_ERROR_INVALID, "invalid assignment line: %s", content);
            status = OD_ERROR_INVALID;
            break;
        }
        *equals = '\0';
        char *variable = trim(content);
        char *value = trim(equals + 1);
        if (!valid_variable(variable) || value[0] == '\0') {
            od_error_set(error, OD_ERROR_INVALID, "invalid assignment: %s", variable);
            status = OD_ERROR_INVALID;
            break;
        }
        errno = 0;
        char *end = NULL;
        unsigned long numeric = strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0') {
            od_error_set(error, OD_ERROR_INVALID, "%s must contain a numeric value", variable);
            status = OD_ERROR_INVALID;
            break;
        }
        if (strcmp(variable, "PORTS_CONFIGURED") == 0 ||
            strcmp(variable, "OPENDOOR_CONFIGURED") == 0) {
            if (numeric != 1UL) {
                od_error_set(error, OD_ERROR_INVALID, "%s must equal 1", variable);
                status = OD_ERROR_INVALID;
            } else if (strcmp(variable, "PORTS_CONFIGURED") == 0) {
                assignments->compatible_marker = true;
            } else {
                assignments->opendoor_marker = true;
            }
            continue;
        }
        if (numeric == 0UL || numeric > 65535UL) {
            od_error_set(error, OD_ERROR_INVALID, "%s must contain a valid port", variable);
            status = OD_ERROR_INVALID;
            break;
        }
        status = append_assignment(assignments, variable, (uint16_t)numeric, error);
    }
    free(buffer);
    if (status == OD_OK && require_marker &&
        !assignments->compatible_marker && !assignments->opendoor_marker) {
        od_error_set(error, OD_ERROR_FOREIGN, "assignment file has no OpenDoor-compatible marker");
        status = OD_ERROR_FOREIGN;
    }
    if (status != OD_OK) {
        od_assignments_free(assignments);
    } else {
        od_error_clear(error);
    }
    return status;
}

OdStatus od_assignments_parse(const char *text,
                              size_t length,
                              OdAssignments *assignments,
                              OdError *error) {
    return parse_assignments(text, length, assignments, true, error);
}

OdStatus od_assignments_import(const char *text,
                               size_t length,
                               OdAssignments *assignments,
                               OdError *error) {
    return parse_assignments(text, length, assignments, false, error);
}

OdStatus od_assignments_render(const OdAssignments *assignments,
                               const char *profile_path,
                               char **text,
                               size_t *length,
                               OdError *error) {
    if (assignments == NULL || profile_path == NULL || text == NULL || length == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "assignments, profile path, and output are required");
        return OD_ERROR_INVALID;
    }
    size_t capacity = strlen(profile_path) + 160U;
    for (size_t index = 0U; index < assignments->count; ++index) {
        if (!valid_variable(assignments->items[index].variable) || assignments->items[index].port == 0U) {
            od_error_set(error, OD_ERROR_INVALID, "invalid assignment at index %zu", index);
            return OD_ERROR_INVALID;
        }
        capacity += strlen(assignments->items[index].variable) + 16U;
    }
    char *output = malloc(capacity);
    if (output == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to render assignments");
        return OD_ERROR_MEMORY;
    }
    int count = snprintf(output, capacity,
                         "# Generated by OpenDoor. Machine-local; do not commit.\n"
                         "# Profile: %s\n"
                         "PORTS_CONFIGURED=1\n"
                         "OPENDOOR_CONFIGURED=1\n",
                         profile_path);
    if (count < 0 || (size_t)count >= capacity) {
        free(output);
        od_error_set(error, OD_ERROR_INVALID, "unable to render assignment header");
        return OD_ERROR_INVALID;
    }
    size_t used = (size_t)count;
    for (size_t index = 0U; index < assignments->count; ++index) {
        int written = snprintf(output + used, capacity - used, "%s=%u\n",
                               assignments->items[index].variable,
                               (unsigned)assignments->items[index].port);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(output);
            od_error_set(error, OD_ERROR_INVALID, "unable to render assignment");
            return OD_ERROR_INVALID;
        }
        used += (size_t)written;
    }
    *text = output;
    *length = used;
    od_error_clear(error);
    return OD_OK;
}
