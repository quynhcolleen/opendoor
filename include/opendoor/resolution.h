#ifndef OPENDOOR_RESOLUTION_H
#define OPENDOOR_RESOLUTION_H

#include "opendoor/allocation.h"
#include "opendoor/discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t declaration_index;
    size_t allocation_index;
    char *variable;
    uint16_t old_port;
    uint16_t new_port;
    bool automatic;
    OdPortSourceKind source_kind;
    OdPortWriteKind write_kind;
    size_t line;
    size_t column;
    size_t byte_offset;
    size_t byte_length;
    size_t file_size;
    uint64_t file_hash;
    char *absolute_path;
    char *relative_path;
    char *line_before;
    char *line_after;
    char *manual_reason;
} OdResolutionItem;

typedef struct {
    OdResolutionItem *items;
    size_t count;
    size_t automatic_count;
    size_t manual_count;
    char *project_root;
} OdResolution;

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdAllocationPlan *plan,
                            OdError *error);
OdStatus od_resolution_build(const char *project_root,
                             const OdProjectDiscovery *discovery,
                             const OdScanSnapshot *snapshot,
                             OdResolution *resolution,
                             OdError *error);
OdStatus od_resolution_validate_snapshot(const char *project_root,
                                         const OdResolution *resolution,
                                         const OdScanSnapshot *snapshot,
                                         OdError *error);
void od_resolution_free(OdResolution *resolution);

#endif
