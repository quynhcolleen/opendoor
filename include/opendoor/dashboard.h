#ifndef OPENDOOR_DASHBOARD_H
#define OPENDOOR_DASHBOARD_H

#include "opendoor/allocation.h"
#include "opendoor/common.h"
#include "opendoor/model.h"
#include "opendoor/scan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OD_SERVICE_AVAILABLE,
    OD_SERVICE_IN_USE,
    OD_SERVICE_REASSIGNED,
    OD_SERVICE_SAVED,
    OD_SERVICE_STALE
} OdServiceStatus;

typedef struct {
    const char *stable_id;
    const char *service;
    const char *group;
    const char *variable;
    uint16_t preferred_port;
    uint16_t selected_port;
    OdServiceStatus status;
    bool conflict;
    char conflict_detail[192];
} OdServiceRow;

typedef enum {
    OD_WIDGET_SERVICES,
    OD_WIDGET_CONFLICTS,
    OD_WIDGET_LISTENERS,
    OD_WIDGET_DOCKER,
    OD_WIDGET_COUNT
} OdDashboardWidget;

typedef enum {
    OD_SERVICE_SORT_NAME,
    OD_SERVICE_SORT_GROUP,
    OD_SERVICE_SORT_VARIABLE,
    OD_SERVICE_SORT_PREFERRED,
    OD_SERVICE_SORT_SELECTED,
    OD_SERVICE_SORT_STATUS,
    OD_SERVICE_SORT_CONFLICT,
    OD_SERVICE_SORT_COUNT
} OdServiceSort;

typedef enum {
    OD_LISTENER_SORT_PORT,
    OD_LISTENER_SORT_PROTOCOL,
    OD_LISTENER_SORT_BIND,
    OD_LISTENER_SORT_PROCESS,
    OD_LISTENER_SORT_PID,
    OD_LISTENER_SORT_USER,
    OD_LISTENER_SORT_SOURCE,
    OD_LISTENER_SORT_COUNT
} OdListenerSort;

typedef enum {
    OD_DOCKER_SORT_CONTAINER,
    OD_DOCKER_SORT_HOST_PORT,
    OD_DOCKER_SORT_CONTAINER_PORT,
    OD_DOCKER_SORT_PROTOCOL,
    OD_DOCKER_SORT_PROJECT,
    OD_DOCKER_SORT_COUNT
} OdDockerSort;

typedef struct {
    size_t managed;
    size_t available;
    size_t conflicts;
    size_t reassigned;
    size_t listeners;
    size_t docker_mappings;
} OdDashboardSummary;

typedef struct {
    char project_name[128];
    OdServiceRow *services;
    size_t service_count;
    size_t *visible_order;
    size_t visible_count;
    size_t selected_visible;
    const char *selected_id;
    size_t page_start;
    size_t page_size;
    char search[128];
    OdServiceSort sort;
    bool sort_ascending;
    bool profile_saved;
    bool auto_refresh;
    unsigned refresh_seconds;
    OdDashboardWidget focused;
    bool expanded;
    size_t conflict_selected;
    size_t conflict_page_start;
    size_t conflict_page_size;
    size_t *conflict_order;
    size_t conflict_visible_count;
    char conflict_search[128];
    bool conflict_sort_ascending;
    size_t listener_selected;
    size_t listener_page_start;
    size_t listener_page_size;
    size_t *listener_order;
    size_t listener_visible_count;
    char listener_search[128];
    OdListenerSort listener_sort;
    bool listener_sort_ascending;
    size_t docker_selected;
    size_t docker_page_start;
    size_t docker_page_size;
    size_t *docker_order;
    size_t docker_visible_count;
    char docker_search[128];
    OdDockerSort docker_sort;
    bool docker_sort_ascending;
    const OdScanSnapshot *snapshot;
    OdDashboardSummary summary;
} OdDashboard;

typedef enum {
    OD_HIT_FOCUS_WIDGET,
    OD_HIT_SELECT_ROW,
    OD_HIT_TOGGLE_EXPAND,
    OD_HIT_SORT_COLUMN,
    OD_HIT_SCROLL_UP,
    OD_HIT_SCROLL_DOWN
} OdHitAction;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    OdHitAction action;
    OdDashboardWidget widget;
    size_t target;
} OdHitRegion;

typedef struct {
    OdHitRegion *items;
    size_t count;
} OdHitMap;

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const OdProfile *profile,
                           const OdScanSnapshot *snapshot,
                           const OdAllocationPlan *plan,
                           OdError *error);
void od_dashboard_free(OdDashboard *dashboard);
const char *od_service_status_name(OdServiceStatus status);
OdStatus od_dashboard_search(OdDashboard *dashboard, const char *query, OdError *error);
OdStatus od_dashboard_search_focused(OdDashboard *dashboard,
                                     const char *query,
                                     OdError *error);
void od_dashboard_sort(OdDashboard *dashboard, OdServiceSort sort);
void od_dashboard_sort_column(OdDashboard *dashboard,
                              OdDashboardWidget widget,
                              size_t column);
void od_dashboard_sort_focused(OdDashboard *dashboard);
void od_dashboard_reverse_sort_focused(OdDashboard *dashboard);
void od_dashboard_move(OdDashboard *dashboard, int rows);
void od_dashboard_move_page(OdDashboard *dashboard, int pages);
void od_dashboard_home(OdDashboard *dashboard);
void od_dashboard_end(OdDashboard *dashboard);
void od_dashboard_set_page_size(OdDashboard *dashboard, size_t rows);
const OdServiceRow *od_dashboard_selected_service(const OdDashboard *dashboard);
void od_dashboard_focus_next(OdDashboard *dashboard, int direction);
void od_dashboard_toggle_expand(OdDashboard *dashboard);
void od_dashboard_set_widget_page_size(OdDashboard *dashboard,
                                       OdDashboardWidget widget,
                                       size_t rows);
void od_dashboard_move_focused(OdDashboard *dashboard, int rows);
void od_dashboard_navigate_focused(OdDashboard *dashboard, int rows);
void od_dashboard_move_focused_page(OdDashboard *dashboard, int pages);
void od_dashboard_home_focused(OdDashboard *dashboard);
void od_dashboard_end_focused(OdDashboard *dashboard);
void od_dashboard_restore_secondary_selection(OdDashboard *destination,
                                              const OdDashboard *source);
void od_hitmap_init(OdHitMap *map);
void od_hitmap_free(OdHitMap *map);
const OdHitRegion *od_hitmap_at(const OdHitMap *map, int x, int y);

#endif
