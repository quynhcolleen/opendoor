#include "opendoor/discovery.h"
#include "opendoor/dotenv.h"

#define JSMN_HEADER
#include "jsmn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OD_DISCOVERY_INPUT_LIMIT (4U * 1024U * 1024U)
#define OD_DISCOVERY_SOURCE_CAP 4096U

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static uint64_t fnv1a_string(const char *value, uint64_t hash) {
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        hash ^= (unsigned char)value[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void candidate_id(OdCandidate *candidate) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = fnv1a_string(candidate->variable, hash);
    hash ^= candidate->port;
    hash *= UINT64_C(1099511628211);
    (void)snprintf(candidate->stable_id, sizeof(candidate->stable_id),
                   "candidate-%016llx", (unsigned long long)hash);
}

static char *display_name(const char *variable) {
    size_t length = strlen(variable);
    if (length > 5U && strcmp(variable + length - 5U, "_PORT") == 0) {
        length -= 5U;
    }
    char *name = malloc(length + 1U);
    if (name == NULL) return NULL;
    bool new_word = true;
    for (size_t index = 0U; index < length; ++index) {
        char character = variable[index];
        if (character == '_') {
            name[index] = ' ';
            new_word = true;
        } else if (new_word) {
            name[index] = (char)toupper((unsigned char)character);
            new_word = false;
        } else {
            name[index] = (char)tolower((unsigned char)character);
        }
    }
    name[length] = '\0';
    return name;
}

static bool valid_variable(const char *variable) {
    if (!(variable[0] == '_' || (variable[0] >= 'A' && variable[0] <= 'Z'))) return false;
    for (size_t index = 1U; variable[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)variable[index];
        if (!(character == '_' || isdigit(character) ||
              (character >= 'A' && character <= 'Z'))) return false;
    }
    return true;
}

static bool ends_with_port(const char *variable) {
    size_t length = strlen(variable);
    return length >= 5U && strcmp(variable + length - 5U, "_PORT") == 0;
}

static bool parse_port(const char *text, uint16_t *port) {
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > 65535UL) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static OdStatus source_add(OdStringList *sources, const char *source, OdError *error) {
    for (size_t index = 0U; index < sources->count; ++index) {
        if (strcmp(sources->items[index], source) == 0) return OD_OK;
    }
    char **items = realloc(sources->items, (sources->count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store discovery source");
        return OD_ERROR_MEMORY;
    }
    sources->items = items;
    items[sources->count] = copy_string(source);
    if (items[sources->count] == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy discovery source");
        return OD_ERROR_MEMORY;
    }
    ++sources->count;
    return OD_OK;
}

static void candidate_clear(OdCandidate *candidate) {
    free(candidate->name);
    free(candidate->variable);
    free(candidate->group);
    for (size_t index = 0U; index < candidate->sources.count; ++index) {
        free(candidate->sources.items[index]);
    }
    free(candidate->sources.items);
    *candidate = (OdCandidate){0};
}

void od_candidate_list_init(OdCandidateList *list) {
    if (list != NULL) *list = (OdCandidateList){0};
}

void od_candidate_list_free(OdCandidateList *list) {
    if (list == NULL) return;
    for (size_t index = 0U; index < list->count; ++index) {
        candidate_clear(&list->items[index]);
    }
    free(list->items);
    *list = (OdCandidateList){0};
}

static OdStatus candidate_append(OdCandidateList *list,
                                 const char *name,
                                 const char *variable,
                                 const char *group,
                                 uint16_t port,
                                 unsigned protocols,
                                 OdConfidence confidence,
                                 bool selected,
                                 const char *source,
                                 OdError *error) {
    OdCandidate *items = realloc(list->items, (list->count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store discovery candidate");
        return OD_ERROR_MEMORY;
    }
    list->items = items;
    OdCandidate *candidate = &items[list->count];
    *candidate = (OdCandidate){0};
    candidate->name = copy_string(name);
    candidate->variable = copy_string(variable);
    candidate->group = copy_string(group);
    candidate->port = port;
    candidate->protocols = protocols;
    candidate->confidence = confidence;
    candidate->selected = selected;
    if (candidate->name == NULL || candidate->variable == NULL || candidate->group == NULL) {
        candidate_clear(candidate);
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy discovery candidate");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = source_add(&candidate->sources, source, error);
    if (status != OD_OK) {
        candidate_clear(candidate);
        return status;
    }
    candidate_id(candidate);
    ++list->count;
    return OD_OK;
}

static bool candidate_exists(const OdCandidateList *list, const char *variable, uint16_t port) {
    for (size_t index = 0U; index < list->count; ++index) {
        if (list->items[index].port == port && strcmp(list->items[index].variable, variable) == 0) {
            return true;
        }
    }
    return false;
}

static char *trim(char *value);

static bool candidate_variable_exists(const OdCandidateList *list, const char *variable) {
    for (size_t index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index].variable, variable) == 0) return true;
    }
    return false;
}

static void compose_variable(const char *service,
                             uint16_t target,
                             const OdCandidateList *list,
                             char *variable,
                             size_t capacity) {
    size_t output = 0U;
    for (size_t index = 0U; service[index] != '\0' && output + 6U < capacity; ++index) {
        unsigned char character = (unsigned char)service[index];
        variable[output++] = isalnum(character) ? (char)toupper(character) : '_';
    }
    (void)snprintf(variable + output, capacity - output, "_PORT");
    if (!candidate_variable_exists(list, variable)) return;
    (void)snprintf(variable + output, capacity - output, "_%u_PORT", (unsigned)target);
}

static OdStatus append_compose_literal(const char *path,
                                       const char *service,
                                       uint16_t published,
                                       uint16_t target,
                                       unsigned protocol,
                                       OdCandidateList *list,
                                       OdError *error) {
    char variable[256];
    compose_variable(service, target, list, variable, sizeof(variable));
    if (candidate_exists(list, variable, published)) return OD_OK;
    char source[OD_DISCOVERY_SOURCE_CAP];
    int count = snprintf(source, sizeof(source), "compose:%s#%s", path, service);
    if (count < 0 || (size_t)count >= sizeof(source)) {
        od_error_set(error, OD_ERROR_INVALID, "Compose source is too long");
        return OD_ERROR_INVALID;
    }
    return candidate_append(list, service, variable, "compose", published, protocol,
                            OD_CONFIDENCE_CONFIRMED, true, source, error);
}

static OdStatus discover_compose_yaml_literals(const char *path,
                                               const char *text,
                                               OdCandidateList *list,
                                               OdError *error) {
    char *buffer = strdup(text);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to inspect Compose literals");
        return OD_ERROR_MEMORY;
    }
    char service[128] = {0};
    bool services = false;
    OdStatus status = OD_OK;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\n", &save); line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &save)) {
        size_t indentation = 0U;
        while (line[indentation] == ' ') ++indentation;
        char *content = trim(line + indentation);
        if (indentation == 0U) {
            services = strcmp(content, "services:") == 0;
            if (!services) service[0] = '\0';
            continue;
        }
        size_t content_length = strlen(content);
        if (services && indentation == 2U && content_length > 1U &&
            content[content_length - 1U] == ':') {
            size_t length = content_length - 1U;
            if (length >= sizeof(service)) length = sizeof(service) - 1U;
            memcpy(service, content, length);
            service[length] = '\0';
            continue;
        }
        if (!services || service[0] == '\0') continue;
        char *value = NULL;
        if (strncmp(content, "- ", 2U) == 0) value = trim(content + 2);
        if (strncmp(content, "published:", 10U) == 0) value = trim(content + 10);
        if (value == NULL) continue;
        if (*value == '\'' || *value == '"') ++value;
        uint16_t published = 0U;
        uint16_t target = 0U;
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(value, &end, 10);
        if (errno != 0 || end == value || parsed == 0UL || parsed > 65535UL) continue;
        published = (uint16_t)parsed;
        if (*end == ':') {
            char *target_end = NULL;
            unsigned long parsed_target = strtoul(end + 1, &target_end, 10);
            if (target_end == end + 1 || parsed_target == 0UL || parsed_target > 65535UL) continue;
            target = (uint16_t)parsed_target;
        } else {
            target = published;
        }
        unsigned protocol = strstr(end, "/udp") == NULL ? OD_PROTOCOL_TCP : OD_PROTOCOL_UDP;
        status = append_compose_literal(path, service, published, target,
                                        protocol, list, error);
    }
    free(buffer);
    return status;
}

OdStatus od_discover_compose_text(const char *path,
                                  const char *text,
                                  OdCandidateList *list,
                                  OdError *error) {
    if (path == NULL || text == NULL || list == NULL || strlen(text) > OD_DISCOVERY_INPUT_LIMIT) {
        od_error_set(error, OD_ERROR_INVALID, "Compose discovery input is invalid");
        return OD_ERROR_INVALID;
    }
    const char *cursor = text;
    OdStatus status = OD_OK;
    while ((cursor = strstr(cursor, "${")) != NULL && status == OD_OK) {
        const char *variable_start = cursor + 2;
        const char *fallback = strstr(variable_start, ":-");
        const char *closing = fallback == NULL ? NULL : strchr(fallback + 2, '}');
        if (fallback == NULL || closing == NULL || fallback == variable_start) {
            cursor += 2;
            continue;
        }
        size_t variable_length = (size_t)(fallback - variable_start);
        size_t port_length = (size_t)(closing - (fallback + 2));
        if (variable_length >= 128U || port_length >= 8U) {
            cursor = closing + 1;
            continue;
        }
        char variable[128];
        char port_text[8];
        memcpy(variable, variable_start, variable_length);
        variable[variable_length] = '\0';
        memcpy(port_text, fallback + 2, port_length);
        port_text[port_length] = '\0';
        uint16_t port = 0U;
        if (valid_variable(variable) && ends_with_port(variable) && parse_port(port_text, &port) &&
            !candidate_exists(list, variable, port)) {
            char *name = display_name(variable);
            char source[OD_DISCOVERY_SOURCE_CAP];
            int count = snprintf(source, sizeof(source), "compose:%s#%s", path, variable);
            if (name == NULL || count < 0 || (size_t)count >= sizeof(source)) {
                free(name);
                od_error_set(error, OD_ERROR_MEMORY, "unable to create Compose candidate");
                status = OD_ERROR_MEMORY;
            } else {
                status = candidate_append(list, name, variable, "compose", port,
                                          OD_PROTOCOL_TCP, OD_CONFIDENCE_CONFIRMED,
                                          true, source, error);
                free(name);
            }
        }
        cursor = closing + 1;
    }
    if (status == OD_OK) status = discover_compose_yaml_literals(path, text, list, error);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static bool json_token_equals(const char *json, const jsmntok_t *token, const char *value) {
    size_t length = strlen(value);
    return token->type == JSMN_STRING && token->start >= 0 && token->end >= token->start &&
           (size_t)(token->end - token->start) == length &&
           strncmp(json + token->start, value, length) == 0;
}

static int json_token_after(const jsmntok_t *tokens, int count, int index) {
    int end = tokens[index].end;
    ++index;
    while (index < count && tokens[index].start < end) ++index;
    return index;
}

static int json_object_value(const char *json,
                             const jsmntok_t *tokens,
                             int count,
                             int object,
                             const char *key) {
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT) return -1;
    int index = object + 1;
    while (index + 1 < count && tokens[index].start < tokens[object].end) {
        int value = index + 1;
        if (json_token_equals(json, &tokens[index], key)) return value;
        index = json_token_after(tokens, count, value);
    }
    return -1;
}

static bool json_value_copy(const char *json,
                            const jsmntok_t *token,
                            char *output,
                            size_t capacity) {
    if ((token->type != JSMN_STRING && token->type != JSMN_PRIMITIVE) ||
        token->start < 0 || token->end < token->start || capacity == 0U) return false;
    size_t length = (size_t)(token->end - token->start);
    if (length >= capacity) return false;
    memcpy(output, json + token->start, length);
    output[length] = '\0';
    return true;
}

OdStatus od_discover_compose_config_json(const char *path,
                                         const char *text,
                                         OdCandidateList *list,
                                         OdError *error) {
    size_t length = text == NULL ? 0U : strlen(text);
    if (path == NULL || text == NULL || list == NULL || length > OD_DISCOVERY_INPUT_LIMIT) {
        od_error_set(error, OD_ERROR_INVALID, "Compose config JSON is invalid");
        return OD_ERROR_INVALID;
    }
    size_t capacity = length / 2U + 32U;
    jsmntok_t *tokens = calloc(capacity, sizeof(*tokens));
    if (tokens == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to tokenize Compose config");
        return OD_ERROR_MEMORY;
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, text, length, tokens, (unsigned int)capacity);
    if (count < 1 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        od_error_set(error, OD_ERROR_INVALID, "Compose returned malformed JSON");
        return OD_ERROR_INVALID;
    }
    int services = json_object_value(text, tokens, count, 0, "services");
    if (services < 0 || tokens[services].type != JSMN_OBJECT) {
        free(tokens);
        od_error_set(error, OD_ERROR_INVALID, "Compose config has no services object");
        return OD_ERROR_INVALID;
    }
    OdStatus status = OD_OK;
    int service_key = services + 1;
    while (service_key + 1 < count && tokens[service_key].start < tokens[services].end &&
           status == OD_OK) {
        int service_object = service_key + 1;
        char service[128];
        if (!json_value_copy(text, &tokens[service_key], service, sizeof(service)) ||
            tokens[service_object].type != JSMN_OBJECT) {
            service_key = json_token_after(tokens, count, service_object);
            continue;
        }
        int ports = json_object_value(text, tokens, count, service_object, "ports");
        if (ports >= 0 && tokens[ports].type == JSMN_ARRAY) {
            int item = ports + 1;
            while (item < count && tokens[item].start < tokens[ports].end && status == OD_OK) {
                if (tokens[item].type == JSMN_OBJECT) {
                    int published_token = json_object_value(text, tokens, count, item, "published");
                    int target_token = json_object_value(text, tokens, count, item, "target");
                    int protocol_token = json_object_value(text, tokens, count, item, "protocol");
                    char published_text[128];
                    char target_text[32];
                    char protocol_text[16] = "tcp";
                    uint16_t published;
                    uint16_t target;
                    if (published_token >= 0 && target_token >= 0 &&
                        json_value_copy(text, &tokens[published_token], published_text,
                                        sizeof(published_text)) &&
                        json_value_copy(text, &tokens[target_token], target_text,
                                        sizeof(target_text)) &&
                        parse_port(published_text, &published) && parse_port(target_text, &target)) {
                        if (protocol_token >= 0) {
                            (void)json_value_copy(text, &tokens[protocol_token], protocol_text,
                                                  sizeof(protocol_text));
                        }
                        unsigned protocol = strcmp(protocol_text, "udp") == 0 ?
                                            OD_PROTOCOL_UDP : OD_PROTOCOL_TCP;
                        status = append_compose_literal(path, service, published, target,
                                                        protocol, list, error);
                    }
                }
                item = json_token_after(tokens, count, item);
            }
        }
        service_key = json_token_after(tokens, count, service_object);
    }
    free(tokens);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static uint64_t discovery_milliseconds(void) {
    struct timespec current;
    if (clock_gettime(CLOCK_MONOTONIC, &current) != 0) return 0U;
    return (uint64_t)current.tv_sec * UINT64_C(1000) +
           (uint64_t)current.tv_nsec / UINT64_C(1000000);
}

OdStatus od_discover_compose_cli(const char *binary,
                                 const char *project_root,
                                 unsigned timeout_milliseconds,
                                 OdCandidateList *list,
                                 OdError *error) {
    if (binary == NULL || project_root == NULL || list == NULL || timeout_milliseconds == 0U) {
        od_error_set(error, OD_ERROR_INVALID, "Compose CLI options are invalid");
        return OD_ERROR_INVALID;
    }
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to capture Compose config");
        return OD_ERROR_IO;
    }
    pid_t child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        od_error_set(error, OD_ERROR_IO, "unable to start Docker Compose");
        return OD_ERROR_IO;
    }
    if (child == 0) {
        (void)dup2(descriptors[1], STDOUT_FILENO);
        int null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_descriptor >= 0) (void)dup2(null_descriptor, STDERR_FILENO);
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        if (null_descriptor >= 0) (void)close(null_descriptor);
        if (chdir(project_root) != 0) _exit(126);
        char *const arguments[] = {(char *)binary, "compose", "config", "--format", "json",
                                   "--no-interpolate", NULL};
        if (strchr(binary, '/') != NULL) execv(binary, arguments);
        else execvp(binary, arguments);
        _exit(127);
    }
    (void)close(descriptors[1]);
    int flags = fcntl(descriptors[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);
    char *output = malloc(OD_DISCOVERY_INPUT_LIMIT + 1U);
    if (output == NULL) {
        (void)close(descriptors[0]);
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        od_error_set(error, OD_ERROR_MEMORY, "unable to capture Compose config");
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    bool timed_out = false;
    bool overflow = false;
    bool eof = false;
    uint64_t deadline = discovery_milliseconds() + timeout_milliseconds;
    while (!eof && !timed_out && !overflow) {
        uint64_t now = discovery_milliseconds();
        if (now >= deadline) { timed_out = true; break; }
        struct pollfd descriptor = {descriptors[0], POLLIN | POLLHUP, 0};
        int result = poll(&descriptor, 1U, (int)(deadline - now));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { timed_out = result == 0; break; }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) continue;
        while (true) {
            char chunk[4096];
            ssize_t bytes = read(descriptors[0], chunk, sizeof(chunk));
            if (bytes > 0) {
                if ((size_t)bytes > OD_DISCOVERY_INPUT_LIMIT - used) { overflow = true; break; }
                memcpy(output + used, chunk, (size_t)bytes);
                used += (size_t)bytes;
            } else {
                if (bytes == 0) eof = true;
                if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) eof = true;
                break;
            }
        }
    }
    (void)close(descriptors[0]);
    int child_status = 0;
    bool reaped = false;
    while (!reaped) {
        pid_t waited = waitpid(child, &child_status, WNOHANG);
        if (waited == child || (waited < 0 && errno != EINTR)) { reaped = true; break; }
        if (!timed_out && discovery_milliseconds() >= deadline) timed_out = true;
        if (timed_out || overflow) (void)kill(child, SIGKILL);
        struct timespec pause = {0, 5000000L};
        (void)nanosleep(&pause, NULL);
    }
    output[used] = '\0';
    if (timed_out || overflow || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        free(output);
        od_error_set(error, OD_ERROR_IO, timed_out ? "Docker Compose config timed out" :
                     (overflow ? "Docker Compose config was too large" :
                                 "Docker Compose config is unavailable"));
        return OD_ERROR_IO;
    }
    OdStatus status = od_discover_compose_config_json("docker-compose-config", output,
                                                       list, error);
    free(output);
    return status;
}

static char *trim(char *value) {
    while (isspace((unsigned char)*value)) ++value;
    size_t length = strlen(value);
    while (length > 0U && isspace((unsigned char)value[length - 1U])) {
        value[--length] = '\0';
    }
    return value;
}

OdStatus od_discover_dotenv_text(const char *path,
                                 const char *text,
                                 OdCandidateList *list,
                                 OdError *error) {
    if (path == NULL || text == NULL || list == NULL || strlen(text) > OD_DISCOVERY_INPUT_LIMIT) {
        od_error_set(error, OD_ERROR_INVALID, "dotenv discovery input is invalid");
        return OD_ERROR_INVALID;
    }
    char *buffer = strdup(text);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse dotenv discovery input");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = OD_OK;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\n", &save); line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &save)) {
        char *content = trim(line);
        if (content[0] == '\0' || content[0] == '#') continue;
        char *equals = strchr(content, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        char *variable = trim(content);
        char *value = trim(equals + 1);
        uint16_t port = 0U;
        if (!valid_variable(variable) || !ends_with_port(variable) || !parse_port(value, &port) ||
            candidate_exists(list, variable, port)) continue;
        char *name = display_name(variable);
        char source[OD_DISCOVERY_SOURCE_CAP];
        int count = snprintf(source, sizeof(source), "dotenv:%s#%s", path, variable);
        if (name == NULL || count < 0 || (size_t)count >= sizeof(source)) {
            free(name);
            od_error_set(error, OD_ERROR_MEMORY, "unable to create dotenv candidate");
            status = OD_ERROR_MEMORY;
        } else {
            status = candidate_append(list, name, variable, "environment", port,
                                      OD_PROTOCOL_TCP, OD_CONFIDENCE_CONFIRMED,
                                      true, source, error);
            free(name);
        }
    }
    free(buffer);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static bool token_copy(const char *json, const jsmntok_t *token,
                       char *output, size_t output_size) {
    if (token->type != JSMN_STRING || token->start < 0 || token->end < token->start) return false;
    size_t length = (size_t)(token->end - token->start);
    if (length >= output_size) length = output_size - 1U;
    memcpy(output, json + token->start, length);
    output[length] = '\0';
    return true;
}

static bool command_port(const char *command, uint16_t *port) {
    const char *flag = strstr(command, "--port");
    if (flag == NULL) return false;
    flag += 6;
    if (*flag == '=') ++flag;
    while (isspace((unsigned char)*flag)) ++flag;
    char digits[8];
    size_t count = 0U;
    while (isdigit((unsigned char)flag[count]) && count + 1U < sizeof(digits)) {
        digits[count] = flag[count];
        ++count;
    }
    digits[count] = '\0';
    return count > 0U && parse_port(digits, port);
}

static void make_package_variable(const char *package, const char *script,
                                  char *variable, size_t variable_size) {
    size_t output = 0U;
    const char *parts[2] = {package, script};
    for (size_t part = 0U; part < 2U; ++part) {
        if (part != 0U && output + 1U < variable_size) variable[output++] = '_';
        for (size_t index = 0U; parts[part][index] != '\0' && output + 1U < variable_size; ++index) {
            unsigned char character = (unsigned char)parts[part][index];
            variable[output++] = isalnum(character) ? (char)toupper(character) : '_';
        }
    }
    const char suffix[] = "_PORT";
    for (size_t index = 0U; index + 1U < sizeof(suffix) && output + 1U < variable_size; ++index) {
        variable[output++] = suffix[index];
    }
    variable[output] = '\0';
}

OdStatus od_discover_package_json(const char *path,
                                  const char *text,
                                  OdCandidateList *list,
                                  OdError *error) {
    size_t length = text == NULL ? 0U : strlen(text);
    if (path == NULL || text == NULL || list == NULL || length > OD_DISCOVERY_INPUT_LIMIT) {
        od_error_set(error, OD_ERROR_INVALID, "package discovery input is invalid");
        return OD_ERROR_INVALID;
    }
    size_t token_capacity = length / 2U + 32U;
    jsmntok_t *tokens = calloc(token_capacity, sizeof(*tokens));
    if (tokens == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to tokenize package metadata");
        return OD_ERROR_MEMORY;
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    int token_count = jsmn_parse(&parser, text, length, tokens, (unsigned int)token_capacity);
    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        od_error_set(error, OD_ERROR_INVALID, "package metadata is malformed JSON");
        return OD_ERROR_INVALID;
    }
    char package[128] = "package";
    for (int index = 1; index + 1 < token_count; ++index) {
        size_t key_length = (size_t)(tokens[index].end - tokens[index].start);
        if (tokens[index].type == JSMN_STRING && key_length == 4U &&
            strncmp(text + tokens[index].start, "name", 4U) == 0) {
            (void)token_copy(text, &tokens[index + 1], package, sizeof(package));
            break;
        }
    }
    OdStatus status = OD_OK;
    for (int index = 2; index < token_count && status == OD_OK; ++index) {
        char command[1024];
        if (!token_copy(text, &tokens[index], command, sizeof(command))) continue;
        uint16_t port = 0U;
        if (!command_port(command, &port) || tokens[index - 1].type != JSMN_STRING) continue;
        char script[128];
        if (!token_copy(text, &tokens[index - 1], script, sizeof(script))) continue;
        char variable[256];
        make_package_variable(package, script, variable, sizeof(variable));
        char name[256];
        char source[OD_DISCOVERY_SOURCE_CAP];
        int name_count = snprintf(name, sizeof(name), "%s %s", package, script);
        int source_count = snprintf(source, sizeof(source), "package:%s#%s", path, script);
        if (name_count < 0 || (size_t)name_count >= sizeof(name) ||
            source_count < 0 || (size_t)source_count >= sizeof(source)) {
            od_error_set(error, OD_ERROR_INVALID, "package candidate is too long");
            status = OD_ERROR_INVALID;
            break;
        }
        status = candidate_append(list, name, variable, "package", port,
                                  OD_PROTOCOL_TCP, OD_CONFIDENCE_LIKELY,
                                  false, source, error);
    }
    free(tokens);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

OdStatus od_discover_makefile_text(const char *path,
                                   const char *text,
                                   OdCandidateList *list,
                                   OdError *error) {
    if (path == NULL || text == NULL || list == NULL || strlen(text) > OD_DISCOVERY_INPUT_LIMIT) {
        od_error_set(error, OD_ERROR_INVALID, "Makefile discovery input is invalid");
        return OD_ERROR_INVALID;
    }
    char *buffer = strdup(text);
    if (buffer == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to parse Makefile");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = OD_OK;
    char *save = NULL;
    for (char *line = strtok_r(buffer, "\n", &save); line != NULL && status == OD_OK;
         line = strtok_r(NULL, "\n", &save)) {
        char *question = strstr(line, "?=");
        char *equals = question == NULL ? strchr(line, '=') : question;
        if (equals == NULL) continue;
        *equals = '\0';
        char *variable = trim(line);
        char *value = trim(equals + (question == NULL ? 1 : 2));
        uint16_t port = 0U;
        if (!valid_variable(variable) || !ends_with_port(variable) || !parse_port(value, &port) ||
            candidate_exists(list, variable, port)) continue;
        char *name = display_name(variable);
        char source[OD_DISCOVERY_SOURCE_CAP];
        int count = snprintf(source, sizeof(source), "makefile:%s#%s", path, variable);
        if (name == NULL || count < 0 || (size_t)count >= sizeof(source)) {
            free(name);
            od_error_set(error, OD_ERROR_MEMORY, "unable to create Makefile candidate");
            status = OD_ERROR_MEMORY;
        } else {
            status = candidate_append(list, name, variable, "makefile", port,
                                      OD_PROTOCOL_TCP, OD_CONFIDENCE_POSSIBLE,
                                      false, source, error);
            free(name);
        }
    }
    free(buffer);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static OdStatus candidate_copy_append(OdCandidateList *destination,
                                      const OdCandidate *source,
                                      OdError *error) {
    OdStatus status = candidate_append(destination, source->name, source->variable,
                                       source->group, source->port, source->protocols,
                                       source->confidence, source->selected,
                                       source->sources.items[0], error);
    if (status != OD_OK) return status;
    OdCandidate *copy = &destination->items[destination->count - 1U];
    for (size_t index = 1U; index < source->sources.count; ++index) {
        status = source_add(&copy->sources, source->sources.items[index], error);
        if (status != OD_OK) return status;
    }
    return OD_OK;
}

OdStatus od_candidates_merge(OdCandidateList *destination,
                             const OdCandidateList *source,
                             OdError *error) {
    if (destination == NULL || source == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "candidate lists are required");
        return OD_ERROR_INVALID;
    }
    OdStatus status = OD_OK;
    for (size_t source_index = 0U; source_index < source->count && status == OD_OK; ++source_index) {
        const OdCandidate *incoming = &source->items[source_index];
        OdCandidate *match = NULL;
        for (size_t destination_index = 0U; destination_index < destination->count; ++destination_index) {
            OdCandidate *candidate = &destination->items[destination_index];
            if (candidate->port == incoming->port &&
                strcmp(candidate->variable, incoming->variable) == 0) {
                match = candidate;
                break;
            }
        }
        if (match == NULL) {
            status = candidate_copy_append(destination, incoming, error);
            continue;
        }
        if (incoming->confidence > match->confidence) match->confidence = incoming->confidence;
        match->selected = match->selected || incoming->selected;
        match->protocols |= incoming->protocols;
        for (size_t source_item = 0U; source_item < incoming->sources.count && status == OD_OK;
             ++source_item) {
            status = source_add(&match->sources, incoming->sources.items[source_item], error);
        }
    }
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static bool suffix(const char *value, const char *ending) {
    size_t value_length = strlen(value);
    size_t ending_length = strlen(ending);
    return value_length >= ending_length &&
           strcmp(value + value_length - ending_length, ending) == 0;
}

static bool compose_name(const char *name) {
    bool extension = suffix(name, ".yaml") || suffix(name, ".yml");
    return extension &&
           (strncmp(name, "compose", 7U) == 0 || strncmp(name, "docker-compose", 14U) == 0);
}

static bool skipped_directory(const char *name) {
    static const char *const skipped[] = {
        ".git", ".opendoor", ".worktrees", "node_modules", "build", "dist"
    };
    for (size_t index = 0U; index < sizeof(skipped) / sizeof(skipped[0]); ++index) {
        if (strcmp(name, skipped[index]) == 0) return true;
    }
    return false;
}

static OdStatus read_discovery_file(const char *path, char **text, OdError *error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return errno == ELOOP ? OD_ERROR_INVALID : OD_ERROR_IO;
    struct stat metadata;
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 || (uintmax_t)metadata.st_size > OD_DISCOVERY_INPUT_LIMIT) {
        (void)close(descriptor);
        return OD_ERROR_INVALID;
    }
    size_t length = (size_t)metadata.st_size;
    char *buffer = malloc(length + 1U);
    if (buffer == NULL) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read %s", path);
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    while (used < length) {
        ssize_t count = read(descriptor, buffer + used, length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(buffer);
            (void)close(descriptor);
            return OD_ERROR_IO;
        }
        used += (size_t)count;
    }
    if (close(descriptor) != 0) {
        free(buffer);
        return OD_ERROR_IO;
    }
    buffer[length] = '\0';
    *text = buffer;
    return OD_OK;
}

static OdStatus discover_file(const char *full_path,
                              const char *relative_path,
                              const char *name,
                              OdCandidateList *candidates,
                              OdError *error) {
    bool is_compose = compose_name(name);
    bool is_dotenv = strncmp(name, ".env", 4U) == 0 || strcmp(name, ".ports.env") == 0;
    bool is_package = strcmp(name, "package.json") == 0;
    bool is_makefile = strcmp(name, "Makefile") == 0 || strcmp(name, "makefile") == 0;
    if (!is_compose && !is_dotenv && !is_package && !is_makefile) return OD_OK;
    char *text = NULL;
    OdStatus status = read_discovery_file(full_path, &text, error);
    if (status != OD_OK) {
        return status == OD_ERROR_MEMORY ? status : OD_OK;
    }
    OdCandidateList found;
    od_candidate_list_init(&found);
    if (strcmp(name, ".ports.env") == 0) {
        OdAssignments assignments;
        OdError marker_error;
        OdStatus marker_status = od_assignments_parse(
            text, strlen(text), &assignments, &marker_error);
        if (marker_status != OD_OK) {
            free(text);
            od_candidate_list_free(&found);
            od_error_clear(error);
            return OD_OK;
        }
        od_assignments_free(&assignments);
    }
    if (is_compose) {
        status = od_discover_compose_text(relative_path, text, &found, error);
    } else if (is_dotenv) {
        status = od_discover_dotenv_text(relative_path, text, &found, error);
    } else if (is_package) {
        status = od_discover_package_json(relative_path, text, &found, error);
    } else {
        status = od_discover_makefile_text(relative_path, text, &found, error);
    }
    free(text);
    if (status == OD_OK) status = od_candidates_merge(candidates, &found, error);
    od_candidate_list_free(&found);
    if (status == OD_ERROR_INVALID || status == OD_ERROR_IO) {
        od_error_clear(error);
        return OD_OK;
    }
    return status;
}

static OdStatus discover_directory(const char *root,
                                   const char *relative,
                                   unsigned depth,
                                   OdCandidateList *candidates,
                                   OdError *error) {
    char directory_path[OD_DISCOVERY_SOURCE_CAP];
    int count = relative[0] == '\0' ?
                snprintf(directory_path, sizeof(directory_path), "%s", root) :
                snprintf(directory_path, sizeof(directory_path), "%s/%s", root, relative);
    if (count < 0 || (size_t)count >= sizeof(directory_path)) {
        od_error_set(error, OD_ERROR_INVALID, "project path is too long");
        return OD_ERROR_INVALID;
    }
    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        if (depth == 0U) {
            od_error_set(error, OD_ERROR_IO, "unable to open project root: %s", root);
            return OD_ERROR_IO;
        }
        return OD_OK;
    }
    OdStatus status = OD_OK;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && status == OD_OK) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child_relative[OD_DISCOVERY_SOURCE_CAP];
        count = relative[0] == '\0' ?
                snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name) :
                snprintf(child_relative, sizeof(child_relative), "%s/%s", relative, entry->d_name);
        if (count < 0 || (size_t)count >= sizeof(child_relative)) continue;
        char full_path[OD_DISCOVERY_SOURCE_CAP];
        count = snprintf(full_path, sizeof(full_path), "%s/%s", root, child_relative);
        if (count < 0 || (size_t)count >= sizeof(full_path)) continue;
        struct stat metadata;
        if (lstat(full_path, &metadata) != 0) continue;
        if (S_ISDIR(metadata.st_mode) && depth < 3U && !skipped_directory(entry->d_name)) {
            status = discover_directory(root, child_relative, depth + 1U, candidates, error);
        } else if (S_ISREG(metadata.st_mode)) {
            status = discover_file(full_path, child_relative, entry->d_name, candidates, error);
        }
    }
    closedir(directory);
    return status;
}

static int compare_candidates(const void *left_value, const void *right_value) {
    const OdCandidate *left = left_value;
    const OdCandidate *right = right_value;
    if (left->confidence != right->confidence) {
        return left->confidence > right->confidence ? -1 : 1;
    }
    int variable = strcmp(left->variable, right->variable);
    if (variable != 0) return variable;
    if (left->port == right->port) return 0;
    return left->port < right->port ? -1 : 1;
}

OdStatus od_discover_project(const char *project_root,
                             OdCandidateList *candidates,
                             OdError *error) {
    if (project_root == NULL || candidates == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "project root and candidate list are required");
        return OD_ERROR_INVALID;
    }
    OdCandidateList normalized;
    od_candidate_list_init(&normalized);
    OdStatus compose_status = od_discover_compose_cli("docker", project_root, 1500U,
                                                      &normalized, error);
    OdStatus status = OD_OK;
    if (compose_status == OD_OK) status = od_candidates_merge(candidates, &normalized, error);
    od_candidate_list_free(&normalized);
    if (status == OD_OK) status = discover_directory(project_root, "", 0U, candidates, error);
    if (status == OD_OK && candidates->count > 1U) {
        qsort(candidates->items, candidates->count, sizeof(*candidates->items), compare_candidates);
    }
    if (status == OD_OK) od_error_clear(error);
    return status;
}
