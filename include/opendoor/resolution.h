#ifndef OPENDOOR_RESOLUTION_H
#define OPENDOOR_RESOLUTION_H

#include "opendoor/allocation.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t allocation_index;
    char *variable;
    uint16_t old_port;
    uint16_t new_port;
} OdResolutionItem;

typedef struct {
    OdResolutionItem *items;
    size_t count;
} OdResolution;

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdAllocationPlan *plan,
                            OdError *error);
void od_resolution_free(OdResolution *resolution);

#endif
