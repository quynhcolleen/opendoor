#include "opendoor/dashboard.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool port_occupied(const OdScanSnapshot *snapshot, uint16_t port, char *detail, size_t capacity) {
    if (snapshot == NULL) return false;
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        if (endpoint->local_port != port) continue;
        (void)snprintf(detail, capacity, "%s%s%s%s%ld",
                       endpoint->process[0] == '\0' ? "host listener" : endpoint->process,
                       endpoint->local_address[0] == '\0' ? "" : " on ",
                       endpoint->local_address,
                       endpoint->pid == 0 ? "" : " pid ",
                       endpoint->pid == 0 ? 0L : (long)endpoint->pid);
        return true;
    }
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        const OdDockerMapping *mapping = &snapshot->docker_mappings[index];
        if (mapping->host_port != port) continue;
        (void)snprintf(detail, capacity, "Docker %s (%s)", mapping->container,
                       mapping->project[0] == '\0' ? "unlabelled" : mapping->project);
        return true;
    }
    return false;
}

static const OdAllocation *find_allocation(const OdAllocationPlan *plan, const char *service_id) {
    if (plan == NULL) return NULL;
    for (size_t index = 0U; index < plan->count; ++index) {
        if (strcmp(plan->items[index].service_id, service_id) == 0) return &plan->items[index];
    }
    return NULL;
}

const char *od_service_status_name(OdServiceStatus status) {
    static const char *const names[] = {
        "AVAILABLE", "IN USE", "REASSIGNED", "SAVED", "STALE"
    };
    return status <= OD_SERVICE_STALE ? names[status] : "UNKNOWN";
}

static bool contains_case_insensitive(const char *text, const char *query) {
    if (query[0] == '\0') return true;
    size_t query_length = strlen(query);
    for (size_t start = 0U; text[start] != '\0'; ++start) {
        size_t matched = 0U;
        while (matched < query_length && text[start + matched] != '\0' &&
               tolower((unsigned char)text[start + matched]) ==
               tolower((unsigned char)query[matched])) {
            ++matched;
        }
        if (matched == query_length) return true;
    }
    return false;
}

static bool row_matches(const OdServiceRow *row, const char *query) {
    return contains_case_insensitive(row->service, query) ||
           contains_case_insensitive(row->group, query) ||
           contains_case_insensitive(row->variable, query) ||
           contains_case_insensitive(od_service_status_name(row->status), query);
}

static int compare_rows(const OdDashboard *dashboard, size_t left_index, size_t right_index) {
    const OdServiceRow *left = &dashboard->services[left_index];
    const OdServiceRow *right = &dashboard->services[right_index];
    int comparison = 0;
    switch (dashboard->sort) {
        case OD_SERVICE_SORT_NAME:
            comparison = strcmp(left->service, right->service);
            break;
        case OD_SERVICE_SORT_GROUP:
            comparison = strcmp(left->group, right->group);
            break;
        case OD_SERVICE_SORT_PREFERRED:
            comparison = left->preferred_port == right->preferred_port ? 0 :
                         (left->preferred_port < right->preferred_port ? -1 : 1);
            break;
        case OD_SERVICE_SORT_SELECTED:
            comparison = left->selected_port == right->selected_port ? 0 :
                         (left->selected_port < right->selected_port ? -1 : 1);
            break;
        case OD_SERVICE_SORT_STATUS:
            comparison = left->status == right->status ? 0 : (left->status < right->status ? -1 : 1);
            break;
    }
    if (comparison == 0) comparison = strcmp(left->stable_id, right->stable_id);
    return dashboard->sort_ascending ? comparison : -comparison;
}

static void merge_order(OdDashboard *dashboard,
                        size_t *temporary,
                        size_t begin,
                        size_t middle,
                        size_t end) {
    size_t left = begin;
    size_t right = middle;
    size_t output = begin;
    while (left < middle && right < end) {
        if (compare_rows(dashboard, dashboard->visible_order[left],
                         dashboard->visible_order[right]) <= 0) {
            temporary[output++] = dashboard->visible_order[left++];
        } else {
            temporary[output++] = dashboard->visible_order[right++];
        }
    }
    while (left < middle) temporary[output++] = dashboard->visible_order[left++];
    while (right < end) temporary[output++] = dashboard->visible_order[right++];
    for (size_t index = begin; index < end; ++index) {
        dashboard->visible_order[index] = temporary[index];
    }
}

static OdStatus sort_visible(OdDashboard *dashboard, OdError *error) {
    if (dashboard->visible_count < 2U) return OD_OK;
    size_t *temporary = malloc(dashboard->visible_count * sizeof(*temporary));
    if (temporary == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to sort dashboard services");
        return OD_ERROR_MEMORY;
    }
    for (size_t width = 1U; width < dashboard->visible_count; width *= 2U) {
        for (size_t begin = 0U; begin < dashboard->visible_count; begin += 2U * width) {
            size_t middle = begin + width;
            size_t end = begin + 2U * width;
            if (middle > dashboard->visible_count) middle = dashboard->visible_count;
            if (end > dashboard->visible_count) end = dashboard->visible_count;
            if (middle < end) merge_order(dashboard, temporary, begin, middle, end);
        }
        if (width > dashboard->visible_count / 2U) break;
    }
    free(temporary);
    return OD_OK;
}

static void ensure_visible_page(OdDashboard *dashboard) {
    if (dashboard->visible_count == 0U) {
        dashboard->selected_visible = 0U;
        dashboard->page_start = 0U;
        dashboard->selected_id[0] = '\0';
        return;
    }
    if (dashboard->selected_visible >= dashboard->visible_count) {
        dashboard->selected_visible = dashboard->visible_count - 1U;
    }
    size_t page_size = dashboard->page_size == 0U ? 1U : dashboard->page_size;
    if (dashboard->selected_visible < dashboard->page_start) {
        dashboard->page_start = (dashboard->selected_visible / page_size) * page_size;
    } else if (dashboard->selected_visible >= dashboard->page_start + page_size) {
        dashboard->page_start = (dashboard->selected_visible / page_size) * page_size;
    }
    (void)snprintf(dashboard->selected_id, sizeof(dashboard->selected_id), "%s",
                   dashboard->services[dashboard->visible_order[dashboard->selected_visible]].stable_id);
}

static OdStatus rebuild_visible(OdDashboard *dashboard, OdError *error) {
    char previous[64];
    (void)snprintf(previous, sizeof(previous), "%s", dashboard->selected_id);
    dashboard->visible_count = 0U;
    for (size_t index = 0U; index < dashboard->service_count; ++index) {
        if (row_matches(&dashboard->services[index], dashboard->search)) {
            dashboard->visible_order[dashboard->visible_count++] = index;
        }
    }
    OdStatus status = sort_visible(dashboard, error);
    if (status != OD_OK) return status;
    dashboard->selected_visible = 0U;
    if (previous[0] != '\0') {
        for (size_t index = 0U; index < dashboard->visible_count; ++index) {
            if (strcmp(dashboard->services[dashboard->visible_order[index]].stable_id,
                       previous) == 0) {
                dashboard->selected_visible = index;
                break;
            }
        }
    }
    ensure_visible_page(dashboard);
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_dashboard_init(OdDashboard *dashboard,
                           const OdProfile *profile,
                           const OdScanSnapshot *snapshot,
                           const OdAllocationPlan *plan,
                           OdError *error) {
    if (dashboard == NULL || profile == NULL || snapshot == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "dashboard inputs are required");
        return OD_ERROR_INVALID;
    }
    *dashboard = (OdDashboard){0};
    (void)snprintf(dashboard->project_name, sizeof(dashboard->project_name), "%s",
                   profile->project_name == NULL ? "Project" : profile->project_name);
    dashboard->service_count = profile->service_count;
    dashboard->snapshot = snapshot;
    dashboard->page_size = 8U;
    dashboard->sort = OD_SERVICE_SORT_NAME;
    dashboard->sort_ascending = true;
    dashboard->focused = OD_WIDGET_SERVICES;
    if (profile->service_count > 0U) {
        dashboard->services = calloc(profile->service_count, sizeof(*dashboard->services));
        dashboard->visible_order = calloc(profile->service_count, sizeof(*dashboard->visible_order));
        if (dashboard->services == NULL || dashboard->visible_order == NULL) {
            od_dashboard_free(dashboard);
            od_error_set(error, OD_ERROR_MEMORY, "unable to allocate dashboard services");
            return OD_ERROR_MEMORY;
        }
    }
    dashboard->summary.managed = profile->service_count;
    dashboard->summary.listeners = snapshot->endpoint_count;
    dashboard->summary.docker_mappings = snapshot->docker_mapping_count;
    for (size_t index = 0U; index < profile->service_count; ++index) {
        const OdService *service = &profile->services[index];
        OdServiceRow *row = &dashboard->services[index];
        (void)snprintf(row->stable_id, sizeof(row->stable_id), "%s", service->id);
        (void)snprintf(row->service, sizeof(row->service), "%s", service->name);
        (void)snprintf(row->group, sizeof(row->group), "%s", service->group);
        (void)snprintf(row->variable, sizeof(row->variable), "%s", service->variable);
        row->preferred_port = service->preferred_port;
        const OdAllocation *allocation = find_allocation(plan, service->id);
        row->selected_port = allocation == NULL ?
                             (service->selected_port == 0U ? service->preferred_port : service->selected_port) :
                             allocation->new_port;
        row->status = OD_SERVICE_AVAILABLE;
        if (allocation != NULL && allocation->reason == OD_ALLOC_REASSIGNED) {
            row->status = OD_SERVICE_REASSIGNED;
            ++dashboard->summary.reassigned;
        } else if (allocation != NULL && allocation->reason == OD_ALLOC_PRESERVED) {
            row->status = OD_SERVICE_SAVED;
        }
        row->conflict = port_occupied(snapshot, row->preferred_port,
                                      row->conflict_detail, sizeof(row->conflict_detail));
        if (row->conflict) ++dashboard->summary.conflicts;
        char selected_detail[192];
        if (port_occupied(snapshot, row->selected_port, selected_detail,
                          sizeof(selected_detail)) && row->selected_port != row->preferred_port) {
            row->status = OD_SERVICE_IN_USE;
            (void)snprintf(row->conflict_detail, sizeof(row->conflict_detail), "%s", selected_detail);
            if (!row->conflict) ++dashboard->summary.conflicts;
            row->conflict = true;
        }
        if (row->status == OD_SERVICE_AVAILABLE) ++dashboard->summary.available;
    }
    OdStatus status = rebuild_visible(dashboard, error);
    if (status != OD_OK) od_dashboard_free(dashboard);
    return status;
}

void od_dashboard_free(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    free(dashboard->services);
    free(dashboard->visible_order);
    *dashboard = (OdDashboard){0};
}

OdStatus od_dashboard_search(OdDashboard *dashboard, const char *query, OdError *error) {
    if (dashboard == NULL || query == NULL || strlen(query) >= sizeof(dashboard->search)) {
        od_error_set(error, OD_ERROR_INVALID, "dashboard search is too long");
        return OD_ERROR_INVALID;
    }
    (void)strcpy(dashboard->search, query);
    dashboard->page_start = 0U;
    return rebuild_visible(dashboard, error);
}

void od_dashboard_sort(OdDashboard *dashboard, OdServiceSort sort) {
    if (dashboard == NULL) return;
    if (dashboard->sort == sort) {
        dashboard->sort_ascending = !dashboard->sort_ascending;
    } else {
        dashboard->sort = sort;
        dashboard->sort_ascending = true;
    }
    OdError error;
    (void)rebuild_visible(dashboard, &error);
}

void od_dashboard_move(OdDashboard *dashboard, int rows) {
    if (dashboard == NULL || dashboard->visible_count == 0U) return;
    long long target = (long long)dashboard->selected_visible + rows;
    if (target < 0LL) target = 0LL;
    if ((unsigned long long)target >= dashboard->visible_count) {
        target = (long long)(dashboard->visible_count - 1U);
    }
    dashboard->selected_visible = (size_t)target;
    ensure_visible_page(dashboard);
}

void od_dashboard_move_page(OdDashboard *dashboard, int pages) {
    if (dashboard == NULL) return;
    long long rows = (long long)pages * (long long)(dashboard->page_size == 0U ? 1U : dashboard->page_size);
    if (rows > INT_MAX) rows = INT_MAX;
    if (rows < INT_MIN) rows = INT_MIN;
    od_dashboard_move(dashboard, (int)rows);
}

void od_dashboard_home(OdDashboard *dashboard) {
    if (dashboard == NULL || dashboard->visible_count == 0U) return;
    dashboard->selected_visible = 0U;
    ensure_visible_page(dashboard);
}

void od_dashboard_end(OdDashboard *dashboard) {
    if (dashboard == NULL || dashboard->visible_count == 0U) return;
    dashboard->selected_visible = dashboard->visible_count - 1U;
    ensure_visible_page(dashboard);
}

void od_dashboard_set_page_size(OdDashboard *dashboard, size_t rows) {
    if (dashboard == NULL) return;
    dashboard->page_size = rows == 0U ? 1U : rows;
    ensure_visible_page(dashboard);
}

const OdServiceRow *od_dashboard_selected_service(const OdDashboard *dashboard) {
    if (dashboard == NULL || dashboard->visible_count == 0U ||
        dashboard->selected_visible >= dashboard->visible_count) return NULL;
    return &dashboard->services[dashboard->visible_order[dashboard->selected_visible]];
}

void od_dashboard_focus_next(OdDashboard *dashboard, int direction) {
    if (dashboard == NULL) return;
    int focused = (int)dashboard->focused + (direction < 0 ? -1 : 1);
    if (focused < 0) focused = (int)OD_WIDGET_COUNT - 1;
    if (focused >= (int)OD_WIDGET_COUNT) focused = 0;
    dashboard->focused = (OdDashboardWidget)focused;
    dashboard->expanded = false;
}

void od_dashboard_toggle_expand(OdDashboard *dashboard) {
    if (dashboard != NULL) dashboard->expanded = !dashboard->expanded;
}

static size_t conflict_count(const OdDashboard *dashboard) {
    size_t count = 0U;
    for (size_t index = 0U; index < dashboard->service_count; ++index) {
        if (dashboard->services[index].conflict) ++count;
    }
    return count;
}

static size_t widget_row_count(const OdDashboard *dashboard, OdDashboardWidget widget) {
    switch (widget) {
        case OD_WIDGET_SERVICES: return dashboard->visible_count;
        case OD_WIDGET_CONFLICTS: return conflict_count(dashboard);
        case OD_WIDGET_LISTENERS: return dashboard->snapshot->endpoint_count;
        case OD_WIDGET_DOCKER: return dashboard->snapshot->docker_mapping_count;
        case OD_WIDGET_COUNT: return 0U;
    }
    return 0U;
}

static void widget_state(OdDashboard *dashboard,
                         OdDashboardWidget widget,
                         size_t **selected,
                         size_t **page_start,
                         size_t **page_size) {
    *selected = NULL;
    *page_start = NULL;
    *page_size = NULL;
    switch (widget) {
        case OD_WIDGET_CONFLICTS:
            *selected = &dashboard->conflict_selected;
            *page_start = &dashboard->conflict_page_start;
            *page_size = &dashboard->conflict_page_size;
            break;
        case OD_WIDGET_LISTENERS:
            *selected = &dashboard->listener_selected;
            *page_start = &dashboard->listener_page_start;
            *page_size = &dashboard->listener_page_size;
            break;
        case OD_WIDGET_DOCKER:
            *selected = &dashboard->docker_selected;
            *page_start = &dashboard->docker_page_start;
            *page_size = &dashboard->docker_page_size;
            break;
        case OD_WIDGET_SERVICES:
        case OD_WIDGET_COUNT:
            break;
    }
}

static void ensure_widget_page(OdDashboard *dashboard, OdDashboardWidget widget) {
    if (widget == OD_WIDGET_SERVICES) {
        ensure_visible_page(dashboard);
        return;
    }
    size_t *selected;
    size_t *page_start;
    size_t *page_size;
    widget_state(dashboard, widget, &selected, &page_start, &page_size);
    if (selected == NULL) return;
    size_t count = widget_row_count(dashboard, widget);
    size_t rows = *page_size == 0U ? 1U : *page_size;
    if (count == 0U) {
        *selected = 0U;
        *page_start = 0U;
        return;
    }
    if (*selected >= count) *selected = count - 1U;
    if (*selected < *page_start || *selected >= *page_start + rows) {
        *page_start = (*selected / rows) * rows;
    }
}

void od_dashboard_set_widget_page_size(OdDashboard *dashboard,
                                       OdDashboardWidget widget,
                                       size_t rows) {
    if (dashboard == NULL) return;
    if (widget == OD_WIDGET_SERVICES) {
        od_dashboard_set_page_size(dashboard, rows);
        return;
    }
    size_t *selected;
    size_t *page_start;
    size_t *page_size;
    widget_state(dashboard, widget, &selected, &page_start, &page_size);
    if (page_size == NULL) return;
    *page_size = rows == 0U ? 1U : rows;
    ensure_widget_page(dashboard, widget);
}

void od_dashboard_move_focused(OdDashboard *dashboard, int rows) {
    if (dashboard == NULL) return;
    if (dashboard->focused == OD_WIDGET_SERVICES) {
        od_dashboard_move(dashboard, rows);
        return;
    }
    size_t *selected;
    size_t *page_start;
    size_t *page_size;
    widget_state(dashboard, dashboard->focused, &selected, &page_start, &page_size);
    (void)page_start;
    (void)page_size;
    size_t count = widget_row_count(dashboard, dashboard->focused);
    if (selected == NULL || count == 0U) return;
    long long target = (long long)*selected + (long long)rows;
    if (target < 0LL) target = 0LL;
    if ((unsigned long long)target >= count) target = (long long)(count - 1U);
    *selected = (size_t)target;
    ensure_widget_page(dashboard, dashboard->focused);
}

void od_dashboard_move_focused_page(OdDashboard *dashboard, int pages) {
    if (dashboard == NULL) return;
    size_t page_size = dashboard->page_size;
    if (dashboard->focused != OD_WIDGET_SERVICES) {
        size_t *selected;
        size_t *page_start;
        size_t *widget_page_size;
        widget_state(dashboard, dashboard->focused, &selected, &page_start,
                     &widget_page_size);
        (void)selected;
        (void)page_start;
        if (widget_page_size != NULL) page_size = *widget_page_size;
    }
    if (page_size == 0U) page_size = 1U;
    long long rows = (long long)pages * (long long)page_size;
    if (rows > INT_MAX) rows = INT_MAX;
    if (rows < INT_MIN) rows = INT_MIN;
    od_dashboard_move_focused(dashboard, (int)rows);
}

void od_dashboard_home_focused(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    if (dashboard->focused == OD_WIDGET_SERVICES) {
        od_dashboard_home(dashboard);
        return;
    }
    size_t *selected;
    size_t *page_start;
    size_t *page_size;
    widget_state(dashboard, dashboard->focused, &selected, &page_start, &page_size);
    (void)page_size;
    if (selected != NULL) {
        *selected = 0U;
        *page_start = 0U;
    }
}

void od_dashboard_end_focused(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    if (dashboard->focused == OD_WIDGET_SERVICES) {
        od_dashboard_end(dashboard);
        return;
    }
    size_t *selected;
    size_t *page_start;
    size_t *page_size;
    widget_state(dashboard, dashboard->focused, &selected, &page_start, &page_size);
    (void)page_start;
    (void)page_size;
    size_t count = widget_row_count(dashboard, dashboard->focused);
    if (selected != NULL && count > 0U) {
        *selected = count - 1U;
        ensure_widget_page(dashboard, dashboard->focused);
    }
}

void od_hitmap_init(OdHitMap *map) {
    if (map != NULL) *map = (OdHitMap){0};
}

void od_hitmap_free(OdHitMap *map) {
    if (map == NULL) return;
    free(map->items);
    *map = (OdHitMap){0};
}

const OdHitRegion *od_hitmap_at(const OdHitMap *map, int x, int y) {
    if (map == NULL) return NULL;
    for (size_t index = map->count; index > 0U; --index) {
        const OdHitRegion *region = &map->items[index - 1U];
        if (x >= region->x && y >= region->y &&
            x < region->x + region->width && y < region->y + region->height) {
            return region;
        }
    }
    return NULL;
}
