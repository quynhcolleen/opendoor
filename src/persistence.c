#include "opendoor/persistence.h"

#include "opendoor/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define OD_MANAGED_FILE_MAX (4U * 1024U * 1024U)

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static bool safe_relative_path(const char *path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/') return false;
    const char *component = path;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        if (length == 0U || (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) return false;
        if (end == NULL) break;
        component = end + 1;
    }
    return true;
}

static OdStatus join_path(const char *root,
                          const char *relative,
                          char **path,
                          OdError *error) {
    if (root == NULL || !safe_relative_path(relative)) {
        od_error_set(error, OD_ERROR_INVALID, "managed file path must be a safe relative path");
        return OD_ERROR_INVALID;
    }
    size_t root_length = strlen(root);
    size_t relative_length = strlen(relative);
    bool separator = root_length > 0U && root[root_length - 1U] != '/';
    if (root_length > SIZE_MAX - relative_length - 2U) {
        od_error_set(error, OD_ERROR_INVALID, "managed file path is too long");
        return OD_ERROR_INVALID;
    }
    *path = malloc(root_length + relative_length + (separator ? 2U : 1U));
    if (*path == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to build managed file path");
        return OD_ERROR_MEMORY;
    }
    (void)snprintf(*path, root_length + relative_length + (separator ? 2U : 1U),
                   separator ? "%s/%s" : "%s%s", root, relative);
    return OD_OK;
}

static OdStatus inspect_regular_target(const char *path,
                                       bool *exists,
                                       mode_t *mode,
                                       OdError *error) {
    struct stat information;
    if (lstat(path, &information) == 0) {
        if (!S_ISREG(information.st_mode)) {
            od_error_set(error, OD_ERROR_INVALID,
                         "refusing non-regular or symlink target: %s", path);
            return OD_ERROR_INVALID;
        }
        *exists = true;
        if (mode != NULL) *mode = information.st_mode & (mode_t)0777;
        return OD_OK;
    }
    if (errno != ENOENT) {
        od_error_set(error, OD_ERROR_IO, "unable to inspect %s: %s", path, strerror(errno));
        return OD_ERROR_IO;
    }
    *exists = false;
    if (mode != NULL) *mode = (mode_t)0644;
    return OD_OK;
}

static OdStatus read_regular_file(const char *path,
                                  char **text,
                                  size_t *length,
                                  mode_t *mode,
                                  OdError *error) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to open %s: %s", path, strerror(errno));
        return OD_ERROR_IO;
    }
    struct stat information;
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0 || (uintmax_t)information.st_size > OD_MANAGED_FILE_MAX) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_INVALID, "%s is not a bounded regular file", path);
        return OD_ERROR_INVALID;
    }
    size_t size = (size_t)information.st_size;
    char *buffer = malloc(size + 1U);
    if (buffer == NULL) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_MEMORY, "unable to read %s", path);
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    while (used < size) {
        ssize_t count = read(descriptor, buffer + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(buffer);
            (void)close(descriptor);
            od_error_set(error, OD_ERROR_IO, "unable to read %s", path);
            return OD_ERROR_IO;
        }
        used += (size_t)count;
    }
    if (close(descriptor) != 0) {
        free(buffer);
        od_error_set(error, OD_ERROR_IO, "unable to close %s", path);
        return OD_ERROR_IO;
    }
    buffer[size] = '\0';
    *text = buffer;
    *length = size;
    if (mode != NULL) *mode = information.st_mode & (mode_t)0777;
    return OD_OK;
}

OdStatus od_assignments_load_file(const char *path,
                                  OdAssignments *assignments,
                                  OdError *error) {
    if (path == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "assignment path and output are required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(path, &text, &length, NULL, error);
    if (status == OD_OK) status = od_assignments_parse(text, length, assignments, error);
    free(text);
    return status;
}

OdStatus od_assignments_import_file(const char *path,
                                    OdAssignments *assignments,
                                    OdError *error) {
    if (path == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "assignment path and output are required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(path, &text, &length, NULL, error);
    if (status == OD_OK) status = od_assignments_import(text, length, assignments, error);
    free(text);
    return status;
}

OdStatus od_plan_to_assignments(const OdAllocationPlan *plan,
                                OdAssignments *assignments,
                                OdError *error) {
    if (plan == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "allocation plan and assignments are required");
        return OD_ERROR_INVALID;
    }
    od_assignments_init(assignments);
    assignments->compatible_marker = true;
    assignments->opendoor_marker = true;
    size_t count = 0U;
    for (size_t index = 0U; index < plan->count; ++index) {
        if (plan->items[index].new_port != 0U) ++count;
    }
    if (count == 0U) {
        od_error_clear(error);
        return OD_OK;
    }
    assignments->items = calloc(count, sizeof(*assignments->items));
    if (assignments->items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to build assignments");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < plan->count; ++index) {
        if (plan->items[index].new_port == 0U) continue;
        OdAssignment *assignment = &assignments->items[assignments->count];
        assignment->variable = copy_string(plan->items[index].variable);
        assignment->port = plan->items[index].new_port;
        if (assignment->variable == NULL) {
            od_assignments_free(assignments);
            od_error_set(error, OD_ERROR_MEMORY, "unable to copy assignment variable");
            return OD_ERROR_MEMORY;
        }
        ++assignments->count;
    }
    od_error_clear(error);
    return OD_OK;
}

static const OdService *find_service(const OdProfile *profile, const char *id) {
    for (size_t index = 0U; index < profile->service_count; ++index) {
        if (strcmp(profile->services[index].id, id) == 0) return &profile->services[index];
    }
    return NULL;
}

static bool snapshot_has_port(const OdScanSnapshot *snapshot, uint16_t port) {
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        if (snapshot->endpoints[index].local_port == port) return true;
    }
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        if (snapshot->docker_mappings[index].host_port == port) return true;
    }
    return false;
}

OdStatus od_plan_validate_snapshot(const OdProfile *profile,
                                   const OdAllocationPlan *plan,
                                   const OdScanSnapshot *snapshot,
                                   OdError *error) {
    if (profile == NULL || plan == NULL || snapshot == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "profile, plan, and snapshot are required");
        return OD_ERROR_INVALID;
    }
    for (size_t index = 0U; index < plan->count; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        if (allocation->new_port == 0U) continue;
        if (find_service(profile, allocation->service_id) == NULL) {
            od_error_set(error, OD_ERROR_INVALID, "plan references unknown service: %s",
                         allocation->service_id);
            return OD_ERROR_INVALID;
        }
        if (allocation->new_port < profile->port_min || allocation->new_port > profile->port_max) {
            od_error_set(error, OD_ERROR_INVALID, "port %u is outside the project range",
                         (unsigned)allocation->new_port);
            return OD_ERROR_INVALID;
        }
        if (snapshot_has_port(snapshot, allocation->new_port)) {
            od_error_set(error, OD_ERROR_CHANGED,
                         "port %u became occupied; review the proposal again",
                         (unsigned)allocation->new_port);
            return OD_ERROR_CHANGED;
        }
        for (size_t other = index + 1U; other < plan->count; ++other) {
            if (allocation->new_port == plan->items[other].new_port) {
                od_error_set(error, OD_ERROR_INVALID,
                             "port %u is assigned to multiple services",
                             (unsigned)allocation->new_port);
                return OD_ERROR_INVALID;
            }
        }
    }
    od_error_clear(error);
    return OD_OK;
}

static OdStatus probe_socket(int family,
                             int type,
                             uint16_t port,
                             OdError *error) {
    int descriptor = socket(family, type | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to create bind-probe socket: %s",
                     strerror(errno));
        return OD_ERROR_IO;
    }
    int result;
    if (family == AF_INET) {
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        result = bind(descriptor, (struct sockaddr *)&address, sizeof(address));
    } else {
        int enabled = 1;
        (void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled));
        struct sockaddr_in6 address = {0};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);
        result = bind(descriptor, (struct sockaddr *)&address, sizeof(address));
    }
    int bind_error = errno;
    (void)close(descriptor);
    if (result != 0) {
        od_error_set(error, OD_ERROR_CHANGED,
                     "port %u failed the final %s bind probe: %s",
                     (unsigned)port, type == SOCK_STREAM ? "TCP" : "UDP",
                     strerror(bind_error));
        return OD_ERROR_CHANGED;
    }
    return OD_OK;
}

OdStatus od_plan_probe_bindings(const OdProfile *profile,
                                const OdAllocationPlan *plan,
                                OdError *error) {
    if (profile == NULL || plan == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "profile and allocation plan are required");
        return OD_ERROR_INVALID;
    }
    for (size_t index = 0U; index < plan->count; ++index) {
        uint16_t port = plan->items[index].new_port;
        if (port == 0U) continue;
        OdStatus status = probe_socket(AF_INET, SOCK_STREAM, port, error);
        if (status == OD_OK) status = probe_socket(AF_INET, SOCK_DGRAM, port, error);
        if (status == OD_OK) status = probe_socket(AF_INET6, SOCK_STREAM, port, error);
        if (status == OD_OK) status = probe_socket(AF_INET6, SOCK_DGRAM, port, error);
        if (status != OD_OK) return status;
    }
    od_error_clear(error);
    return OD_OK;
}

static OdStatus ensure_directory(const char *path, OdError *error) {
    struct stat information;
    if (lstat(path, &information) == 0) {
        if (!S_ISDIR(information.st_mode)) {
            od_error_set(error, OD_ERROR_INVALID, "%s is not a directory", path);
            return OD_ERROR_INVALID;
        }
        return OD_OK;
    }
    if (errno != ENOENT) {
        od_error_set(error, OD_ERROR_IO, "unable to inspect directory %s: %s",
                     path, strerror(errno));
        return OD_ERROR_IO;
    }
    if (mkdir(path, (mode_t)0777) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to create directory %s: %s",
                     path, strerror(errno));
        return OD_ERROR_IO;
    }
    return OD_OK;
}

static OdStatus ensure_parent_directories(const char *path, OdError *error) {
    char *copy = copy_string(path);
    if (copy == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare parent directory");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = OD_OK;
    for (char *cursor = copy + (copy[0] == '/' ? 1 : 0); *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (copy[0] != '\0') status = ensure_directory(copy, error);
        *cursor = '/';
        if (status != OD_OK) break;
    }
    free(copy);
    return status;
}

static char *parent_directory(const char *path) {
    char *parent = copy_string(path);
    if (parent == NULL) return NULL;
    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        (void)strcpy(parent, ".");
    } else if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return parent;
}

static OdStatus sync_parent(const char *path, OdError *error) {
    char *parent = parent_directory(path);
    if (parent == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to identify parent directory");
        return OD_ERROR_MEMORY;
    }
    int descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to open parent directory: %s", strerror(errno));
        return OD_ERROR_IO;
    }
    int result = fsync(descriptor);
    int sync_error = errno;
    (void)close(descriptor);
    if (result != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to sync parent directory: %s",
                     strerror(sync_error));
        return OD_ERROR_IO;
    }
    return OD_OK;
}

static OdStatus write_all(int descriptor,
                          const char *data,
                          size_t length,
                          OdError *error) {
    size_t written = 0U;
    while (written < length) {
        ssize_t count = write(descriptor, data + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            od_error_set(error, OD_ERROR_IO, "unable to write temporary managed file");
            return OD_ERROR_IO;
        }
        written += (size_t)count;
    }
    return OD_OK;
}

static OdStatus atomic_replace(const char *path,
                               const char *data,
                               size_t length,
                               mode_t mode,
                               bool enforce_mode,
                               OdError *error) {
    size_t capacity = strlen(path) + 64U;
    char *temporary = malloc(capacity);
    if (temporary == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare temporary file");
        return OD_ERROR_MEMORY;
    }
    int descriptor = -1;
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        (void)snprintf(temporary, capacity, "%s.tmp.%ld.%u", path, (long)getpid(), attempt);
        descriptor = open(temporary,
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          (mode_t)0666);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to create temporary file for %s: %s",
                     path, strerror(errno));
        free(temporary);
        return OD_ERROR_IO;
    }
    OdStatus status = write_all(descriptor, data, length, error);
    if (status == OD_OK && enforce_mode && fchmod(descriptor, mode) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to preserve permissions for %s", path);
        status = OD_ERROR_IO;
    }
    if (status == OD_OK && fsync(descriptor) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to sync temporary file for %s", path);
        status = OD_ERROR_IO;
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        od_error_set(error, OD_ERROR_IO, "unable to close temporary file for %s", path);
        status = OD_ERROR_IO;
    }
    if (status == OD_OK && rename(temporary, path) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to replace %s: %s", path, strerror(errno));
        status = OD_ERROR_IO;
    }
    if (status == OD_OK) status = sync_parent(path, error);
    if (status != OD_OK) (void)unlink(temporary);
    free(temporary);
    return status;
}

static OdStatus backup_existing(const char *path,
                                bool exists,
                                mode_t mode,
                                OdError *error) {
    if (!exists) return OD_OK;
    char *data = NULL;
    size_t length = 0U;
    OdStatus status = read_regular_file(path, &data, &length, NULL, error);
    if (status != OD_OK) return status;
    size_t backup_capacity = strlen(path) + sizeof(".opendoor.bak");
    char *backup = malloc(backup_capacity);
    if (backup == NULL) {
        free(data);
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare backup path");
        return OD_ERROR_MEMORY;
    }
    (void)snprintf(backup, backup_capacity, "%s.opendoor.bak", path);
    bool backup_exists;
    mode_t backup_mode;
    status = inspect_regular_target(backup, &backup_exists, &backup_mode, error);
    (void)backup_exists;
    (void)backup_mode;
    if (status == OD_OK) status = atomic_replace(backup, data, length, mode, true, error);
    free(backup);
    free(data);
    return status;
}

static OdStatus replace_with_backup(const char *path,
                                    const char *data,
                                    size_t length,
                                    OdError *error) {
    bool exists;
    mode_t mode;
    OdStatus status = inspect_regular_target(path, &exists, &mode, error);
    if (status == OD_OK) status = backup_existing(path, exists, mode, error);
    if (status == OD_OK) status = atomic_replace(path, data, length, mode, exists, error);
    return status;
}

static const char *profile_reference(const char *project_root, const char *profile_path) {
    size_t root_length = strlen(project_root);
    if (strncmp(project_root, profile_path, root_length) == 0 &&
        profile_path[root_length] == '/') return profile_path + root_length + 1U;
    return profile_path;
}

static OdStatus project_save_with_policy(const char *project_root,
                                         const char *profile_path,
                                         const OdProfile *profile,
                                         const OdAllocationPlan *plan,
                                         bool import_foreign,
                                         OdError *error) {
    if (project_root == NULL || profile_path == NULL || profile == NULL || plan == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "project save inputs are required");
        return OD_ERROR_INVALID;
    }
    OdStatus status = od_profile_validate(profile, error);
    char *assignment_path = NULL;
    if (status == OD_OK) {
        status = join_path(project_root, profile->assignment_file, &assignment_path, error);
    }

    bool assignment_exists = false;
    mode_t assignment_mode;
    if (status == OD_OK) {
        status = inspect_regular_target(assignment_path, &assignment_exists,
                                        &assignment_mode, error);
    }
    if (status == OD_OK && assignment_exists) {
        OdAssignments existing;
        status = od_assignments_load_file(assignment_path, &existing, error);
        if (status == OD_ERROR_FOREIGN && import_foreign) {
            status = od_assignments_import_file(assignment_path, &existing, error);
        }
        if (status == OD_OK) od_assignments_free(&existing);
    }
    bool profile_exists;
    mode_t profile_mode;
    if (status == OD_OK) {
        status = inspect_regular_target(profile_path, &profile_exists, &profile_mode, error);
        (void)profile_exists;
        (void)profile_mode;
    }

    char *profile_text = NULL;
    size_t profile_length = 0U;
    OdAssignments assignments;
    od_assignments_init(&assignments);
    char *assignment_text = NULL;
    size_t assignment_length = 0U;
    if (status == OD_OK) {
        status = od_profile_render(profile, &profile_text, &profile_length, error);
    }
    if (status == OD_OK) status = od_plan_to_assignments(plan, &assignments, error);
    if (status == OD_OK) {
        status = od_assignments_render(&assignments,
                                       profile_reference(project_root, profile_path),
                                       &assignment_text, &assignment_length, error);
    }
    if (status == OD_OK) status = ensure_parent_directories(profile_path, error);
    if (status == OD_OK) {
        status = replace_with_backup(profile_path, profile_text, profile_length, error);
    }
    if (status == OD_OK) {
        status = replace_with_backup(assignment_path, assignment_text,
                                     assignment_length, error);
    }
    free(profile_text);
    free(assignment_text);
    od_assignments_free(&assignments);
    free(assignment_path);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

OdStatus od_project_save(const char *project_root,
                         const char *profile_path,
                         const OdProfile *profile,
                         const OdAllocationPlan *plan,
                         OdError *error) {
    return project_save_with_policy(project_root, profile_path, profile, plan, false, error);
}

OdStatus od_project_save_importing_foreign(const char *project_root,
                                           const char *profile_path,
                                           const OdProfile *profile,
                                           const OdAllocationPlan *plan,
                                           OdError *error) {
    return project_save_with_policy(project_root, profile_path, profile, plan, true, error);
}

OdStatus od_project_reset_assignments(const char *project_root,
                                      const OdProfile *profile,
                                      OdError *error) {
    if (project_root == NULL || profile == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "project root and profile are required");
        return OD_ERROR_INVALID;
    }
    char *path = NULL;
    OdStatus status = join_path(project_root, profile->assignment_file, &path, error);
    bool exists = false;
    mode_t mode = (mode_t)0644;
    if (status == OD_OK) status = inspect_regular_target(path, &exists, &mode, error);
    if (status == OD_OK && !exists) {
        free(path);
        od_error_clear(error);
        return OD_OK;
    }
    if (status == OD_OK) {
        OdAssignments assignments;
        status = od_assignments_load_file(path, &assignments, error);
        if (status == OD_OK) od_assignments_free(&assignments);
    }
    if (status == OD_OK) status = backup_existing(path, true, mode, error);
    if (status == OD_OK && unlink(path) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to remove %s: %s", path, strerror(errno));
        status = OD_ERROR_IO;
    }
    if (status == OD_OK) status = sync_parent(path, error);
    free(path);
    if (status == OD_OK) od_error_clear(error);
    return status;
}
