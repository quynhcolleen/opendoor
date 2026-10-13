#include "opendoor/scan_report.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The single classification policy. New/unknown kinds fail closed. A working
 * fallback and missing owner metadata do not invalidate occupied port numbers. */
OdWarningSeverity od_scan_warning_severity(OdScanWarningKind kind) {
    switch (kind) {
        case OD_WARNING_DISCOVERY:
        case OD_WARNING_OWNER_ATTRIBUTION:
        case OD_WARNING_SOCKET_FALLBACK:
            return OD_WARNING_INFORMATIONAL;
        case OD_WARNING_SOCKET_UNREADABLE:
        case OD_WARNING_SOCKET_PARTIAL:
        case OD_WARNING_UNCLASSIFIED:
        default:
            return OD_WARNING_BLOCKING;
    }
}

void od_scan_report_free(OdScanReport *report) {
    if (report == NULL) return;
    for (size_t i = 0U; i < report->count; ++i) free(report->items[i].text);
    free(report->items);
    *report = (OdScanReport){0};
}

bool od_scan_report_blocks_apply(const OdScanReport *report) {
    return report != NULL && report->blocking_count > 0U;
}

OdStatus od_scan_report_add(OdScanReport *report, OdScanWarningKind kind,
                            const char *subject, const char *reason, OdError *error) {
    if (report == NULL || subject == NULL || reason == NULL ||
        report->total_count == SIZE_MAX) {
        od_error_set(error, OD_ERROR_INVALID, "invalid scan warning or warning count");
        return OD_ERROR_INVALID;
    }
    if (report->count < OD_SCAN_WARNING_CAP) {
        size_t a = strlen(subject), b = strlen(reason);
        if (b > SIZE_MAX - 3U || a > SIZE_MAX - b - 3U) goto no_memory;
        char *text = malloc(a + b + 3U);
        if (text == NULL) goto no_memory;
        (void)snprintf(text, a + b + 3U, b == 0U ? "%s%s" : "%s: %s", subject, reason);
        OdScanWarning *items = realloc(report->items, (report->count + 1U) * sizeof(*items));
        if (items == NULL) { free(text); goto no_memory; }
        report->items = items;
        report->items[report->count++] = (OdScanWarning){kind, text};
    }
    ++report->total_count;
    if (od_scan_warning_severity(kind) == OD_WARNING_BLOCKING) ++report->blocking_count;
    report->completeness = OD_SCAN_INCOMPLETE;
    od_error_clear(error);
    return OD_OK;
no_memory:
    od_error_set(error, OD_ERROR_MEMORY, "unable to retain scan warning");
    return OD_ERROR_MEMORY;
}

OdStatus od_scan_report_combine(OdScanReport *output, const OdScanReport *discovery,
                                const OdScanReport *sockets, OdError *error) {
    if (output == NULL || output == discovery || output == sockets) {
        od_error_set(error, OD_ERROR_INVALID, "separate scan report output is required");
        return OD_ERROR_INVALID;
    }
    OdScanReport combined = {0};
    const OdScanReport *inputs[] = {discovery, sockets};
    size_t total = 0U, blocking = 0U;
    for (size_t source = 0U; source < 2U; ++source) {
        const OdScanReport *input = inputs[source];
        if (input == NULL) continue;
        if (input->total_count > SIZE_MAX - total) {
            od_scan_report_free(&combined);
            od_error_set(error, OD_ERROR_INVALID, "scan warning count overflow");
            return OD_ERROR_INVALID;
        }
        total += input->total_count;
        blocking += input->blocking_count;
        for (size_t i = 0U; i < input->count && combined.count < OD_SCAN_WARNING_CAP; ++i) {
            OdStatus status = od_scan_report_add(&combined, input->items[i].kind,
                                                 input->items[i].text, "", error);
            if (status != OD_OK) { od_scan_report_free(&combined); return status; }
        }
    }
    combined.total_count = total;
    combined.blocking_count = blocking;
    combined.completeness = total == 0U ? OD_SCAN_COMPLETE : OD_SCAN_INCOMPLETE;
    od_scan_report_free(output);
    *output = combined;
    od_error_clear(error);
    return OD_OK;
}
