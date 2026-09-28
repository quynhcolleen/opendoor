#ifndef OPENDOOR_PERSISTENCE_H
#define OPENDOOR_PERSISTENCE_H

#include "opendoor/resolution.h"

typedef enum {
    OD_PATCH_VALIDATE_MEMORY = 0,
    OD_PATCH_VALIDATE_WRITTEN
} OdPatchValidationPhase;

typedef OdStatus (*OdPatchValidator)(OdPortSourceKind source_kind,
                                     const char *path,
                                     const char *text,
                                     size_t length,
                                     OdPatchValidationPhase phase,
                                     void *context,
                                     OdError *error);

OdStatus od_config_write(const char *path,
                         const OdAssignments *assignments,
                         OdError *error);

OdStatus od_apply_resolution_with_validator(const OdResolution *resolution,
                                            OdPatchValidator validator,
                                            void *context,
                                            size_t *updated,
                                            OdError *error);
OdStatus od_apply_resolution(const OdResolution *resolution,
                             size_t *updated,
                             OdError *error);

#endif
