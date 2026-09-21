#ifndef OPENDOOR_DOTENV_H
#define OPENDOOR_DOTENV_H

#include "opendoor/common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *variable;
    uint16_t port;
} OdAssignment;

typedef struct {
    OdAssignment *items;
    size_t count;
    bool compatible_marker;
    bool opendoor_marker;
} OdAssignments;

void od_assignments_init(OdAssignments *assignments);
void od_assignments_free(OdAssignments *assignments);
const OdAssignment *od_assignments_find(const OdAssignments *assignments, const char *variable);
OdStatus od_assignments_parse(const char *text, size_t length, OdAssignments *assignments, OdError *error);
OdStatus od_assignments_render(const OdAssignments *assignments, const char *profile_path, char **text, size_t *length, OdError *error);

#endif
