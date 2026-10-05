#ifndef OPENDOOR_HISTORY_H
#define OPENDOOR_HISTORY_H

#include "opendoor/discovery.h"

#define OD_HISTORY_VERSION 1U
#define OD_HISTORY_RECORD_MAX_BYTES (64U * 1024U)

typedef enum {
    OD_HISTORY_APPLY = 0,
    OD_HISTORY_REVERT
} OdHistoryKind;

typedef enum {
    OD_HISTORY_UNCHECKED = 0,
    OD_HISTORY_READY,
    OD_HISTORY_UNAVAILABLE
} OdHistoryAvailability;

/* Each record owns its strings. Availability/reason are derived, never logged.
 * has_reverts distinguishes a numeric identity from the persisted null value.
 * Span metadata describes the token after this single declaration change. */
typedef struct {
    unsigned int version;
    uint64_t id;
    char *timestamp;
    OdHistoryKind kind;
    bool has_reverts;
    uint64_t reverts;
    char *relative_path;
    OdPortSourceKind source_kind;
    OdPortWriteKind write_kind;
    char *environment_key;
    size_t line;
    size_t column;
    size_t byte_offset;
    size_t byte_length;
    uint16_t old_port;
    uint16_t new_port;
    OdHistoryAvailability availability;
    char *reason;
} OdHistoryRecord;

/* Owns every record and warning string. Records are newest-first by ID. */
typedef struct {
    OdHistoryRecord *items;
    size_t count;
    char **warnings;
    size_t warning_count;
} OdHistory;

/* Outputs must be fresh or freed before reuse; failure leaves them empty.
 * Parse/render operate on one JSON object without a JSONL newline. Rendered
 * text is owned by the caller. Both enforce the 64 KiB record bound.
 * Loading never creates storage; skipped lines are exposed through warnings. */
OdStatus od_history_record_parse(const char *text, size_t length,
                                  OdHistoryRecord *record, OdError *error);
OdStatus od_history_record_render(const OdHistoryRecord *record,
                                   char **text, size_t *length, OdError *error);
OdStatus od_history_load(const char *project_root, OdHistory *history,
                          OdError *error);
void od_history_record_free(OdHistoryRecord *record);
void od_history_free(OdHistory *history);

#endif
