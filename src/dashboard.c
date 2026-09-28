#include "opendoor/dashboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pid_t pid;
    uint16_t port;
    bool project_owned;
    char relative_folder[OD_PATH_CAP];
} LiveRecord;

static bool checked_copy(char *destination, size_t capacity, const char *source) {
    size_t length = strlen(source);
    if (length >= capacity) return false;
    strcpy(destination, source);
    return true;
}

static bool path_is_within(const char *root, const char *candidate) {
    size_t root_length = strlen(root);
    return strcmp(root, candidate) == 0 ||
           (strncmp(root, candidate, root_length) == 0 &&
            candidate[root_length] == '/');
}

static bool owner_directory(const OdEndpoint *endpoint,
                            char **canonical_directory) {
    *canonical_directory = NULL;
    if (endpoint->directory[0] != '\0') {
        *canonical_directory = realpath(endpoint->directory, NULL);
        return *canonical_directory != NULL;
    }
    if (endpoint->executable[0] == '\0') return false;
    char *canonical_executable = realpath(endpoint->executable, NULL);
    if (canonical_executable == NULL) return false;
    char *slash = strrchr(canonical_executable, '/');
    if (slash == NULL) {
        free(canonical_executable);
        return false;
    }
    if (slash == canonical_executable) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    *canonical_directory = canonical_executable;
    return true;
}

static bool relative_owner_folder(const char *root,
                                  const char *owner,
                                  char *relative,
                                  size_t capacity) {
    if (!path_is_within(root, owner)) return false;
    if (strcmp(root, owner) == 0) return checked_copy(relative, capacity, "./");
    size_t suffix_length = strlen(owner) - strlen(root);
    if (suffix_length > SIZE_MAX - 2U || suffix_length + 2U > capacity) return false;
    relative[0] = '.';
    memcpy(relative + 1U, owner + strlen(root), suffix_length);
    relative[suffix_length + 1U] = '\0';
    return true;
}

static bool duplicate_live_record(const LiveRecord *records,
                                  size_t count,
                                  pid_t pid,
                                  uint16_t port) {
    for (size_t index = 0U; index < count; ++index) {
        if (records[index].pid == pid && records[index].port == port) return true;
    }
    return false;
}

static bool duplicate_declaration_row(const OdDashboard *dashboard,
                                      const OdPortDeclaration *declaration) {
    for (size_t index = 0U; index < dashboard->count; ++index) {
        const OdPortRow *row = &dashboard->rows[index];
        if (row->declared && row->port == declaration->port &&
            strcmp(row->source, declaration->relative_path) == 0) return true;
    }
    return false;
}

static bool port_is_declared(const OdDashboard *dashboard, uint16_t port) {
    for (size_t index = 0U; index < dashboard->count; ++index) {
        if (dashboard->rows[index].declared && dashboard->rows[index].port == port) {
            return true;
        }
    }
    return false;
}

static const LiveRecord *representative_project_record(
    const LiveRecord *records,
    size_t count,
    uint16_t port,
    const char *preferred_folder) {
    const LiveRecord *best = NULL;
    for (size_t index = 0U; index < count; ++index) {
        const LiveRecord *record = &records[index];
        if (!record->project_owned || record->port != port) continue;
        if (best == NULL) {
            best = record;
            continue;
        }
        bool record_exact = preferred_folder != NULL &&
                            strcmp(record->relative_folder, preferred_folder) == 0;
        bool best_exact = preferred_folder != NULL &&
                          strcmp(best->relative_folder, preferred_folder) == 0;
        if ((record_exact && !best_exact) ||
            (record_exact == best_exact &&
             strcmp(record->relative_folder, best->relative_folder) < 0)) {
            best = record;
        }
    }
    return best;
}

static bool any_other_record(const LiveRecord *records,
                             size_t count,
                             uint16_t port) {
    for (size_t index = 0U; index < count; ++index) {
        if (records[index].port == port && !records[index].project_owned) return true;
    }
    return false;
}

static int compare_rows(const void *left, const void *right) {
    const OdPortRow *first = left;
    const OdPortRow *second = right;
    int folder_order = strcmp(first->relative_folder, second->relative_folder);
    if (folder_order != 0) return folder_order;
    if (first->port < second->port) return -1;
    if (first->port > second->port) return 1;
    return strcmp(first->source, second->source);
}

static OdStatus store_row_strings(OdPortRow *row,
                                  const char *folder,
                                  const char *source,
                                  OdError *error) {
    if (folder == NULL || source == NULL || folder[0] != '.' || folder[1] != '/' ||
        (strcmp(source, "live") != 0 && (source[0] != '.' || source[1] != '/')) ||
        !checked_copy(row->relative_folder, sizeof(row->relative_folder), folder) ||
        !checked_copy(row->source, sizeof(row->source), source)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "dashboard paths must be bounded project-relative paths");
        return OD_ERROR_INVALID;
    }
    return OD_OK;
}

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const char *project_root,
                           const OdProjectDiscovery *discovery,
                           const OdScanSnapshot *snapshot,
                           OdError *error) {
    if (dashboard == NULL || project_root == NULL || discovery == NULL ||
        snapshot == NULL ||
        (discovery->count > 0U && discovery->items == NULL) ||
        (snapshot->endpoint_count > 0U && snapshot->endpoints == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "project discovery and a scan snapshot are required");
        return OD_ERROR_INVALID;
    }
    *dashboard = (OdDashboard){.page_size = 10U};
    char *canonical_root = realpath(project_root, NULL);
    if (canonical_root == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "unable to resolve dashboard project root");
        return OD_ERROR_INVALID;
    }
    LiveRecord *records = snapshot->endpoint_count == 0U ? NULL :
        calloc(snapshot->endpoint_count, sizeof(*records));
    size_t maximum_rows = discovery->count + snapshot->endpoint_count;
    dashboard->rows = maximum_rows == 0U ? NULL :
        calloc(maximum_rows, sizeof(*dashboard->rows));
    if ((snapshot->endpoint_count > 0U && records == NULL) ||
        (maximum_rows > 0U && dashboard->rows == NULL)) {
        free(records);
        free(canonical_root);
        od_dashboard_free(dashboard);
        od_error_set(error, OD_ERROR_MEMORY, "unable to build dashboard");
        return OD_ERROR_MEMORY;
    }

    size_t record_count = 0U;
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        if (endpoint->local_port == 0U ||
            duplicate_live_record(records, record_count, endpoint->pid,
                                  endpoint->local_port)) {
            continue;
        }
        LiveRecord *record = &records[record_count++];
        record->pid = endpoint->pid;
        record->port = endpoint->local_port;
        char *owner = NULL;
        if (owner_directory(endpoint, &owner) &&
            relative_owner_folder(canonical_root, owner, record->relative_folder,
                                  sizeof(record->relative_folder))) {
            record->project_owned = true;
        }
        free(owner);
    }

    OdStatus status = OD_OK;
    for (size_t index = 0U; index < discovery->count && status == OD_OK; ++index) {
        const OdPortDeclaration *declaration = &discovery->items[index];
        if (declaration->port == 0U || declaration->relative_path == NULL ||
            declaration->relative_folder == NULL ||
            duplicate_declaration_row(dashboard, declaration)) {
            continue;
        }
        OdPortRow *row = &dashboard->rows[dashboard->count];
        *row = (OdPortRow){.port = declaration->port, .declared = true};
        status = store_row_strings(row, declaration->relative_folder,
                                   declaration->relative_path, error);
        if (status != OD_OK) break;
        const LiveRecord *owner = representative_project_record(
            records, record_count, declaration->port, declaration->relative_folder);
        if (owner != NULL) {
            row->status = OD_PORT_RUNNING;
        } else if (any_other_record(records, record_count, declaration->port)) {
            row->status = OD_PORT_IN_USE_OTHER;
        } else {
            row->status = OD_PORT_NOT_RUNNING;
        }
        ++dashboard->count;
    }

    for (size_t index = 0U; index < record_count && status == OD_OK; ++index) {
        const LiveRecord *record = &records[index];
        if (!record->project_owned || port_is_declared(dashboard, record->port)) continue;
        const LiveRecord *best = representative_project_record(
            records, record_count, record->port, NULL);
        if (best != record) continue;
        OdPortRow *row = &dashboard->rows[dashboard->count];
        *row = (OdPortRow){
            .port = record->port,
            .status = OD_PORT_RUNNING
        };
        status = store_row_strings(row, record->relative_folder, "live", error);
        if (status != OD_OK) break;
        ++dashboard->count;
    }
    free(records);
    free(canonical_root);
    if (status != OD_OK) {
        od_dashboard_free(dashboard);
        return status;
    }
    qsort(dashboard->rows, dashboard->count, sizeof(*dashboard->rows), compare_rows);
    od_error_clear(error);
    return OD_OK;
}

void od_dashboard_free(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    free(dashboard->rows);
    *dashboard = (OdDashboard){0};
}

void od_dashboard_set_page_size(OdDashboard *dashboard, size_t rows) {
    if (dashboard == NULL) return;
    dashboard->page_size = rows == 0U ? 1U : rows;
    size_t maximum = dashboard->count > dashboard->page_size ?
        dashboard->count - dashboard->page_size : 0U;
    if (dashboard->scroll > maximum) dashboard->scroll = maximum;
}

void od_dashboard_scroll(OdDashboard *dashboard, int rows) {
    if (dashboard == NULL) return;
    size_t page_size = dashboard->page_size == 0U ? 1U : dashboard->page_size;
    size_t maximum = dashboard->count > page_size ?
        dashboard->count - page_size : 0U;
    long long target = (long long)dashboard->scroll + (long long)rows;
    if (target < 0LL) {
        dashboard->scroll = 0U;
    } else if ((unsigned long long)target > (unsigned long long)maximum) {
        dashboard->scroll = maximum;
    } else {
        dashboard->scroll = (size_t)target;
    }
}
