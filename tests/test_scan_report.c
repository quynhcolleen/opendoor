#include "opendoor/scan_report.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(void) {
    OdScanReport discovery = {0}, sockets = {0}, screen = {0};
    OdError error;
    CHECK(od_scan_report_combine(&screen, &discovery, &sockets, &error) == OD_OK);
    CHECK(screen.completeness == OD_SCAN_COMPLETE);
    CHECK(!od_scan_report_blocks_apply(&screen));
    CHECK(od_scan_report_add(&discovery, OD_WARNING_DISCOVERY,
                             "./package.json", "invalid JSON", &error) == OD_OK);
    CHECK(od_scan_report_add(&sockets, OD_WARNING_OWNER_ATTRIBUTION,
                             "Process ownership", "some owners could not be read", &error) == OD_OK);
    CHECK(od_scan_report_combine(&screen, &discovery, &sockets, &error) == OD_OK);
    od_scan_report_free(&discovery);
    od_scan_report_free(&sockets);
    CHECK(screen.completeness == OD_SCAN_INCOMPLETE && screen.total_count == 2U);
    CHECK(!od_scan_report_blocks_apply(&screen));
    CHECK(screen.count == 2U);
    if (screen.count == 2U) {
        CHECK(strcmp(screen.items[0].text, "./package.json: invalid JSON") == 0);
        CHECK(strcmp(screen.items[1].text, "Process ownership: some owners could not be read") == 0);
    }
    od_scan_report_free(&screen);
    for (size_t i = 0U; i < 205U; ++i) {
        CHECK(od_scan_report_add(&discovery, OD_WARNING_DISCOVERY,
                                 "./package.json", "invalid JSON", &error) == OD_OK);
    }
    CHECK(discovery.count == 200U && discovery.total_count == 205U);
    CHECK(od_scan_report_add(&sockets, OD_WARNING_SOCKET_PARTIAL,
                             "Socket table (IPv4 TCP)", "partial read", &error) == OD_OK);
    CHECK(od_scan_report_combine(&screen, &discovery, &sockets, &error) == OD_OK);
    CHECK(screen.count == 200U && screen.total_count == 206U);
    CHECK(screen.blocking_count == 1U && od_scan_report_blocks_apply(&screen));
    od_scan_report_free(&discovery);
    od_scan_report_free(&sockets);
    od_scan_report_free(&screen);
    CHECK(od_scan_report_add(&screen, (OdScanWarningKind)999,
                             "Unknown scan stage", "unclassified failure", &error) == OD_OK);
    CHECK(od_scan_report_blocks_apply(&screen));
    CHECK(od_scan_warning_severity(OD_WARNING_SOCKET_UNREADABLE) == OD_WARNING_BLOCKING);
    CHECK(od_scan_warning_severity(OD_WARNING_SOCKET_PARTIAL) == OD_WARNING_BLOCKING);
    CHECK(od_scan_warning_severity(OD_WARNING_UNCLASSIFIED) == OD_WARNING_BLOCKING);
    CHECK(od_scan_warning_severity(OD_WARNING_OWNER_ATTRIBUTION) == OD_WARNING_INFORMATIONAL);
    CHECK(od_scan_warning_severity(OD_WARNING_DISCOVERY) == OD_WARNING_INFORMATIONAL);
    CHECK(od_scan_warning_severity(OD_WARNING_SOCKET_FALLBACK) == OD_WARNING_INFORMATIONAL);
    od_scan_report_free(&screen);
    return failures == 0 ? 0 : 1;
}
