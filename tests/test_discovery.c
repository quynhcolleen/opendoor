#include "opendoor/discovery.h"
#include "opendoor/docker.h"
#include "opendoor/scan.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static void test_proc_tcp_udp_ipv4_ipv6_and_deduplication(void) {
    const char *tcp4 =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000 0 11111\n"
        "   1: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000 0 11111\n";
    const char *udp6 =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 00000000000000000000000000000000:14E9 00000000000000000000000000000000:0000 07 00000000:00000000 00:00000000 00000000  1001 0 22222\n";
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 7U);
    OdStatus tcp_status = od_parse_proc_net(tcp4, AF_INET, OD_PROTOCOL_TCP, &snapshot, &error);
    OdStatus udp_status = od_parse_proc_net(udp6, AF_INET6, OD_PROTOCOL_UDP, &snapshot, &error);
    CHECK(tcp_status == OD_OK);
    CHECK(udp_status == OD_OK);
    CHECK(snapshot.endpoint_count == 2U);
    if (snapshot.endpoint_count != 2U) {
        od_scan_snapshot_free(&snapshot);
        return;
    }
    CHECK(strcmp(snapshot.endpoints[0].local_address, "127.0.0.1") == 0);
    CHECK(snapshot.endpoints[0].local_port == 3000U);
    CHECK(snapshot.endpoints[0].inode == 11111U);
    CHECK(snapshot.endpoints[1].family == AF_INET6);
    CHECK(snapshot.endpoints[1].local_port == 5353U);
    CHECK(snapshot.endpoints[1].protocol == OD_PROTOCOL_UDP);
    CHECK(strcmp(snapshot.endpoints[0].stable_id, snapshot.endpoints[1].stable_id) != 0);
    od_scan_snapshot_free(&snapshot);
}

static void test_socket_inode_target(void) {
    uint64_t inode = 0U;
    CHECK(od_parse_socket_inode("socket:[987654]", &inode));
    CHECK(inode == 987654U);
    CHECK(!od_parse_socket_inode("pipe:[987654]", &inode));
    CHECK(!od_parse_socket_inode("socket:[broken]", &inode));
}

static void test_process_owner_fixture_and_permission_limit(void) {
    char root[] = "/tmp/opendoor-proc-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char process_dir[512];
    char fd_dir[512];
    char path[512];
    (void)snprintf(process_dir, sizeof(process_dir), "%s/123", root);
    (void)snprintf(fd_dir, sizeof(fd_dir), "%s/123/fd", root);
    CHECK(mkdir(process_dir, 0700) == 0);
    CHECK(mkdir(fd_dir, 0700) == 0);
    (void)snprintf(path, sizeof(path), "%s/123/fd/3", root);
    CHECK(symlink("socket:[11111]", path) == 0);
    (void)snprintf(path, sizeof(path), "%s/123/comm", root);
    FILE *comm = fopen(path, "wb");
    CHECK(comm != NULL);
    if (comm != NULL) {
        (void)fputs("fixture-api\n", comm);
        (void)fclose(comm);
    }
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 2U);
    snapshot.endpoints = calloc(2U, sizeof(*snapshot.endpoints));
    snapshot.endpoint_count = 2U;
    snapshot.endpoints[0].inode = 11111U;
    snapshot.endpoints[0].uid = getuid();
    snapshot.endpoints[1].inode = 22222U;
    snapshot.endpoints[1].uid = getuid();
    CHECK(od_resolve_process_owners(root, &snapshot, &error) == OD_OK);
    CHECK(snapshot.endpoints[0].pid == 123);
    CHECK(strcmp(snapshot.endpoints[0].process, "fixture-api") == 0);

    CHECK(chmod(fd_dir, 0000) == 0);
    snapshot.endpoints[0].pid = 0;
    CHECK(od_resolve_process_owners(root, &snapshot, &error) == OD_OK);
    CHECK(snapshot.process_permissions_limited);
    CHECK(snapshot.warning_count == 1U);
    CHECK(chmod(fd_dir, 0700) == 0);
    od_scan_snapshot_free(&snapshot);

    (void)snprintf(path, sizeof(path), "%s/123/fd/3", root);
    (void)unlink(path);
    (void)rmdir(fd_dir);
    (void)snprintf(path, sizeof(path), "%s/123/comm", root);
    (void)unlink(path);
    (void)rmdir(process_dir);
    (void)rmdir(root);
}

static void test_docker_json_lines(void) {
    const char *lines =
        "{\"ID\":\"abc123\",\"Names\":\"api\",\"Ports\":\"0.0.0.0:8080->80/tcp, [::]:8080->80/tcp\",\"Labels\":\"com.docker.compose.project=demo,com.docker.compose.service=api\"}\n"
        "{\"ID\":\"def456\",\"Names\":\"dns\",\"Ports\":\"127.0.0.1:5353->53/udp\",\"Labels\":\"\"}\n";
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 1U);
    OdStatus status = od_docker_parse_ps_json_lines(lines, strlen(lines), &snapshot, &error);
    CHECK(status == OD_OK);
    CHECK(snapshot.docker_available);
    CHECK(snapshot.docker_mapping_count == 3U);
    if (snapshot.docker_mapping_count != 3U) {
        od_scan_snapshot_free(&snapshot);
        return;
    }
    CHECK(strcmp(snapshot.docker_mappings[0].project, "demo") == 0);
    CHECK(strcmp(snapshot.docker_mappings[0].service, "api") == 0);
    CHECK(snapshot.docker_mappings[0].host_port == 8080U);
    CHECK(snapshot.docker_mappings[2].protocol == OD_PROTOCOL_UDP);
    CHECK(od_docker_parse_ps_json_lines("{bad json}\n", 11U, &snapshot, &error) == OD_ERROR_INVALID);
    od_scan_snapshot_free(&snapshot);
}

static void test_docker_absent_is_nonfatal(void) {
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 1U);
    CHECK(od_docker_scan("/definitely/not/a/docker/binary", 100U, 4096U,
                         &snapshot, &error) == OD_OK);
    CHECK(!snapshot.docker_available);
    CHECK(snapshot.warning_count == 1U);
    od_scan_snapshot_free(&snapshot);

    od_scan_snapshot_init(&snapshot, 1U);
    CHECK(od_docker_scan("/bin/false", 1000U, 4096U, &snapshot, &error) == OD_OK);
    CHECK(!snapshot.docker_available);
    CHECK(snapshot.warning_count == 1U);
    od_scan_snapshot_free(&snapshot);
}

static void test_many_docker_containers(void) {
    const size_t containers = 200U;
    const size_t line_size = 160U;
    char *lines = calloc(containers, line_size);
    CHECK(lines != NULL);
    if (lines == NULL) return;
    size_t used = 0U;
    for (size_t index = 0U; index < containers; ++index) {
        int count = snprintf(lines + used, containers * line_size - used,
                             "{\"ID\":\"id%zu\",\"Names\":\"c%zu\",\"Ports\":\"0.0.0.0:%zu->80/tcp\",\"Labels\":\"\"}\n",
                             index, index, 10000U + index);
        CHECK(count > 0);
        used += (size_t)count;
    }
    OdScanSnapshot snapshot;
    OdError error;
    od_scan_snapshot_init(&snapshot, 3U);
    CHECK(od_docker_parse_ps_json_lines(lines, used, &snapshot, &error) == OD_OK);
    CHECK(snapshot.docker_mapping_count == containers);
    od_scan_snapshot_free(&snapshot);
    free(lines);
}

static void test_project_discovery_and_merge(void) {
    const char *compose =
        "services:\n"
        "  api:\n"
        "    ports:\n"
        "      - \"${API_HOST_PORT:-3000}:3000\"\n"
        "  redis:\n"
        "    ports:\n"
        "      - \"${REDIS_PORT:-6379}:6379\"\n";
    const char *dotenv = "API_HOST_PORT=3000\nMETRICS_PORT=9090\nNOT_A_PORT=word\n";
    const char *package =
        "{\"name\":\"web-ui\",\"scripts\":{\"dev\":\"vite --port 5173\",\"preview\":\"vite preview --port=4173\"}}";
    const char *makefile = "ADMIN_PORT ?= 8081\nrun:\n\tserver --port $(ADMIN_PORT)\n";
    OdCandidateList compose_candidates;
    OdCandidateList dotenv_candidates;
    OdCandidateList package_candidates;
    OdCandidateList make_candidates;
    OdError error;
    od_candidate_list_init(&compose_candidates);
    od_candidate_list_init(&dotenv_candidates);
    od_candidate_list_init(&package_candidates);
    od_candidate_list_init(&make_candidates);
    CHECK(od_discover_compose_text("compose.yaml", compose, &compose_candidates, &error) == OD_OK);
    CHECK(compose_candidates.count == 2U);
    CHECK(od_discover_dotenv_text(".env", dotenv, &dotenv_candidates, &error) == OD_OK);
    CHECK(dotenv_candidates.count == 2U);
    CHECK(od_discover_package_json("package.json", package, &package_candidates, &error) == OD_OK);
    CHECK(package_candidates.count == 2U);
    CHECK(od_discover_makefile_text("Makefile", makefile, &make_candidates, &error) == OD_OK);
    CHECK(make_candidates.count == 1U);
    if (compose_candidates.count != 2U || dotenv_candidates.count != 2U ||
        package_candidates.count != 2U || make_candidates.count != 1U) {
        od_candidate_list_free(&compose_candidates);
        od_candidate_list_free(&dotenv_candidates);
        od_candidate_list_free(&package_candidates);
        od_candidate_list_free(&make_candidates);
        return;
    }
    CHECK(compose_candidates.items[0].confidence == OD_CONFIDENCE_CONFIRMED);
    CHECK(compose_candidates.items[0].selected);
    CHECK(package_candidates.items[0].confidence == OD_CONFIDENCE_LIKELY);
    CHECK(make_candidates.items[0].confidence == OD_CONFIDENCE_POSSIBLE);
    CHECK(!make_candidates.items[0].selected);

    char compose_api_id[40];
    (void)strcpy(compose_api_id, compose_candidates.items[0].stable_id);
    CHECK(od_candidates_merge(&compose_candidates, &dotenv_candidates, &error) == OD_OK);
    CHECK(compose_candidates.count == 3U);
    CHECK(strcmp(compose_candidates.items[0].stable_id, compose_api_id) == 0);
    CHECK(compose_candidates.items[0].sources.count == 2U);
    CHECK(od_candidates_merge(&compose_candidates, &package_candidates, &error) == OD_OK);
    CHECK(od_candidates_merge(&compose_candidates, &make_candidates, &error) == OD_OK);
    CHECK(compose_candidates.count == 6U);

    od_candidate_list_free(&compose_candidates);
    od_candidate_list_free(&dotenv_candidates);
    od_candidate_list_free(&package_candidates);
    od_candidate_list_free(&make_candidates);
}

int main(void) {
    test_proc_tcp_udp_ipv4_ipv6_and_deduplication();
    test_socket_inode_target();
    test_process_owner_fixture_and_permission_limit();
    test_docker_json_lines();
    test_docker_absent_is_nonfatal();
    test_many_docker_containers();
    test_project_discovery_and_merge();
    if (failures != 0) {
        fprintf(stderr, "%d discovery checks failed\n", failures);
        return 1;
    }
    puts("discovery checks passed");
    return 0;
}
