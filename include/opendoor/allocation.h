#ifndef OPENDOOR_ALLOCATION_H
#define OPENDOOR_ALLOCATION_H

#include "opendoor/dotenv.h"
#include "opendoor/model.h"

typedef enum {
    OD_ALLOC_PREFERRED,
    OD_ALLOC_PRESERVED,
    OD_ALLOC_REASSIGNED
} OdAllocationReason;

typedef struct {
    uint16_t port;
    unsigned protocols;
} OdOccupiedPort;

typedef struct {
    char *service_id;
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
OdStatus od_allocate(const OdProfile *profile,
                     const OdOccupiedPort *occupied,
                     size_t occupied_count,
                     const OdAssignments *saved,
                     OdAllocationPlan *plan,
                     OdError *error);

#endif

