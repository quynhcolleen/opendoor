#include "opendoor/display.h"

#include <stdio.h>
#include <string.h>

static const char *source_kind_name(OdPortSourceKind kind) {
    switch (kind) {
        case OD_SOURCE_ENV: return "env";
        case OD_SOURCE_COMPOSE: return "compose";
        case OD_SOURCE_PACKAGE_JSON: return "package.json";
        case OD_SOURCE_MAKEFILE: return "makefile";
    }
    return "source";
}

static bool format_source(const char *kind,
                          const char *key,
                          const char *paths,
                          char *destination,
                          size_t capacity) {
    const char *key_prefix = key == NULL || key[0] == '\0' ? "" : " (";
    const char *key_text = key == NULL ? "" : key;
    const char *key_suffix = key == NULL || key[0] == '\0' ? "" : ")";
    const char *path_prefix = paths == NULL || paths[0] == '\0' ? "" : " — ";
    const char *path_text = paths == NULL ? "" : paths;
    int written = snprintf(destination, capacity, "%s%s%s%s%s%s",
                           kind, key_prefix, key_text, key_suffix,
                           path_prefix, path_text);
    return written >= 0 && (size_t)written < capacity;
}

bool od_display_relative_folder(const char *relative_path,
                                char *destination,
                                size_t capacity) {
    if (destination == NULL || capacity == 0U || relative_path == NULL ||
        relative_path[0] == '\0') {
        return false;
    }
    destination[0] = '\0';
    const char *slash = strrchr(relative_path, '/');
    if (slash == NULL || (slash == relative_path + 1 && relative_path[0] == '.')) {
        return snprintf(destination, capacity, "./") == 2;
    }
    size_t length = (size_t)(slash - relative_path);
    if (relative_path[0] == '.' && relative_path[1] == '/') {
        if (length >= capacity) return false;
        memcpy(destination, relative_path, length);
        destination[length] = '\0';
        return true;
    }
    int written = snprintf(destination, capacity, "./%.*s", (int)length,
                           relative_path);
    return written >= 0 && (size_t)written < capacity;
}

bool od_display_source(OdPortSourceKind source_kind,
                       const char *environment_key,
                       const char *contributing_paths,
                       char *destination,
                       size_t capacity) {
    if (destination == NULL || capacity == 0U) return false;
    destination[0] = '\0';
    const char *kind = source_kind_name(source_kind);
    if (format_source(kind, environment_key, contributing_paths,
                      destination, capacity)) {
        return true;
    }
    destination[0] = '\0';
    return format_source(kind, NULL, contributing_paths, destination, capacity);
}

bool od_display_history_when(const char *timestamp,
                             char *destination,
                             size_t capacity) {
    if (destination == NULL || capacity < 20U || timestamp == NULL ||
        strlen(timestamp) < 19U || timestamp[10] != 'T') {
        return false;
    }
    memcpy(destination, timestamp, 10U);
    destination[10] = ' ';
    memcpy(destination + 11U, timestamp + 11U, 8U);
    destination[19] = '\0';
    return true;
}
