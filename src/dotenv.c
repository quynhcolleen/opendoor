#include "opendoor/dotenv.h"

#include <stdlib.h>
#include <string.h>

void od_assignments_init(OdAssignments *assignments) {
    if (assignments != NULL) *assignments = (OdAssignments){0};
}

void od_assignments_free(OdAssignments *assignments) {
    if (assignments == NULL) return;
    for (size_t index = 0U; index < assignments->count; ++index) {
        free(assignments->items[index].variable);
    }
    free(assignments->items);
    *assignments = (OdAssignments){0};
}

const OdAssignment *od_assignments_find(const OdAssignments *assignments,
                                        const char *variable) {
    if (assignments == NULL || variable == NULL) return NULL;
    for (size_t index = 0U; index < assignments->count; ++index) {
        if (strcmp(assignments->items[index].variable, variable) == 0) {
            return &assignments->items[index];
        }
    }
    return NULL;
}

OdStatus od_assignments_add(OdAssignments *assignments,
                            const char *variable,
                            uint16_t port,
                            OdError *error) {
    if (assignments == NULL || variable == NULL || variable[0] == '\0' || port == 0U) {
        od_error_set(error, OD_ERROR_INVALID, "assignment key and port are required");
        return OD_ERROR_INVALID;
    }
    if (od_assignments_find(assignments, variable) != NULL) {
        od_error_set(error, OD_ERROR_INVALID, "duplicate assignment: %s", variable);
        return OD_ERROR_INVALID;
    }
    OdAssignment *items = realloc(assignments->items,
                                  (assignments->count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow wanted-port list");
        return OD_ERROR_MEMORY;
    }
    assignments->items = items;
    size_t length = strlen(variable) + 1U;
    items[assignments->count].variable = malloc(length);
    if (items[assignments->count].variable == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy wanted-port key");
        return OD_ERROR_MEMORY;
    }
    memcpy(items[assignments->count].variable, variable, length);
    items[assignments->count].port = port;
    ++assignments->count;
    od_error_clear(error);
    return OD_OK;
}
