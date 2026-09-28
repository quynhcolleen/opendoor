#ifndef OPENDOOR_DASHBOARD_H
#define OPENDOOR_DASHBOARD_H

#include "opendoor/dotenv.h"
#include "opendoor/scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t port;
    bool running;
    bool conflict;
    char process[OD_PROCESS_CAP];
    char directory[OD_PATH_CAP];
} OdPortRow;

typedef struct {
    OdPortRow *rows;
    size_t count;
    size_t scroll;
    size_t page_size;
} OdDashboard;

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const OdAssignments *wanted,
                           const OdScanSnapshot *snapshot,
                           OdError *error);
void od_dashboard_free(OdDashboard *dashboard);
void od_dashboard_set_page_size(OdDashboard *dashboard, size_t rows);
void od_dashboard_scroll(OdDashboard *dashboard, int rows);

#endif
