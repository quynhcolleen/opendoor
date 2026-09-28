#ifndef OPENDOOR_ALLOCATION_H
#define OPENDOOR_ALLOCATION_H

#include "opendoor/dotenv.h"

#include <stddef.h>
#include <stdint.h>

#define OD_REPLACEMENT_PORT_MIN 1024U
#define OD_REPLACEMENT_PORT_MAX 65535U

typedef enum {
    OD_ALLOC_UNCHANGED,
    OD_ALLOC_REASSIGNED
} OdAllocationReason;

typedef struct {
    char *variable;
    uint16_t old_port;
    uint16_t new_port;
    OdAllocationReason reason;
} OdAllocation;

typedef struct {
    OdAllocation *items;
    size_t count;
} OdAllocationPlan;

void od_allocation_plan_free(OdAllocationPlan *plan);
OdStatus od_allocate(const OdAssignments *wanted,
                     const uint16_t *occupied,
                     size_t occupied_count,
                     OdAllocationPlan *plan,
                     OdError *error);

#endif
