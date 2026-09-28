#ifndef OPENDOOR_DOTENV_H
#define OPENDOOR_DOTENV_H

#include "opendoor/common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *variable;
    uint16_t port;
} OdAssignment;

typedef struct {
    OdAssignment *items;
    size_t count;
} OdAssignments;

void od_assignments_init(OdAssignments *assignments);
void od_assignments_free(OdAssignments *assignments);
const OdAssignment *od_assignments_find(const OdAssignments *assignments,
                                        const char *variable);
OdStatus od_assignments_add(OdAssignments *assignments,
                            const char *variable,
                            uint16_t port,
                            OdError *error);

#endif
