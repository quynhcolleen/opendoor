/* Linked only into opendoor_scan_test; the shipped binary has no fault controls. */
#include "opendoor/scan.h"
#include <stdlib.h>
#include <string.h>

OdStatus __real_od_scan_host(OdScanSnapshot *snapshot, OdError *error);

OdStatus __wrap_od_scan_host(OdScanSnapshot *snapshot, OdError *error) {
    static size_t calls;
    ++calls;
    OdStatus status = __real_od_scan_host(snapshot, error);
    const char *mode = getenv("OD_TEST_SCAN_WARNING");
    if (status != OD_OK || mode == NULL) return status;
    /* Preserve real endpoints, control just the environmental diagnostics. */
    od_scan_report_free(&snapshot->report);
    if (strcmp(mode, "owner") == 0 ||
        (strcmp(mode, "owner_prewrite") == 0 && calls == 2U)) {
        return od_scan_report_add(&snapshot->report, OD_WARNING_OWNER_ATTRIBUTION,
            "Process ownership", "some owners could not be read", error);
    }
    if (strcmp(mode, "blocking_initial") == 0 ||
        (strcmp(mode, "blocking_prewrite") == 0 && calls == 2U)) {
        return od_scan_report_add(&snapshot->report, OD_WARNING_SOCKET_PARTIAL,
            "Socket table (IPv4 TCP)", "partial read", error);
    }
    if (strcmp(mode, "unknown") == 0) {
        return od_scan_snapshot_add_warning(snapshot, "unclassified failure", error);
    }
    return OD_OK;
}
