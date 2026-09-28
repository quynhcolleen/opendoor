#include "opendoor/dashboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define OD_PORT_COUNT 65536U

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const OdAssignments *wanted,
                           const OdScanSnapshot *snapshot,
                           OdError *error) {
    if (dashboard == NULL || wanted == NULL || snapshot == NULL ||
        (snapshot->endpoint_count > 0U && snapshot->endpoints == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "wanted ports and a scan snapshot are required");
        return OD_ERROR_INVALID;
    }
    *dashboard = (OdDashboard){.page_size = 10U};
    size_t *configured = calloc(OD_PORT_COUNT, sizeof(*configured));
    bool *running = calloc(OD_PORT_COUNT, sizeof(*running));
    const OdEndpoint **owners = calloc(OD_PORT_COUNT, sizeof(*owners));
    if (configured == NULL || running == NULL || owners == NULL) {
        free(configured);
        free(running);
        free(owners);
        od_error_set(error, OD_ERROR_MEMORY, "unable to build dashboard");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < wanted->count; ++index) {
        ++configured[wanted->items[index].port];
    }
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        uint16_t port = endpoint->local_port;
        if (port == 0U) continue;
        running[port] = true;
        if (owners[port] == NULL ||
            (owners[port]->process[0] == '\0' && endpoint->process[0] != '\0')) {
            owners[port] = endpoint;
        }
    }
    for (unsigned port = 1U; port <= 65535U; ++port) {
        if (configured[port] > 0U || running[port]) ++dashboard->count;
    }
    if (dashboard->count > 0U) {
        dashboard->rows = calloc(dashboard->count, sizeof(*dashboard->rows));
        if (dashboard->rows == NULL) {
            free(configured);
            free(running);
            free(owners);
            *dashboard = (OdDashboard){0};
            od_error_set(error, OD_ERROR_MEMORY, "unable to store dashboard rows");
            return OD_ERROR_MEMORY;
        }
    }
    size_t output = 0U;
    for (unsigned port = 1U; port <= 65535U; ++port) {
        if (configured[port] == 0U && !running[port]) continue;
        OdPortRow *row = &dashboard->rows[output++];
        *row = (OdPortRow){
            .port = (uint16_t)port,
            .running = running[port],
            .conflict = (configured[port] > 0U && running[port]) ||
                        configured[port] > 1U
        };
        const OdEndpoint *owner = owners[port];
        (void)snprintf(row->process, sizeof(row->process), "%s",
                       owner != NULL && owner->process[0] != '\0' ?
                           owner->process : "-");
        (void)snprintf(row->directory, sizeof(row->directory), "%s",
                       owner != NULL && owner->directory[0] != '\0' ?
                           owner->directory : "-");
    }
    free(configured);
    free(running);
    free(owners);
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
