#include "opendoor/settings_store.h"

#include "opendoor/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

OdStatus od_settings_resolve_path(char *path, size_t capacity, OdError *error) {
    if (path == NULL || capacity == 0U) {
        od_error_set(error, OD_ERROR_INVALID, "settings path output is required");
        return OD_ERROR_INVALID;
    }
    const char *base = getenv("XDG_CONFIG_HOME");
    char fallback[4096];
    if (base == NULL || base[0] != '/') {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] != '/') {
            od_error_set(error, OD_ERROR_INVALID,
                         "HOME or absolute XDG_CONFIG_HOME is required for settings");
            return OD_ERROR_INVALID;
        }
        int fallback_count = snprintf(fallback, sizeof(fallback), "%s/.config", home);
        if (fallback_count < 0 || (size_t)fallback_count >= sizeof(fallback)) {
            od_error_set(error, OD_ERROR_INVALID, "settings base path is too long");
            return OD_ERROR_INVALID;
        }
        base = fallback;
    }
    int count = snprintf(path, capacity, "%s/opendoor/settings.toml", base);
    if (count < 0 || (size_t)count >= capacity) {
        od_error_set(error, OD_ERROR_INVALID, "settings path is too long");
        return OD_ERROR_INVALID;
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
    if (errno != ENOENT || mkdir(path, (mode_t)0700) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to create settings directory %s: %s",
                     path, strerror(errno));
        return OD_ERROR_IO;
    }
    return OD_OK;
}

static OdStatus ensure_parents(const char *path, OdError *error) {
    size_t length = strlen(path) + 1U;
    char *copy = malloc(length);
    if (copy == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare settings path");
        return OD_ERROR_MEMORY;
    }
    memcpy(copy, path, length);
    OdStatus status = OD_OK;
    for (char *cursor = copy + (copy[0] == '/' ? 1 : 0); *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        status = ensure_directory(copy, error);
        *cursor = '/';
        if (status != OD_OK) break;
    }
    free(copy);
    return status;
}

static OdStatus inspect_target(const char *path,
                               bool *exists,
                               mode_t *mode,
                               OdError *error) {
    struct stat information;
    if (lstat(path, &information) == 0) {
        if (!S_ISREG(information.st_mode)) {
            od_error_set(error, OD_ERROR_INVALID,
                         "refusing non-regular settings target: %s", path);
            return OD_ERROR_INVALID;
        }
        *exists = true;
        *mode = information.st_mode & (mode_t)0777;
        return OD_OK;
    }
    if (errno != ENOENT) {
        od_error_set(error, OD_ERROR_IO, "unable to inspect %s: %s", path, strerror(errno));
        return OD_ERROR_IO;
    }
    *exists = false;
    *mode = (mode_t)0600;
    return OD_OK;
}

static OdStatus write_settings_file(const char *path,
                                    const char *text,
                                    size_t length,
                                    bool preserve_mode,
                                    mode_t mode,
                                    OdError *error) {
    size_t temporary_capacity = strlen(path) + 48U;
    char *temporary = malloc(temporary_capacity);
    if (temporary == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare settings temporary file");
        return OD_ERROR_MEMORY;
    }
    int descriptor = -1;
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        (void)snprintf(temporary, temporary_capacity, "%s.tmp.%ld.%u",
                       path, (long)getpid(), attempt);
        descriptor = open(temporary,
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          (mode_t)0600);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        od_error_set(error, OD_ERROR_IO, "unable to create settings temporary file: %s",
                     strerror(errno));
        free(temporary);
        return OD_ERROR_IO;
    }
    OdStatus status = OD_OK;
    size_t written = 0U;
    while (written < length) {
        ssize_t count = write(descriptor, text + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            od_error_set(error, OD_ERROR_IO, "unable to write settings temporary file");
            status = OD_ERROR_IO;
            break;
        }
        written += (size_t)count;
    }
    if (status == OD_OK && preserve_mode && fchmod(descriptor, mode) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to preserve settings permissions");
        status = OD_ERROR_IO;
    }
    if (status == OD_OK && fsync(descriptor) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to sync settings");
        status = OD_ERROR_IO;
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        od_error_set(error, OD_ERROR_IO, "unable to close settings temporary file");
        status = OD_ERROR_IO;
    }
    if (status == OD_OK && rename(temporary, path) != 0) {
        od_error_set(error, OD_ERROR_IO, "unable to replace settings: %s", strerror(errno));
        status = OD_ERROR_IO;
    }
    if (status != OD_OK) (void)unlink(temporary);
    free(temporary);
    return status;
}

OdStatus od_settings_save(const char *path, const OdSettings *settings, OdError *error) {
    if (path == NULL || settings == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "settings path and values are required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = od_settings_render(settings, &text, &length, error);
    if (status == OD_OK) status = ensure_parents(path, error);
    bool exists = false;
    mode_t mode = (mode_t)0600;
    if (status == OD_OK) status = inspect_target(path, &exists, &mode, error);
    if (status == OD_OK) {
        status = write_settings_file(path, text, length, exists, mode, error);
    }
    free(text);
    if (status == OD_OK) od_error_clear(error);
    return status;
}
