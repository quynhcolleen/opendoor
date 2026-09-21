#ifndef OPENDOOR_RESOLUTION_H
#define OPENDOOR_RESOLUTION_H

#include "opendoor/allocation.h"
#include "opendoor/common.h"
#include "opendoor/model.h"
#include "opendoor/scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_RESOLUTION_PENDING,
    OD_RESOLUTION_ACCEPTED,
    OD_RESOLUTION_SKIPPED
} OdResolutionDecision;

typedef struct {
    size_t allocation_index;
    char service[128];
    char owner[192];
    uint16_t original_port;
    OdResolutionDecision decision;
} OdResolutionItem;

typedef struct {
    const OdProfile *profile;
    const OdScanSnapshot *snapshot;
    OdAllocationPlan *plan;
    OdResolutionItem *items;
    size_t count;
    size_t current;
} OdResolution;

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdProfile *profile,
                            const OdScanSnapshot *snapshot,
                            OdAllocationPlan *plan,
                            OdError *error);
void od_resolution_free(OdResolution *resolution);
const OdResolutionItem *od_resolution_current(const OdResolution *resolution);
bool od_resolution_done(const OdResolution *resolution);
void od_resolution_accept(OdResolution *resolution);
void od_resolution_skip(OdResolution *resolution);
OdStatus od_resolution_edit(OdResolution *resolution,
                            uint16_t port,
                            OdError *error);

#endif
