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

/* Borrowed for the duration of the commit hook, in resolution item order.
 * Positions and file metadata describe the fully patched file. */
typedef struct {
    const OdResolutionItem *item;
    size_t line;
    size_t column;
    size_t byte_offset;
    size_t byte_length;
    size_t file_size;
    uint64_t file_hash;
} OdCommittedEdit;

typedef OdStatus (*OdPatchCommitHook)(const OdCommittedEdit *edits,
                                      size_t count,
                                      void *context,
                                      OdError *error);

/* A NULL validator uses production validation. The optional commit hook runs
 * once after all written validation, while source rollback remains possible.
 * Hook failure rolls every touched source back. */
OdStatus od_apply_resolution_with_commit(const OdResolution *resolution,
                                         OdPatchValidator validator,
                                         void *validator_context,
                                         OdPatchCommitHook commit,
                                         void *commit_context,
                                         size_t *updated,
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
