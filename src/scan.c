#include "opendoor/scan.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define OD_PROC_FILE_LIMIT (32U * 1024U * 1024U)

static uint64_t fnv1a(const void *data, size_t length, uint64_t hash) {
    const unsigned char *bytes = data;
    for (size_t index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void assign_stable_id(OdEndpoint *endpoint) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = fnv1a(&endpoint->family, sizeof(endpoint->family), hash);
    hash = fnv1a(&endpoint->protocol, sizeof(endpoint->protocol), hash);
    hash = fnv1a(endpoint->local_address, strlen(endpoint->local_address), hash);
    hash = fnv1a(&endpoint->local_port, sizeof(endpoint->local_port), hash);
    hash = fnv1a(&endpoint->inode, sizeof(endpoint->inode), hash);
    (void)snprintf(endpoint->stable_id, sizeof(endpoint->stable_id),
                   "ep-%016llx", (unsigned long long)hash);
}

void od_scan_snapshot_init(OdScanSnapshot *snapshot, uint64_t generation) {
    if (snapshot != NULL) {
        *snapshot = (OdScanSnapshot){0};
        snapshot->generation = generation;
    }
}

void od_scan_snapshot_free(OdScanSnapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->endpoints);
    free(snapshot->docker_mappings);
    for (size_t index = 0U; index < snapshot->warning_count; ++index) {
        free(snapshot->warnings[index]);
    }
    free(snapshot->warnings);
    *snapshot = (OdScanSnapshot){0};
}

OdStatus od_scan_snapshot_add_warning(OdScanSnapshot *snapshot, const char *warning, OdError *error) {
    if (snapshot == NULL || warning == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "snapshot and warning are required");
        return OD_ERROR_INVALID;
    }
    for (size_t index = 0U; index < snapshot->warning_count; ++index) {
        if (strcmp(snapshot->warnings[index], warning) == 0) {
            return OD_OK;
        }
    }
    char **warnings = realloc(snapshot->warnings,
                              (snapshot->warning_count + 1U) * sizeof(*warnings));
    if (warnings == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store scan warning");
        return OD_ERROR_MEMORY;
    }
    snapshot->warnings = warnings;
    warnings[snapshot->warning_count] = strdup(warning);
    if (warnings[snapshot->warning_count] == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy scan warning");
        return OD_ERROR_MEMORY;
    }
    ++snapshot->warning_count;
    return OD_OK;
}

static bool same_endpoint(const OdEndpoint *left, const OdEndpoint *right) {
    return left->family == right->family && left->protocol == right->protocol &&
           left->local_port == right->local_port && left->remote_port == right->remote_port &&
           left->inode == right->inode &&
           strcmp(left->local_address, right->local_address) == 0 &&
           strcmp(left->remote_address, right->remote_address) == 0;
}

static OdStatus add_endpoint(OdScanSnapshot *snapshot,
                             const OdEndpoint *endpoint,
                             OdError *error) {
    if (snapshot->endpoint_count == snapshot->endpoint_capacity) {
        size_t capacity = snapshot->endpoint_capacity == 0U ? 64U :
                          snapshot->endpoint_capacity * 2U;
        if (capacity <= snapshot->endpoint_count ||
            capacity > SIZE_MAX / sizeof(*snapshot->endpoints)) {
            od_error_set(error, OD_ERROR_MEMORY, "endpoint collection is too large");
            return OD_ERROR_MEMORY;
        }
        OdEndpoint *items = realloc(snapshot->endpoints,
                                    capacity * sizeof(*items));
        if (items == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to store endpoint");
            return OD_ERROR_MEMORY;
        }
        snapshot->endpoints = items;
        snapshot->endpoint_capacity = capacity;
    }
    snapshot->endpoints[snapshot->endpoint_count] = *endpoint;
    ++snapshot->endpoint_count;
    return OD_OK;
}

static OdStatus deduplicate_endpoints(OdScanSnapshot *snapshot, OdError *error) {
    if (snapshot->endpoint_count < 2U) return OD_OK;
    if (snapshot->endpoint_count > SIZE_MAX / 2U) {
        od_error_set(error, OD_ERROR_MEMORY, "endpoint index is too large");
        return OD_ERROR_MEMORY;
    }
    size_t table_size = 1U;
    size_t required = snapshot->endpoint_count * 2U;
    while (table_size < required) {
        if (table_size > SIZE_MAX / 2U) {
            od_error_set(error, OD_ERROR_MEMORY, "endpoint index is too large");
            return OD_ERROR_MEMORY;
        }
        table_size *= 2U;
    }
    size_t *table = calloc(table_size, sizeof(*table));
    if (table == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to index endpoints");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        uint64_t hash = fnv1a(endpoint->stable_id, strlen(endpoint->stable_id),
                              UINT64_C(1469598103934665603));
        size_t slot = (size_t)hash & (table_size - 1U);
        bool duplicate = false;
        while (table[slot] != 0U) {
            size_t existing = table[slot] - 1U;
            if (same_endpoint(&snapshot->endpoints[existing], endpoint)) {
                duplicate = true;
                break;
            }
            slot = (slot + 1U) & (table_size - 1U);
        }
        if (duplicate) continue;
        if (output != index) snapshot->endpoints[output] = *endpoint;
        table[slot] = output + 1U;
        ++output;
    }
    snapshot->endpoint_count = output;
    free(table);
    return OD_OK;
}

static bool parse_hex_byte(const char *text, unsigned char *byte) {
    char value[3] = {text[0], text[1], '\0'};
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 16);
    if (errno != 0 || end == value || *end != '\0' || parsed > 255UL) {
        return false;
    }
    *byte = (unsigned char)parsed;
    return true;
}

static bool proc_address(const char *hex, int family, char *output, size_t output_size) {
    unsigned char bytes[16] = {0};
    size_t expected = family == AF_INET ? 8U : 32U;
    if (strlen(hex) != expected) {
        return false;
    }
    size_t chunks = family == AF_INET ? 1U : 4U;
    for (size_t chunk = 0U; chunk < chunks; ++chunk) {
        for (size_t byte = 0U; byte < 4U; ++byte) {
            size_t source = chunk * 8U + (3U - byte) * 2U;
            if (!parse_hex_byte(hex + source, &bytes[chunk * 4U + byte])) {
                return false;
            }
        }
    }
    return inet_ntop(family, bytes, output, (socklen_t)output_size) != NULL;
}

static bool split_endpoint(char *value,
                           int family,
                           char *address,
                           size_t address_size,
                           uint16_t *port) {
    char *separator = strrchr(value, ':');
    if (separator == NULL) {
        return false;
    }
    *separator = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long parsed_port = strtoul(separator + 1, &end, 16);
    if (errno != 0 || end == separator + 1 || *end != '\0' || parsed_port > 65535UL ||
        !proc_address(value, family, address, address_size)) {
        return false;
    }
    *port = (uint16_t)parsed_port;
    return true;
}

OdStatus od_parse_proc_net(const char *text,
                           int family,
                           unsigned protocol,
                           OdScanSnapshot *snapshot,
                           OdError *error) {
    if (text == NULL || snapshot == NULL || (family != AF_INET && family != AF_INET6) ||
        (protocol != OD_PROTOCOL_TCP && protocol != OD_PROTOCOL_UDP)) {
        od_error_set(error, OD_ERROR_INVALID, "invalid proc network input");
        return OD_ERROR_INVALID;
    }
    char *buffer = strdup(text);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse proc network table");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = OD_OK;
    char *line_save = NULL;
    for (char *line = strtok_r(buffer, "\n", &line_save);
         line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &line_save)) {
        while (isspace((unsigned char)*line)) ++line;
        if (*line == '\0' || strncmp(line, "sl", 2U) == 0) {
            continue;
        }
        char *fields[16] = {0};
        size_t field_count = 0U;
        char *field_save = NULL;
        for (char *field = strtok_r(line, " \t", &field_save);
             field != NULL && field_count < 16U;
             field = strtok_r(NULL, " \t", &field_save)) {
            fields[field_count++] = field;
        }
        if (field_count < 10U) {
            od_error_set(error, OD_ERROR_INVALID, "malformed proc network row");
            status = OD_ERROR_INVALID;
            break;
        }
        OdEndpoint endpoint = {0};
        endpoint.family = family;
        endpoint.protocol = protocol;
        if (!split_endpoint(fields[1], family, endpoint.local_address,
                            sizeof(endpoint.local_address), &endpoint.local_port) ||
            !split_endpoint(fields[2], family, endpoint.remote_address,
                            sizeof(endpoint.remote_address), &endpoint.remote_port)) {
            od_error_set(error, OD_ERROR_INVALID, "malformed proc endpoint");
            status = OD_ERROR_INVALID;
            break;
        }
        char *end = NULL;
        endpoint.state = (unsigned)strtoul(fields[3], &end, 16);
        if (end == fields[3] || *end != '\0') {
            od_error_set(error, OD_ERROR_INVALID, "malformed proc socket state");
            status = OD_ERROR_INVALID;
            break;
        }
        endpoint.uid = (uid_t)strtoul(fields[7], &end, 10);
        if (end == fields[7] || *end != '\0') {
            od_error_set(error, OD_ERROR_INVALID, "malformed proc socket uid");
            status = OD_ERROR_INVALID;
            break;
        }
        endpoint.inode = strtoull(fields[9], &end, 10);
        if (end == fields[9] || *end != '\0') {
            od_error_set(error, OD_ERROR_INVALID, "malformed proc socket inode");
            status = OD_ERROR_INVALID;
            break;
        }
        assign_stable_id(&endpoint);
        status = add_endpoint(snapshot, &endpoint, error);
    }
    free(buffer);
    if (status == OD_OK) status = deduplicate_endpoints(snapshot, error);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

bool od_parse_socket_inode(const char *target, uint64_t *inode) {
    if (target == NULL || inode == NULL || strncmp(target, "socket:[", 8U) != 0) {
        return false;
    }
    const char *digits = target + 8;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(digits, &end, 10);
    if (errno != 0 || end == digits || strcmp(end, "]") != 0) {
        return false;
    }
    *inode = (uint64_t)value;
    return true;
}

static void diag_address(int family, const uint32_t address[4], char *output, size_t output_size) {
    if (inet_ntop(family, address, output, (socklen_t)output_size) == NULL) {
        (void)strcpy(output, "?");
    }
}

static OdStatus scan_netlink_query(int socket_fd,
                                   int family,
                                   int ip_protocol,
                                   uint32_t sequence,
                                   OdScanSnapshot *snapshot,
                                   OdError *error) {
    struct {
        struct nlmsghdr header;
        struct inet_diag_req_v2 request;
    } message;
    memset(&message, 0, sizeof(message));
    message.header.nlmsg_len = (uint32_t)sizeof(message);
    message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    message.header.nlmsg_seq = sequence;
    message.request.sdiag_family = (uint8_t)family;
    message.request.sdiag_protocol = (uint8_t)ip_protocol;
    message.request.idiag_states = UINT32_MAX;
    if (send(socket_fd, &message, sizeof(message), 0) < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to request kernel socket diagnostics");
        return OD_ERROR_IO;
    }

    unsigned char buffer[65536];
    bool done = false;
    while (!done) {
        ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            od_error_set(error, OD_ERROR_IO, "unable to receive kernel socket diagnostics");
            return OD_ERROR_IO;
        }
        for (struct nlmsghdr *header = (struct nlmsghdr *)buffer;
             NLMSG_OK(header, (unsigned)received);
             header = NLMSG_NEXT(header, received)) {
            if (header->nlmsg_seq != sequence) continue;
            if (header->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                od_error_set(error, OD_ERROR_IO, "kernel rejected socket diagnostics request");
                return OD_ERROR_IO;
            }
            if (header->nlmsg_type != SOCK_DIAG_BY_FAMILY ||
                header->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg))) {
                continue;
            }
            const struct inet_diag_msg *diagnostic = NLMSG_DATA(header);
            OdEndpoint endpoint = {0};
            endpoint.family = family;
            endpoint.protocol = ip_protocol == IPPROTO_TCP ? OD_PROTOCOL_TCP : OD_PROTOCOL_UDP;
            endpoint.state = diagnostic->idiag_state;
            endpoint.local_port = ntohs(diagnostic->id.idiag_sport);
            endpoint.remote_port = ntohs(diagnostic->id.idiag_dport);
            endpoint.uid = diagnostic->idiag_uid;
            endpoint.inode = diagnostic->idiag_inode;
            diag_address(family, diagnostic->id.idiag_src, endpoint.local_address,
                         sizeof(endpoint.local_address));
            diag_address(family, diagnostic->id.idiag_dst, endpoint.remote_address,
                         sizeof(endpoint.remote_address));
            assign_stable_id(&endpoint);
            OdStatus status = add_endpoint(snapshot, &endpoint, error);
            if (status != OD_OK) return status;
        }
    }
    return OD_OK;
}

static OdStatus scan_netlink(OdScanSnapshot *snapshot, OdError *error) {
    int socket_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (socket_fd < 0) {
        od_error_set(error, OD_ERROR_IO, "NETLINK_INET_DIAG is unavailable");
        return OD_ERROR_IO;
    }
    const int families[] = {AF_INET, AF_INET6};
    const int protocols[] = {IPPROTO_TCP, IPPROTO_UDP};
    OdStatus status = OD_OK;
    uint32_t sequence = 1U;
    for (size_t family = 0U; family < 2U && status == OD_OK; ++family) {
        for (size_t protocol = 0U; protocol < 2U && status == OD_OK; ++protocol) {
            status = scan_netlink_query(socket_fd, families[family], protocols[protocol],
                                        sequence++, snapshot, error);
        }
    }
    if (status == OD_OK) status = deduplicate_endpoints(snapshot, error);
    (void)close(socket_fd);
    return status;
}

static OdStatus read_whole_file(const char *path, char **text, OdError *error) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        od_error_set(error, OD_ERROR_IO, "unable to open %s", path);
        return OD_ERROR_IO;
    }
    size_t capacity = 16384U;
    size_t length = 0U;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        fclose(file);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read %s", path);
        return OD_ERROR_MEMORY;
    }
    while (!feof(file)) {
        if (length == capacity) {
            if (capacity >= OD_PROC_FILE_LIMIT) {
                free(buffer);
                fclose(file);
                od_error_set(error, OD_ERROR_INVALID, "%s exceeds scan limit", path);
                return OD_ERROR_INVALID;
            }
            capacity *= 2U;
            char *grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                fclose(file);
                od_error_set(error, OD_ERROR_MEMORY, "unable to grow read buffer");
                return OD_ERROR_MEMORY;
            }
            buffer = grown;
        }
        length += fread(buffer + length, 1U, capacity - length, file);
        if (ferror(file)) {
            free(buffer);
            fclose(file);
            od_error_set(error, OD_ERROR_IO, "unable to read %s", path);
            return OD_ERROR_IO;
        }
    }
    fclose(file);
    char *terminated = realloc(buffer, length + 1U);
    if (terminated == NULL) {
        free(buffer);
        od_error_set(error, OD_ERROR_MEMORY, "unable to terminate read buffer");
        return OD_ERROR_MEMORY;
    }
    terminated[length] = '\0';
    *text = terminated;
    return OD_OK;
}

static OdStatus scan_proc(OdScanSnapshot *snapshot, OdError *error) {
    struct {
        const char *path;
        int family;
        unsigned protocol;
    } files[] = {
        {"/proc/net/tcp", AF_INET, OD_PROTOCOL_TCP},
        {"/proc/net/tcp6", AF_INET6, OD_PROTOCOL_TCP},
        {"/proc/net/udp", AF_INET, OD_PROTOCOL_UDP},
        {"/proc/net/udp6", AF_INET6, OD_PROTOCOL_UDP}
    };
    OdStatus status = OD_OK;
    size_t successful = 0U;
    for (size_t index = 0U; index < sizeof(files) / sizeof(files[0]); ++index) {
        char *text = NULL;
        OdStatus read_status = read_whole_file(files[index].path, &text, error);
        if (read_status != OD_OK) {
            continue;
        }
        status = od_parse_proc_net(text, files[index].family, files[index].protocol,
                                   snapshot, error);
        free(text);
        if (status != OD_OK) return status;
        ++successful;
    }
    if (successful == 0U) {
        od_error_set(error, OD_ERROR_IO, "no proc socket tables were readable");
        return OD_ERROR_IO;
    }
    return OD_OK;
}

OdStatus od_scan_host(OdScanSnapshot *snapshot, OdError *error) {
    if (snapshot == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "scan snapshot is required");
        return OD_ERROR_INVALID;
    }
    size_t original_count = snapshot->endpoint_count;
    OdStatus status = scan_netlink(snapshot, error);
    if (status != OD_OK) {
        snapshot->endpoint_count = original_count;
        OdStatus warning_status = od_scan_snapshot_add_warning(
            snapshot, "NETLINK_INET_DIAG unavailable; using /proc socket tables", error);
        if (warning_status != OD_OK) return warning_status;
        status = scan_proc(snapshot, error);
    }
    if (status == OD_OK) {
        status = od_resolve_process_owners("/proc", snapshot, error);
    }
    return status;
}

static bool decimal_name(const char *name) {
    if (name[0] == '\0') return false;
    for (size_t index = 0U; name[index] != '\0'; ++index) {
        if (!isdigit((unsigned char)name[index])) return false;
    }
    return true;
}

static void sanitize_field(char *text) {
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)text[index];
        if (character < 0x20U || character == 0x7fU) text[index] = ' ';
    }
}

static void read_process_metadata(const char *proc_root, pid_t pid, OdEndpoint *endpoint) {
    char path[OD_PATH_CAP];
    (void)snprintf(path, sizeof(path), "%s/%ld/comm", proc_root, (long)pid);
    FILE *comm = fopen(path, "rb");
    if (comm != NULL) {
        if (fgets(endpoint->process, sizeof(endpoint->process), comm) != NULL) {
            endpoint->process[strcspn(endpoint->process, "\r\n")] = '\0';
            sanitize_field(endpoint->process);
        }
        fclose(comm);
    }
    (void)snprintf(path, sizeof(path), "%s/%ld/exe", proc_root, (long)pid);
    ssize_t exe_length = readlink(path, endpoint->executable, sizeof(endpoint->executable) - 1U);
    if (exe_length >= 0) {
        endpoint->executable[(size_t)exe_length] = '\0';
        sanitize_field(endpoint->executable);
    }
    (void)snprintf(path, sizeof(path), "%s/%ld/cmdline", proc_root, (long)pid);
    int command_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (command_fd >= 0) {
        ssize_t command_length = read(command_fd, endpoint->command, sizeof(endpoint->command) - 1U);
        (void)close(command_fd);
        if (command_length > 0) {
            size_t length = (size_t)command_length;
            for (size_t index = 0U; index < length; ++index) {
                unsigned char character = (unsigned char)endpoint->command[index];
                if (character < 0x20U || character == 0x7fU) {
                    endpoint->command[index] = ' ';
                }
            }
            while (length > 0U && endpoint->command[length - 1U] == ' ') --length;
            endpoint->command[length] = '\0';
        }
    }
    struct passwd pwd;
    struct passwd *result = NULL;
    char buffer[4096];
    if (getpwuid_r(endpoint->uid, &pwd, buffer, sizeof(buffer), &result) == 0 && result != NULL) {
        (void)snprintf(endpoint->user, sizeof(endpoint->user), "%s", result->pw_name);
    } else {
        (void)snprintf(endpoint->user, sizeof(endpoint->user), "%lu",
                       (unsigned long)endpoint->uid);
    }
}

typedef struct {
    uint64_t inode;
    size_t endpoint_index;
} InodeTarget;

static int compare_inode_targets(const void *left_value, const void *right_value) {
    const InodeTarget *left = left_value;
    const InodeTarget *right = right_value;
    if (left->inode == right->inode) return 0;
    return left->inode < right->inode ? -1 : 1;
}

static size_t inode_lower_bound(const InodeTarget *targets,
                                size_t count,
                                uint64_t inode) {
    size_t left = 0U;
    size_t right = count;
    while (left < right) {
        size_t middle = left + (right - left) / 2U;
        if (targets[middle].inode < inode) {
            left = middle + 1U;
        } else {
            right = middle;
        }
    }
    return left;
}

static void attach_inode(OdScanSnapshot *snapshot,
                         const InodeTarget *targets,
                         size_t target_count,
                         uint64_t inode,
                         const char *proc_root,
                         pid_t pid) {
    size_t index = inode_lower_bound(targets, target_count, inode);
    while (index < target_count && targets[index].inode == inode) {
        OdEndpoint *endpoint = &snapshot->endpoints[targets[index].endpoint_index];
        if (endpoint->pid == 0) {
            endpoint->pid = pid;
            read_process_metadata(proc_root, pid, endpoint);
        }
        ++index;
    }
}

OdStatus od_resolve_process_owners(const char *proc_root,
                                   OdScanSnapshot *snapshot,
                                   OdError *error) {
    if (proc_root == NULL || snapshot == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "proc root and snapshot are required");
        return OD_ERROR_INVALID;
    }
    InodeTarget *targets = NULL;
    if (snapshot->endpoint_count > 0U) {
        targets = calloc(snapshot->endpoint_count, sizeof(*targets));
        if (targets == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to index socket inodes");
            return OD_ERROR_MEMORY;
        }
        for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
            targets[index] = (InodeTarget){snapshot->endpoints[index].inode, index};
        }
        qsort(targets, snapshot->endpoint_count, sizeof(*targets),
              compare_inode_targets);
    }
    DIR *root = opendir(proc_root);
    if (root == NULL) {
        free(targets);
        od_error_set(error, OD_ERROR_IO, "unable to open %s", proc_root);
        return OD_ERROR_IO;
    }
    bool limited = false;
    struct dirent *process_entry;
    while ((process_entry = readdir(root)) != NULL) {
        if (!decimal_name(process_entry->d_name)) continue;
        char fd_path[OD_PATH_CAP];
        int count = snprintf(fd_path, sizeof(fd_path), "%s/%s/fd", proc_root,
                             process_entry->d_name);
        if (count < 0 || (size_t)count >= sizeof(fd_path)) continue;
        DIR *fds = opendir(fd_path);
        if (fds == NULL) {
            if (errno == EACCES || errno == EPERM) limited = true;
            continue;
        }
        pid_t pid = (pid_t)strtol(process_entry->d_name, NULL, 10);
        struct dirent *fd_entry;
        while ((fd_entry = readdir(fds)) != NULL) {
            if (fd_entry->d_name[0] == '.') continue;
            char link_path[OD_PATH_CAP];
            count = snprintf(link_path, sizeof(link_path), "%s/%s", fd_path,
                             fd_entry->d_name);
            if (count < 0 || (size_t)count >= sizeof(link_path)) continue;
            char target[128];
            ssize_t target_length = readlink(link_path, target, sizeof(target) - 1U);
            if (target_length < 0) {
                if (errno == EACCES || errno == EPERM) limited = true;
                continue;
            }
            target[(size_t)target_length] = '\0';
            uint64_t inode = 0U;
            if (od_parse_socket_inode(target, &inode)) {
                attach_inode(snapshot, targets, snapshot->endpoint_count,
                             inode, proc_root, pid);
            }
        }
        closedir(fds);
    }
    closedir(root);
    free(targets);
    if (limited) {
        snapshot->process_permissions_limited = true;
        for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
            if (snapshot->endpoints[index].pid == 0) {
                snapshot->endpoints[index].permission_limited = true;
            }
        }
        OdStatus status = od_scan_snapshot_add_warning(
            snapshot, "Some process owners are hidden by /proc permissions", error);
        if (status != OD_OK) return status;
    }
    od_error_clear(error);
    return OD_OK;
}
