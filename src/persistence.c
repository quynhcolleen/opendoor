#include "opendoor/persistence.h"

#include "opendoor/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OD_PATCH_FILE_MAX (4U * 1024U * 1024U)

typedef struct {
    const char *path;
    const char *relative_path;
    OdPortSourceKind source_kind;
    int parent_fd;
    char *basename;
    char *original;
    size_t original_length;
    char *patched;
    size_t patched_length;
    mode_t mode;
    char *backup_name;
    bool touched;
} PatchFile;

static uint64_t hash_bytes(const char *text, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        hash ^= (uint64_t)(unsigned char)text[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static OdStatus write_all(int descriptor,
                          const char *text,
                          size_t length,
                          OdError *error) {
    size_t offset = 0U;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            od_error_set(error, OD_ERROR_IO, "unable to write file: %s",
                         strerror(errno));
            return OD_ERROR_IO;
        }
        offset += (size_t)count;
    }
    return OD_OK;
}

static int open_absolute_directory_nofollow(const char *path,
                                            OdError *error) {
    if (path == NULL || path[0] != '/') {
        od_error_set(error, OD_ERROR_INVALID,
                     "conflict project root must be absolute");
        return -1;
    }
    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to open filesystem root: %s",
                     strerror(errno));
        return -1;
    }
    char *copy = strdup(path + 1U);
    if (copy == NULL) {
        (void)close(current);
        od_error_set(error, OD_ERROR_MEMORY, "unable to open project root");
        return -1;
    }
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            free(copy);
            (void)close(current);
            od_error_set(error, OD_ERROR_INVALID,
                         "project root contains an unsafe path component");
            return -1;
        }
        int next = openat(current, part,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            int saved_errno = errno;
            free(copy);
            (void)close(current);
            od_error_set(error, OD_ERROR_CHANGED,
                         "project root changed before apply: %s",
                         strerror(saved_errno));
            return -1;
        }
        (void)close(current);
        current = next;
    }
    free(copy);
    return current;
}

static bool item_path_matches_root(const char *root,
                                   const OdResolutionItem *item) {
    if (root == NULL || item->absolute_path == NULL ||
        item->relative_path == NULL ||
        strncmp(item->relative_path, "./", 2U) != 0 ||
        item->relative_path[2] == '\0') {
        return false;
    }
    size_t root_length = strlen(root);
    const char *relative = item->relative_path + 2U;
    size_t relative_length = strlen(relative);
    if (root_length > SIZE_MAX - relative_length - 2U) return false;
    size_t length = root_length + relative_length + 2U;
    char *expected = malloc(length);
    if (expected == NULL) return false;
    int written = snprintf(expected, length, "%s/%s", root, relative);
    bool matches = written >= 0 && (size_t)written < length &&
                   strcmp(expected, item->absolute_path) == 0;
    free(expected);
    return matches;
}

static int open_parent_under_root(int root_fd,
                                  const char *relative_path,
                                  char **basename,
                                  OdError *error) {
    *basename = NULL;
    if (relative_path == NULL || strncmp(relative_path, "./", 2U) != 0 ||
        relative_path[2] == '\0' || relative_path[2] == '/' ||
        relative_path[strlen(relative_path) - 1U] == '/' ||
        strstr(relative_path, "//") != NULL) {
        od_error_set(error, OD_ERROR_INVALID, "unsafe conflict source path");
        return -1;
    }
    char *copy = strdup(relative_path + 2U);
    if (copy == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to open conflict source");
        return -1;
    }
    char *slash = strrchr(copy, '/');
    char *leaf = slash == NULL ? copy : slash + 1U;
    if (leaf[0] == '\0' || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        free(copy);
        od_error_set(error, OD_ERROR_INVALID, "unsafe conflict source name");
        return -1;
    }
    *basename = strdup(leaf);
    if (*basename == NULL) {
        free(copy);
        od_error_set(error, OD_ERROR_MEMORY, "unable to store conflict source name");
        return -1;
    }
    if (slash != NULL) *slash = '\0';
    int current = dup(root_fd);
    if (current < 0) {
        free(*basename);
        *basename = NULL;
        free(copy);
        od_error_set(error, OD_ERROR_IO, "unable to retain project root: %s",
                     strerror(errno));
        return -1;
    }
    if (slash != NULL) {
        char *save = NULL;
        for (char *part = strtok_r(copy, "/", &save); part != NULL;
             part = strtok_r(NULL, "/", &save)) {
            if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
                free(*basename);
                *basename = NULL;
                free(copy);
                (void)close(current);
                od_error_set(error, OD_ERROR_INVALID,
                             "unsafe conflict source directory");
                return -1;
            }
            int next = openat(current, part,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) {
                int saved_errno = errno;
                free(*basename);
                *basename = NULL;
                free(copy);
                (void)close(current);
                od_error_set(error, OD_ERROR_CHANGED,
                             "conflict source directory changed: %s",
                             strerror(saved_errno));
                return -1;
            }
            (void)close(current);
            current = next;
        }
    }
    free(copy);
    return current;
}

static OdStatus read_bounded_regular_at(int parent_fd,
                                        const char *name,
                                        char **text,
                                        size_t *length,
                                        mode_t *mode,
                                        OdError *error) {
    int descriptor = openat(parent_fd, name,
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_CHANGED,
                     "unable to open conflict source %s: %s",
                     name, strerror(errno));
        return OD_ERROR_CHANGED;
    }
    struct stat information;
    if (fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_size < 0 ||
        (uintmax_t)information.st_size > OD_PATCH_FILE_MAX) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_CHANGED,
                     "%s is no longer a bounded regular file", name);
        return OD_ERROR_CHANGED;
    }
    size_t size = (size_t)information.st_size;
    char *buffer = malloc(size + 1U);
    if (buffer == NULL) {
        (void)close(descriptor);
        od_error_set(error, OD_ERROR_MEMORY,
                     "unable to read conflict source %s", name);
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    while (used < size) {
        ssize_t count = read(descriptor, buffer + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            free(buffer);
            (void)close(descriptor);
            od_error_set(error, OD_ERROR_IO,
                         "unable to read conflict source %s", name);
            return OD_ERROR_IO;
        }
        used += (size_t)count;
    }
    if (close(descriptor) != 0) {
        free(buffer);
        od_error_set(error, OD_ERROR_IO,
                     "unable to close conflict source %s", name);
        return OD_ERROR_IO;
    }
    buffer[size] = '\0';
    *text = buffer;
    *length = size;
    if (mode != NULL) *mode = information.st_mode & 07777;
    return OD_OK;
}

static OdStatus sync_parent_directory(const char *path, OdError *error) {
    char *directory = strdup(path);
    if (directory == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to sync parent directory");
        return OD_ERROR_MEMORY;
    }
    char *slash = strrchr(directory, '/');
    if (slash == NULL) {
        free(directory);
        directory = strdup(".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    if (directory == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to sync parent directory");
        return OD_ERROR_MEMORY;
    }
    int descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to open parent directory: %s",
                     strerror(errno));
        return OD_ERROR_IO;
    }
    OdStatus status = OD_OK;
    if (fsync(descriptor) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to sync parent directory: %s",
                     strerror(errno));
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to close parent directory: %s",
                     strerror(errno));
    }
    return status;
}

static OdStatus atomic_replace(const char *path,
                               const char *text,
                               size_t length,
                               mode_t mode,
                               OdError *error) {
    size_t path_length = strlen(path);
    static const char suffix[] = ".opendoor.tmp.XXXXXX";
    if (path_length > SIZE_MAX - sizeof(suffix)) {
        od_error_set(error, OD_ERROR_INVALID, "patch path is too long");
        return OD_ERROR_INVALID;
    }
    char *temporary = malloc(path_length + sizeof(suffix));
    if (temporary == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate patch path");
        return OD_ERROR_MEMORY;
    }
    (void)snprintf(temporary, path_length + sizeof(suffix), "%s%s", path, suffix);
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        free(temporary);
        od_error_set(error, OD_ERROR_IO, "unable to create temporary file: %s",
                     strerror(errno));
        return OD_ERROR_IO;
    }
    OdStatus status = OD_OK;
    if (fchmod(descriptor, mode) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to preserve file mode: %s",
                     strerror(errno));
    }
    if (status == OD_OK) status = write_all(descriptor, text, length, error);
    if (status == OD_OK && fsync(descriptor) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to sync temporary file: %s",
                     strerror(errno));
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to close temporary file: %s",
                     strerror(errno));
    }
    if (status == OD_OK && rename(temporary, path) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to replace %s: %s", path,
                     strerror(errno));
    }
    if (status == OD_OK) status = sync_parent_directory(path, error);
    if (status != OD_OK) (void)unlink(temporary);
    free(temporary);
    return status;
}

static OdStatus atomic_replace_at(int parent_fd,
                                  const char *name,
                                  const char *text,
                                  size_t length,
                                  mode_t mode,
                                  OdError *error) {
    static unsigned long sequence = 0UL;
    char temporary[96];
    int descriptor = -1;
    for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
        ++sequence;
        int written = snprintf(temporary, sizeof(temporary),
                               ".opendoor.tmp.%ld.%lu",
                               (long)getpid(), sequence);
        if (written < 0 || (size_t)written >= sizeof(temporary)) {
            od_error_set(error, OD_ERROR_INVALID,
                         "unable to construct temporary filename");
            return OD_ERROR_INVALID;
        }
        descriptor = openat(parent_fd, temporary,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                            O_NOFOLLOW, 0600);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO,
                     "unable to create temporary file for %s: %s",
                     name, strerror(errno));
        return OD_ERROR_IO;
    }
    OdStatus status = OD_OK;
    if (fchmod(descriptor, mode) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to preserve file mode: %s",
                     strerror(errno));
    }
    if (status == OD_OK) status = write_all(descriptor, text, length, error);
    if (status == OD_OK && fsync(descriptor) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to sync temporary file: %s",
                     strerror(errno));
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to close temporary file: %s",
                     strerror(errno));
    }
    if (status == OD_OK &&
        renameat(parent_fd, temporary, parent_fd, name) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to replace %s: %s", name,
                     strerror(errno));
    }
    if (status == OD_OK && fsync(parent_fd) != 0) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to sync source directory: %s",
                     strerror(errno));
    }
    if (status != OD_OK) (void)unlinkat(parent_fd, temporary, 0);
    return status;
}

OdStatus od_config_write(const char *path,
                         const OdAssignments *assignments,
                         OdError *error) {
    if (path == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID,
                     "wanted-ports path and values are required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = od_config_render(assignments, &text, &length, error);
    if (status != OD_OK) return status;
    status = atomic_replace(path, text, length, 0600, error);
    free(text);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

static bool eligible_automatic_item(const OdResolutionItem *item) {
    return item->automatic &&
           ((item->source_kind == OD_SOURCE_ENV &&
             item->write_kind == OD_WRITE_ENV_LITERAL) ||
            (item->source_kind == OD_SOURCE_COMPOSE &&
             item->write_kind == OD_WRITE_COMPOSE_LITERAL));
}

static void patch_files_free(PatchFile *files, size_t count) {
    for (size_t index = 0U; index < count; ++index) {
        if (files[index].parent_fd >= 0) (void)close(files[index].parent_fd);
        free(files[index].basename);
        free(files[index].original);
        free(files[index].patched);
        free(files[index].backup_name);
    }
    free(files);
}

static size_t find_patch_file(const PatchFile *files,
                              size_t count,
                              const char *path) {
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(files[index].path, path) == 0) return index;
    }
    return SIZE_MAX;
}

static OdStatus add_patch_file(PatchFile **files,
                               size_t *count,
                               int root_fd,
                               const char *project_root,
                               const OdResolutionItem *item,
                               OdError *error) {
    if (find_patch_file(*files, *count, item->absolute_path) != SIZE_MAX) return OD_OK;
    PatchFile *grown = realloc(*files, (*count + 1U) * sizeof(*grown));
    if (grown == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to collect patch files");
        return OD_ERROR_MEMORY;
    }
    *files = grown;
    PatchFile *file = &grown[*count];
    *file = (PatchFile){
        .path = item->absolute_path,
        .relative_path = item->relative_path,
        .source_kind = item->source_kind,
        .parent_fd = -1
    };
    ++*count;
    if (!item_path_matches_root(project_root, item)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "conflict source is outside the selected project root");
        return OD_ERROR_INVALID;
    }
    file->parent_fd = open_parent_under_root(root_fd, item->relative_path,
                                             &file->basename, error);
    if (file->parent_fd < 0) return error->code;
    OdStatus status = read_bounded_regular_at(
        file->parent_fd, file->basename, &file->original,
        &file->original_length, &file->mode, error);
    if (status != OD_OK) return status;
    file->patched = malloc(file->original_length + 1U);
    if (file->patched == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy patch input");
        return OD_ERROR_MEMORY;
    }
    memcpy(file->patched, file->original, file->original_length + 1U);
    file->patched_length = file->original_length;
    size_t path_length = strlen(file->basename);
    static const char suffix[] = ".opendoor.bak";
    if (path_length > SIZE_MAX - sizeof(suffix)) {
        od_error_set(error, OD_ERROR_INVALID, "backup path is too long");
        return OD_ERROR_INVALID;
    }
    file->backup_name = malloc(path_length + sizeof(suffix));
    if (file->backup_name == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate backup path");
        return OD_ERROR_MEMORY;
    }
    (void)snprintf(file->backup_name, path_length + sizeof(suffix),
                   "%s%s", file->basename, suffix);
    return OD_OK;
}

static int compare_item_offsets_descending(const void *left, const void *right) {
    const OdResolutionItem *const *first = left;
    const OdResolutionItem *const *second = right;
    if ((*first)->byte_offset > (*second)->byte_offset) return -1;
    if ((*first)->byte_offset < (*second)->byte_offset) return 1;
    return 0;
}

static OdStatus verify_item_span(const PatchFile *file,
                                 const OdResolutionItem *item,
                                 OdError *error) {
    char old_port[6];
    int written = snprintf(old_port, sizeof(old_port), "%u",
                           (unsigned)item->old_port);
    bool stale = written < 0 || (size_t)written != item->byte_length ||
                 item->file_size != file->original_length ||
                 item->file_hash != hash_bytes(file->original,
                                               file->original_length) ||
                 item->byte_offset > file->original_length ||
                 item->byte_length > file->original_length - item->byte_offset ||
                 memcmp(file->original + item->byte_offset, old_port,
                        item->byte_length) != 0;
    if (stale) {
        od_error_set(error, OD_ERROR_CHANGED,
                     "%s changed since conflicts were scanned", item->relative_path);
        return OD_ERROR_CHANGED;
    }
    return OD_OK;
}

static OdStatus build_patched_file(PatchFile *file,
                                   const OdResolution *resolution,
                                   OdPatchValidator validator,
                                   void *context,
                                   OdError *error) {
    size_t item_count = 0U;
    for (size_t index = 0U; index < resolution->count; ++index) {
        if (eligible_automatic_item(&resolution->items[index]) &&
            strcmp(resolution->items[index].absolute_path, file->path) == 0) {
            ++item_count;
        }
    }
    const OdResolutionItem **items = calloc(item_count, sizeof(*items));
    if (items == NULL && item_count > 0U) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to sort patch spans");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < resolution->count; ++index) {
        const OdResolutionItem *item = &resolution->items[index];
        if (eligible_automatic_item(item) &&
            strcmp(item->absolute_path, file->path) == 0) {
            OdStatus status = verify_item_span(file, item, error);
            if (status != OD_OK) {
                free(items);
                return status;
            }
            items[output++] = item;
        }
    }
    qsort(items, item_count, sizeof(*items), compare_item_offsets_descending);
    size_t previous_offset = file->original_length;
    for (size_t index = 0U; index < item_count; ++index) {
        const OdResolutionItem *item = items[index];
        if (item->byte_offset + item->byte_length > previous_offset) {
            free(items);
            od_error_set(error, OD_ERROR_INVALID, "overlapping conflict spans in %s",
                         item->relative_path);
            return OD_ERROR_INVALID;
        }
        previous_offset = item->byte_offset;
        char replacement[6];
        int written = snprintf(replacement, sizeof(replacement), "%u",
                               (unsigned)item->new_port);
        if (written < 0 || (size_t)written >= sizeof(replacement)) {
            free(items);
            od_error_set(error, OD_ERROR_INVALID, "invalid replacement port");
            return OD_ERROR_INVALID;
        }
        size_t replacement_length = (size_t)written;
        size_t new_length = file->patched_length - item->byte_length +
                            replacement_length;
        char *patched = realloc(file->patched, new_length + 1U);
        if (patched == NULL) {
            free(items);
            od_error_set(error, OD_ERROR_MEMORY, "unable to build patched file");
            return OD_ERROR_MEMORY;
        }
        file->patched = patched;
        memmove(file->patched + item->byte_offset + replacement_length,
                file->patched + item->byte_offset + item->byte_length,
                file->patched_length - item->byte_offset - item->byte_length);
        memcpy(file->patched + item->byte_offset, replacement, replacement_length);
        file->patched_length = new_length;
        file->patched[new_length] = '\0';
    }
    free(items);
    return validator(file->source_kind, file->path, file->patched,
                     file->patched_length, OD_PATCH_VALIDATE_MEMORY,
                     context, error);
}

static bool file_bytes_equal_at(int parent_fd,
                                const char *name,
                                const char *expected,
                                size_t expected_length) {
    char *actual = NULL;
    size_t actual_length = 0U;
    mode_t mode = 0;
    OdError ignored;
    if (read_bounded_regular_at(parent_fd, name, &actual, &actual_length,
                                &mode, &ignored) != OD_OK) {
        return false;
    }
    bool equal = actual_length == expected_length &&
                 memcmp(actual, expected, expected_length) == 0;
    free(actual);
    return equal;
}

static OdStatus production_validator(OdPortSourceKind source_kind,
                                     const char *path,
                                     const char *text,
                                     size_t length,
                                     OdPatchValidationPhase phase,
                                     void *context,
                                     OdError *error) {
    (void)path;
    (void)phase;
    (void)context;
    return od_validate_discovery_text(source_kind, text, length, error);
}

OdStatus od_apply_resolution_with_validator(const OdResolution *resolution,
                                            OdPatchValidator validator,
                                            void *context,
                                            size_t *updated,
                                            OdError *error) {
    if (resolution == NULL || validator == NULL || updated == NULL ||
        (resolution->count > 0U && resolution->items == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "resolution, validator, and update count are required");
        return OD_ERROR_INVALID;
    }
    *updated = 0U;
    PatchFile *files = NULL;
    size_t file_count = 0U;
    size_t automatic_count = 0U;
    OdStatus status = OD_OK;
    bool has_automatic = false;
    for (size_t index = 0U; index < resolution->count; ++index) {
        if (resolution->items[index].automatic) {
            has_automatic = true;
            break;
        }
    }
    int root_fd = -1;
    if (has_automatic) {
        if (resolution->project_root == NULL ||
            resolution->project_root[0] == '\0') {
            od_error_set(error, OD_ERROR_INVALID,
                         "automatic conflicts require a selected project root");
            return OD_ERROR_INVALID;
        }
        root_fd = open_absolute_directory_nofollow(resolution->project_root, error);
        if (root_fd < 0) return error->code;
    }
    for (size_t index = 0U; index < resolution->count && status == OD_OK; ++index) {
        const OdResolutionItem *item = &resolution->items[index];
        if (!item->automatic) continue;
        if (!eligible_automatic_item(item) || item->absolute_path == NULL ||
            item->relative_path == NULL) {
            od_error_set(error, OD_ERROR_INVALID,
                         "automatic conflicts must be plain .env or Compose literals");
            status = OD_ERROR_INVALID;
            break;
        }
        ++automatic_count;
        status = add_patch_file(&files, &file_count, root_fd,
                                resolution->project_root, item, error);
        if (status == OD_OK) {
            size_t file_index = find_patch_file(files, file_count,
                                                item->absolute_path);
            if (file_index == SIZE_MAX ||
                files[file_index].source_kind != item->source_kind) {
                od_error_set(error, OD_ERROR_INVALID,
                             "one source file has conflicting format metadata");
                status = OD_ERROR_INVALID;
            }
        }
    }
    if (root_fd >= 0) (void)close(root_fd);
    for (size_t index = 0U; index < file_count && status == OD_OK; ++index) {
        status = build_patched_file(&files[index], resolution, validator,
                                    context, error);
    }
    for (size_t index = 0U; index < file_count && status == OD_OK; ++index) {
        status = atomic_replace_at(files[index].parent_fd,
                                   files[index].backup_name,
                                   files[index].original,
                                   files[index].original_length,
                                   files[index].mode, error);
    }
    for (size_t index = 0U; index < file_count && status == OD_OK; ++index) {
        PatchFile *file = &files[index];
        file->touched = true;
        status = atomic_replace_at(file->parent_fd, file->basename,
                                   file->patched, file->patched_length,
                                   file->mode, error);
        if (status != OD_OK) break;
        char *written_text = NULL;
        size_t written_length = 0U;
        mode_t written_mode = 0;
        status = read_bounded_regular_at(file->parent_fd, file->basename,
                                         &written_text, &written_length,
                                         &written_mode, error);
        if (status == OD_OK &&
            (written_length != file->patched_length ||
             memcmp(written_text, file->patched, written_length) != 0)) {
            status = OD_ERROR_CHANGED;
            od_error_set(error, status, "written bytes changed for %s", file->path);
        }
        if (status == OD_OK) {
            status = validator(file->source_kind, file->path, written_text,
                               written_length, OD_PATCH_VALIDATE_WRITTEN,
                               context, error);
        }
        free(written_text);
    }
    if (status != OD_OK) {
        OdStatus original_status = status;
        char original_message[OD_ERROR_MESSAGE_CAP];
        (void)snprintf(original_message, sizeof(original_message), "%s", error->message);
        bool rollback_ok = true;
        const char *failed_rollback_path = NULL;
        for (size_t index = 0U; index < file_count; ++index) {
            PatchFile *file = &files[index];
            if (!file->touched) continue;
            OdError rollback_error;
            if (atomic_replace_at(file->parent_fd, file->basename,
                                  file->original, file->original_length,
                                  file->mode, &rollback_error) != OD_OK ||
                !file_bytes_equal_at(file->parent_fd, file->basename,
                                     file->original,
                                     file->original_length)) {
                rollback_ok = false;
                if (failed_rollback_path == NULL) {
                    failed_rollback_path = file->relative_path;
                }
            }
        }
        if (rollback_ok) {
            od_error_set(error, original_status,
                         "%s; no automatic conflicts were applied; rollback completed",
                         original_message);
        } else {
            od_error_set(error, original_status,
                         "rollback incomplete for %s; backups retained; original failure: %.96s",
                         failed_rollback_path == NULL ? "an updated file" :
                                                       failed_rollback_path,
                         original_message);
        }
        patch_files_free(files, file_count);
        return original_status;
    }
    *updated = automatic_count;
    patch_files_free(files, file_count);
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_apply_resolution(const OdResolution *resolution,
                             size_t *updated,
                             OdError *error) {
    return od_apply_resolution_with_validator(resolution, production_validator,
                                              NULL, updated, error);
}
