#ifndef OPENDOOR_DASHBOARD_H
#define OPENDOOR_DASHBOARD_H

#include "opendoor/discovery.h"
#include "opendoor/scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_PORT_NOT_RUNNING = 0,
    OD_PORT_RUNNING,
    OD_PORT_IN_USE_OTHER
} OdPortStatus;

typedef struct {
    uint16_t port;
    OdPortStatus status;
    bool declared;
    char relative_folder[OD_PATH_CAP];
    char process[OD_PROCESS_CAP];
    char source[OD_PATH_CAP];
} OdPortRow;

typedef struct {
    OdPortRow *rows;
    size_t count;
    size_t scroll;
    size_t page_size;
} OdDashboard;

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const char *project_root,
                           const OdProjectDiscovery *discovery,
                           const OdScanSnapshot *snapshot,
                           OdError *error);
void od_dashboard_free(OdDashboard *dashboard);
void od_dashboard_set_page_size(OdDashboard *dashboard, size_t rows);
void od_dashboard_scroll(OdDashboard *dashboard, int rows);

#endif
