#ifndef OPENDOOR_HISTORY_H
#define OPENDOOR_HISTORY_H

#include "opendoor/discovery.h"
#include "opendoor/resolution.h"

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
/* Pure snapshot classification: borrows discovery, and replaces only the
 * record's availability and owned reason. NULL and empty keys are equivalent.
 * This does not verify file safety or authorize mutation. */
OdStatus od_history_classify_record(OdHistoryRecord *record,
                                     const OdProjectDiscovery *discovery,
                                     OdError *error);
/* Rediscover the project and refresh availability/reasons in already loaded
 * records, including source path safety. Never creates history storage. */
OdStatus od_history_refresh(const char *project_root, OdHistory *history,
                             OdError *error);
/* Reload by stable ID, rediscover and revalidate, then perform one recorded
 * reverse transaction. Refusal leaves updated at zero. */
OdStatus od_history_revert(const char *project_root, uint64_t record_id,
                            size_t *updated, OdError *error);
/* Records each automatic edit under an exclusive history lock. Normal apply
 * uses OD_HISTORY_APPLY and NULL; revert uses OD_HISTORY_REVERT and a target ID.
 * Eligibility for a revert must be checked by the caller. Logging errors roll
 * back the uncommitted log tail and every changed source. */
OdStatus od_history_apply_resolution(const OdResolution *resolution,
                                      OdHistoryKind kind,
                                      const uint64_t *reverts,
                                      size_t *updated,
                                      OdError *error);
void od_history_record_free(OdHistoryRecord *record);
void od_history_free(OdHistory *history);

#endif
