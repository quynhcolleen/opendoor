#ifndef OPENDOOR_SCAN_H
#define OPENDOOR_SCAN_H

#include "opendoor/common.h"
#include "opendoor/model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define OD_ADDRESS_CAP 64U
#define OD_PROCESS_CAP 64U
#define OD_COMMAND_CAP 256U
#define OD_PATH_CAP 4096U
#define OD_USER_CAP 64U

typedef struct {
    char stable_id[40];
    int family;
    unsigned protocol;
    unsigned state;
    char local_address[OD_ADDRESS_CAP];
    char remote_address[OD_ADDRESS_CAP];
    uint16_t local_port;
    uint16_t remote_port;
    uid_t uid;
    uint64_t inode;
    pid_t pid;
    char process[OD_PROCESS_CAP];
    char executable[OD_PATH_CAP];
    char command[OD_COMMAND_CAP];
    char user[OD_USER_CAP];
    bool permission_limited;
} OdEndpoint;

typedef struct {
    char container[128];
    char container_id[80];
    char project[128];
    char service[128];
    char bind_address[OD_ADDRESS_CAP];
    uint16_t host_port;
    uint16_t container_port;
    unsigned protocol;
} OdDockerMapping;

typedef struct {
    uint64_t generation;
    OdEndpoint *endpoints;
    size_t endpoint_count;
    OdDockerMapping *docker_mappings;
    size_t docker_mapping_count;
    char **warnings;
    size_t warning_count;
    bool docker_available;
    bool process_permissions_limited;
} OdScanSnapshot;

void od_scan_snapshot_init(OdScanSnapshot *snapshot, uint64_t generation);
void od_scan_snapshot_free(OdScanSnapshot *snapshot);
OdStatus od_scan_snapshot_add_warning(OdScanSnapshot *snapshot, const char *warning, OdError *error);
OdStatus od_parse_proc_net(const char *text,
                           int family,
                           unsigned protocol,
                           OdScanSnapshot *snapshot,
                           OdError *error);
bool od_parse_socket_inode(const char *target, uint64_t *inode);
OdStatus od_scan_host(OdScanSnapshot *snapshot, OdError *error);
OdStatus od_resolve_process_owners(const char *proc_root,
                                   OdScanSnapshot *snapshot,
                                   OdError *error);

#endif

