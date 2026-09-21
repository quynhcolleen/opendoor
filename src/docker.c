#include "opendoor/docker.h"

#define JSMN_HEADER
#include "jsmn.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool token_equals(const char *json, const jsmntok_t *token, const char *value) {
    size_t length = strlen(value);
    return token->type == JSMN_STRING && token->start >= 0 && token->end >= token->start &&
           (size_t)(token->end - token->start) == length &&
           strncmp(json + token->start, value, length) == 0;
}

static bool json_string_field(const char *json,
                              const jsmntok_t *tokens,
                              int token_count,
                              const char *key,
                              char *output,
                              size_t output_size) {
    for (int index = 1; index + 1 < token_count; ++index) {
        if (!token_equals(json, &tokens[index], key)) continue;
        const jsmntok_t *value = &tokens[index + 1];
        if (value->type != JSMN_STRING || value->start < 0 || value->end < value->start) {
            return false;
        }
        size_t source_length = (size_t)(value->end - value->start);
        size_t destination = 0U;
        bool escaped = false;
        for (size_t source = 0U; source < source_length; ++source) {
            char character = json[(size_t)value->start + source];
            if (escaped) {
                if (character == 'n' || character == 'r' || character == 't') {
                    character = ' ';
                }
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
                continue;
            }
            if (destination + 1U < output_size) output[destination++] = character;
        }
        output[destination] = '\0';
        return true;
    }
    return false;
}

static void label_value(const char *labels, const char *key, char *output, size_t output_size) {
    output[0] = '\0';
    char *copy = strdup(labels);
    if (copy == NULL) return;
    char *save = NULL;
    for (char *entry = strtok_r(copy, ",", &save); entry != NULL;
         entry = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*entry)) ++entry;
        char *equals = strchr(entry, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        if (strcmp(entry, key) == 0) {
            (void)snprintf(output, output_size, "%s", equals + 1);
            break;
        }
    }
    free(copy);
}

static char *trim(char *value) {
    while (isspace((unsigned char)*value)) ++value;
    size_t length = strlen(value);
    while (length > 0U && isspace((unsigned char)value[length - 1U])) {
        value[--length] = '\0';
    }
    return value;
}

static bool parse_decimal_port(const char *value, uint16_t *port) {
    char *end = NULL;
    errno = 0;
    unsigned long numeric = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || numeric == 0UL || numeric > 65535UL) {
        return false;
    }
    *port = (uint16_t)numeric;
    return true;
}

static OdStatus add_mapping(OdScanSnapshot *snapshot,
                            const OdDockerMapping *mapping,
                            OdError *error) {
    if (snapshot->docker_mapping_count == snapshot->docker_mapping_capacity) {
        size_t capacity = snapshot->docker_mapping_capacity == 0U ? 32U :
                          snapshot->docker_mapping_capacity * 2U;
        if (capacity <= snapshot->docker_mapping_count ||
            capacity > SIZE_MAX / sizeof(*snapshot->docker_mappings)) {
            od_error_set(error, OD_ERROR_MEMORY, "Docker mapping collection is too large");
            return OD_ERROR_MEMORY;
        }
        OdDockerMapping *items = realloc(snapshot->docker_mappings,
                                         capacity * sizeof(*items));
        if (items == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to store Docker mapping");
            return OD_ERROR_MEMORY;
        }
        snapshot->docker_mappings = items;
        snapshot->docker_mapping_capacity = capacity;
    }
    snapshot->docker_mappings[snapshot->docker_mapping_count++] = *mapping;
    return OD_OK;
}

static bool same_mapping(const OdDockerMapping *left,
                         const OdDockerMapping *right) {
    return left->host_port == right->host_port &&
           left->container_port == right->container_port &&
           left->protocol == right->protocol &&
           strcmp(left->bind_address, right->bind_address) == 0 &&
           strcmp(left->container_id, right->container_id) == 0;
}

static uint64_t mapping_hash(const OdDockerMapping *mapping) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const char *values[] = {mapping->container_id, mapping->bind_address};
    for (size_t value = 0U; value < sizeof(values) / sizeof(values[0]); ++value) {
        for (size_t index = 0U; values[value][index] != '\0'; ++index) {
            hash ^= (unsigned char)values[value][index];
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= mapping->host_port;
    hash *= UINT64_C(1099511628211);
    hash ^= mapping->container_port;
    hash *= UINT64_C(1099511628211);
    hash ^= mapping->protocol;
    hash *= UINT64_C(1099511628211);
    return hash;
}

static OdStatus deduplicate_mappings(OdScanSnapshot *snapshot, OdError *error) {
    if (snapshot->docker_mapping_count < 2U) return OD_OK;
    if (snapshot->docker_mapping_count > SIZE_MAX / 2U) {
        od_error_set(error, OD_ERROR_MEMORY, "Docker mapping index is too large");
        return OD_ERROR_MEMORY;
    }
    size_t table_size = 1U;
    size_t required = snapshot->docker_mapping_count * 2U;
    while (table_size < required) {
        if (table_size > SIZE_MAX / 2U) {
            od_error_set(error, OD_ERROR_MEMORY, "Docker mapping index is too large");
            return OD_ERROR_MEMORY;
        }
        table_size *= 2U;
    }
    size_t *table = calloc(table_size, sizeof(*table));
    if (table == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to index Docker mappings");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        const OdDockerMapping *mapping = &snapshot->docker_mappings[index];
        size_t slot = (size_t)mapping_hash(mapping) & (table_size - 1U);
        bool duplicate = false;
        while (table[slot] != 0U) {
            size_t existing = table[slot] - 1U;
            if (same_mapping(&snapshot->docker_mappings[existing], mapping)) {
                duplicate = true;
                break;
            }
            slot = (slot + 1U) & (table_size - 1U);
        }
        if (duplicate) continue;
        if (output != index) snapshot->docker_mappings[output] = *mapping;
        table[slot] = output + 1U;
        ++output;
    }
    snapshot->docker_mapping_count = output;
    free(table);
    return OD_OK;
}

static OdStatus parse_port_entry(char *entry,
                                 const char *id,
                                 const char *name,
                                 const char *project,
                                 const char *service,
                                 OdScanSnapshot *snapshot,
                                 OdError *error) {
    char *arrow = strstr(entry, "->");
    if (arrow == NULL) return OD_OK;
    *arrow = '\0';
    char *host = trim(entry);
    char *container = trim(arrow + 2);
    char *slash = strrchr(container, '/');
    if (slash == NULL) return OD_OK;
    *slash = '\0';
    unsigned protocol;
    if (strcmp(slash + 1, "tcp") == 0) {
        protocol = OD_PROTOCOL_TCP;
    } else if (strcmp(slash + 1, "udp") == 0) {
        protocol = OD_PROTOCOL_UDP;
    } else {
        return OD_OK;
    }
    char *host_separator = strrchr(host, ':');
    if (host_separator == NULL) return OD_OK;
    *host_separator = '\0';
    char *host_port_text = host_separator + 1;
    char *bind = trim(host);
    if (bind[0] == '[') {
        size_t length = strlen(bind);
        if (length >= 2U && bind[length - 1U] == ']') {
            bind[length - 1U] = '\0';
            ++bind;
        }
    }
    OdDockerMapping mapping = {0};
    if (!parse_decimal_port(host_port_text, &mapping.host_port) ||
        !parse_decimal_port(container, &mapping.container_port)) {
        return OD_OK;
    }
    mapping.protocol = protocol;
    (void)snprintf(mapping.bind_address, sizeof(mapping.bind_address), "%s", bind);
    (void)snprintf(mapping.container_id, sizeof(mapping.container_id), "%s", id);
    (void)snprintf(mapping.container, sizeof(mapping.container), "%s", name);
    (void)snprintf(mapping.project, sizeof(mapping.project), "%s", project);
    (void)snprintf(mapping.service, sizeof(mapping.service), "%s", service);
    return add_mapping(snapshot, &mapping, error);
}

static OdStatus parse_json_line(char *line, OdScanSnapshot *snapshot, OdError *error) {
    jsmn_parser parser;
    jsmntok_t tokens[128];
    jsmn_init(&parser);
    int token_count = jsmn_parse(&parser, line, strlen(line), tokens,
                                 sizeof(tokens) / sizeof(tokens[0]));
    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        od_error_set(error, OD_ERROR_INVALID, "malformed Docker JSON output");
        return OD_ERROR_INVALID;
    }
    char id[80] = {0};
    char name[128] = {0};
    char ports[2048] = {0};
    char labels[4096] = {0};
    if (!json_string_field(line, tokens, token_count, "ID", id, sizeof(id)) ||
        !json_string_field(line, tokens, token_count, "Names", name, sizeof(name)) ||
        !json_string_field(line, tokens, token_count, "Ports", ports, sizeof(ports))) {
        od_error_set(error, OD_ERROR_INVALID, "Docker JSON row is missing required fields");
        return OD_ERROR_INVALID;
    }
    (void)json_string_field(line, tokens, token_count, "Labels", labels, sizeof(labels));
    char project[128];
    char service[128];
    label_value(labels, "com.docker.compose.project", project, sizeof(project));
    label_value(labels, "com.docker.compose.service", service, sizeof(service));

    char *save = NULL;
    OdStatus status = OD_OK;
    for (char *entry = strtok_r(ports, ",", &save); entry != NULL && status == OD_OK;
         entry = strtok_r(NULL, ",", &save)) {
        status = parse_port_entry(entry, id, name, project, service, snapshot, error);
    }
    return status;
}

OdStatus od_docker_parse_ps_json_lines(const char *text,
                                       size_t length,
                                       OdScanSnapshot *snapshot,
                                       OdError *error) {
    if (text == NULL || snapshot == NULL || length > 4U * 1024U * 1024U) {
        od_error_set(error, OD_ERROR_INVALID, "Docker output is missing or too large");
        return OD_ERROR_INVALID;
    }
    char *buffer = malloc(length + 1U);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse Docker output");
        return OD_ERROR_MEMORY;
    }
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    OdStatus status = OD_OK;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\n", &save); line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &save)) {
        line = trim(line);
        if (line[0] != '\0') status = parse_json_line(line, snapshot, error);
    }
    free(buffer);
    if (status == OD_OK) status = deduplicate_mappings(snapshot, error);
    if (status == OD_OK) {
        snapshot->docker_available = true;
        od_error_clear(error);
    }
    return status;
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0U;
    return (uint64_t)time.tv_sec * UINT64_C(1000) + (uint64_t)time.tv_nsec / UINT64_C(1000000);
}

static OdStatus docker_warning(OdScanSnapshot *snapshot, const char *message, OdError *error) {
    snapshot->docker_available = false;
    OdStatus status = od_scan_snapshot_add_warning(snapshot, message, error);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

OdStatus od_docker_scan(const char *binary,
                        unsigned timeout_milliseconds,
                        size_t output_limit,
                        OdScanSnapshot *snapshot,
                        OdError *error) {
    if (binary == NULL || snapshot == NULL || timeout_milliseconds == 0U || output_limit == 0U) {
        od_error_set(error, OD_ERROR_INVALID, "Docker scan options are invalid");
        return OD_ERROR_INVALID;
    }
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        return docker_warning(snapshot, "Docker scan could not create an output pipe", error);
    }
    (void)fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return docker_warning(snapshot, "Docker scan could not start the CLI", error);
    }
    if (child == 0) {
        (void)dup2(descriptors[1], STDOUT_FILENO);
        (void)dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[0]);
        close(descriptors[1]);
        char *const arguments[] = {(char *)binary, "ps", "--format", "{{json .}}", NULL};
        if (strchr(binary, '/') != NULL) {
            execv(binary, arguments);
        } else {
            execvp(binary, arguments);
        }
        _exit(127);
    }
    close(descriptors[1]);
    int flags = fcntl(descriptors[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);
    char *output = malloc(output_limit + 1U);
    if (output == NULL) {
        close(descriptors[0]);
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        od_error_set(error, OD_ERROR_MEMORY, "unable to capture Docker output");
        return OD_ERROR_MEMORY;
    }
    size_t length = 0U;
    bool overflow = false;
    bool timed_out = false;
    bool eof = false;
    uint64_t deadline = monotonic_milliseconds() + timeout_milliseconds;
    while (!eof) {
        uint64_t now = monotonic_milliseconds();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        int remaining = (int)(deadline - now);
        struct pollfd poll_descriptor = {descriptors[0], POLLIN | POLLHUP, 0};
        int poll_result = poll(&poll_descriptor, 1U, remaining);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_result == 0) {
            timed_out = true;
            break;
        }
        if ((poll_descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            while (true) {
                char chunk[4096];
                ssize_t count = read(descriptors[0], chunk, sizeof(chunk));
                if (count > 0) {
                    size_t bytes = (size_t)count;
                    if (bytes > output_limit - length) {
                        overflow = true;
                        break;
                    }
                    memcpy(output + length, chunk, bytes);
                    length += bytes;
                    continue;
                }
                if (count == 0) eof = true;
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) eof = true;
                break;
            }
        }
        if (overflow) break;
    }
    close(descriptors[0]);
    if (timed_out || overflow) {
        (void)kill(child, SIGTERM);
        (void)kill(child, SIGKILL);
    }
    int child_status = 0;
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
    output[length] = '\0';
    if (timed_out) {
        free(output);
        return docker_warning(snapshot, "Docker scan timed out; host sockets are still available", error);
    }
    if (overflow) {
        free(output);
        return docker_warning(snapshot, "Docker output exceeded the configured limit", error);
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        free(output);
        return docker_warning(snapshot, "Docker is absent, stopped, or inaccessible", error);
    }
    OdStatus status = od_docker_parse_ps_json_lines(output, length, snapshot, error);
    free(output);
    if (status == OD_ERROR_INVALID) {
        return docker_warning(snapshot, "Docker returned malformed output", error);
    }
    return status;
}
