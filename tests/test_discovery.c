#include "opendoor/config.h"
#include "opendoor/dashboard.h"
#include "opendoor/discovery.h"
#include "opendoor/scan.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static void test_proc_socket_parsing_and_deduplication(void) {
    const char *tcp4 =
        "  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
        "   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 11111\n"
        "   1: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 11111\n";
    const char *udp6 =
        "  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
        "   0: 00000000000000000000000000000000:14E9 00000000000000000000000000000000:0000 07 00000000:00000000 00:00000000 00000000 1001 0 22222\n";
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 7U);
    CHECK(od_parse_proc_net(tcp4, AF_INET, OD_PROTOCOL_TCP,
                            &snapshot, &error) == OD_OK);
    CHECK(od_parse_proc_net(udp6, AF_INET6, OD_PROTOCOL_UDP,
                            &snapshot, &error) == OD_OK);
    CHECK(snapshot.endpoint_count == 2U);
    if (snapshot.endpoint_count == 2U) {
        CHECK(strcmp(snapshot.endpoints[0].local_address, "127.0.0.1") == 0);
        CHECK(snapshot.endpoints[0].local_port == 3000U);
        CHECK(snapshot.endpoints[1].local_port == 5353U);
        CHECK(snapshot.endpoints[1].protocol == OD_PROTOCOL_UDP);
    }
    od_scan_snapshot_free(&snapshot);
}

static void test_socket_inode_target(void) {
    uint64_t inode = 0U;
    CHECK(od_parse_socket_inode("socket:[987654]", &inode));
    CHECK(inode == 987654U);
    CHECK(!od_parse_socket_inode("pipe:[987654]", &inode));
    CHECK(!od_parse_socket_inode("socket:[broken]", &inode));
}

static void test_occupied_ports_are_unique_and_sorted(void) {
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 1U);
    snapshot.endpoints = calloc(4U, sizeof(*snapshot.endpoints));
    CHECK(snapshot.endpoints != NULL);
    if (snapshot.endpoints == NULL) return;
    snapshot.endpoint_count = 4U;
    snapshot.endpoint_capacity = 4U;
    snapshot.endpoints[0].local_port = 5000U;
    snapshot.endpoints[1].local_port = 3000U;
    snapshot.endpoints[2].local_port = 5000U;
    snapshot.endpoints[3].local_port = 4000U;

    uint16_t *ports = NULL;
    size_t count = 0U;
    CHECK(od_discover_occupied_ports(&snapshot, &ports, &count, &error) == OD_OK);
    CHECK(count == 3U);
    if (count == 3U) {
        CHECK(ports[0] == 3000U);
        CHECK(ports[1] == 4000U);
        CHECK(ports[2] == 5000U);
    }
    free(ports);
    od_scan_snapshot_free(&snapshot);
}

static void test_dashboard_is_the_union_of_wanted_and_occupied_ports(void) {
    OdAssignments wanted;
    OdError error;
    const char *text =
        "API_PORT=3000\n"
        "FREE_PORT=4000\n"
        "DUPLICATE_PORT=4000\n";
    CHECK(od_config_parse(text, strlen(text), &wanted, &error) == OD_OK);
    OdEndpoint endpoints[] = {
        {
            .local_port = 3000U,
            .pid = 42,
            .process = "api-server",
            .directory = "/work/services/api"
        },
        {
            .local_port = 5000U,
            .pid = 84,
            .process = "metrics",
            .directory = "/work/services/metrics"
        }
    };
    OdScanSnapshot snapshot = {
        .endpoints = endpoints,
        .endpoint_count = 2U
    };
    OdDashboard dashboard;
    CHECK(od_dashboard_init(&dashboard, &wanted, &snapshot, &error) == OD_OK);
    CHECK(dashboard.count == 3U);
    if (dashboard.count == 3U) {
        CHECK(dashboard.rows[0].port == 3000U);
        CHECK(dashboard.rows[0].running);
        CHECK(dashboard.rows[0].conflict);
        CHECK(strcmp(dashboard.rows[0].process, "api-server") == 0);
        CHECK(strcmp(dashboard.rows[0].directory, "/work/services/api") == 0);
        CHECK(dashboard.rows[1].port == 4000U);
        CHECK(!dashboard.rows[1].running);
        CHECK(dashboard.rows[1].conflict);
        CHECK(strcmp(dashboard.rows[1].process, "-") == 0);
        CHECK(strcmp(dashboard.rows[1].directory, "-") == 0);
        CHECK(dashboard.rows[2].port == 5000U);
        CHECK(dashboard.rows[2].running);
        CHECK(!dashboard.rows[2].conflict);
        CHECK(strcmp(dashboard.rows[2].process, "metrics") == 0);
        CHECK(strcmp(dashboard.rows[2].directory, "/work/services/metrics") == 0);
    }
    od_dashboard_set_page_size(&dashboard, 2U);
    od_dashboard_scroll(&dashboard, 20);
    CHECK(dashboard.scroll == 1U);
    od_dashboard_scroll(&dashboard, -20);
    CHECK(dashboard.scroll == 0U);
    od_dashboard_free(&dashboard);
    od_assignments_free(&wanted);
}

int main(void) {
    test_proc_socket_parsing_and_deduplication();
    test_socket_inode_target();
    test_occupied_ports_are_unique_and_sorted();
    test_dashboard_is_the_union_of_wanted_and_occupied_ports();
    if (failures != 0) {
        fprintf(stderr, "%d scan checks failed\n", failures);
        return 1;
    }
    puts("scan checks passed");
    return 0;
}
