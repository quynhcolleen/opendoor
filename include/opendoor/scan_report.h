#ifndef OPENDOOR_SCAN_REPORT_H
#define OPENDOOR_SCAN_REPORT_H

#include "opendoor/common.h"
#include <stdbool.h>
#include <stddef.h>

#define OD_SCAN_WARNING_CAP 200U
#define OD_SCAN_APPLY_BLOCKED "Apply blocked: socket table could not be fully read. Refresh and retry."

typedef enum {
    OD_WARNING_UNCLASSIFIED = 0,
    OD_WARNING_DISCOVERY,
    OD_WARNING_OWNER_ATTRIBUTION,
    OD_WARNING_SOCKET_FALLBACK,
    OD_WARNING_SOCKET_UNREADABLE,
    OD_WARNING_SOCKET_PARTIAL
} OdScanWarningKind;

typedef enum { OD_WARNING_INFORMATIONAL, OD_WARNING_BLOCKING } OdWarningSeverity;
typedef enum { OD_SCAN_COMPLETE, OD_SCAN_INCOMPLETE } OdScanCompleteness;

typedef struct {
    OdScanWarningKind kind;
    char *text;
} OdScanWarning;

/* count is retained entries; total_count and blocking_count include omissions.
 * The cap is enforced at warning producers as well as when reports are merged. */
typedef struct {
    OdScanWarning *items;
    size_t count;
    size_t total_count;
    size_t blocking_count;
    OdScanCompleteness completeness;
} OdScanReport;

OdWarningSeverity od_scan_warning_severity(OdScanWarningKind kind);
OdStatus od_scan_report_add(OdScanReport *report, OdScanWarningKind kind,
                            const char *subject, const char *reason, OdError *error);
OdStatus od_scan_report_combine(OdScanReport *output, const OdScanReport *discovery,
                                const OdScanReport *sockets, OdError *error);
void od_scan_report_free(OdScanReport *report);
bool od_scan_report_blocks_apply(const OdScanReport *report);

#endif
