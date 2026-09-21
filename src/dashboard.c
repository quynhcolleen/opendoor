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
        (void)snprintf(detail, capacity, "Docker %.80s (%.80s)", mapping->container,
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
    if (text == NULL) return false;
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

static bool secondary_row_matches(const OdDashboard *dashboard,
                                  OdDashboardWidget widget,
                                  size_t raw_index,
                                  const char *query) {
    if (query[0] == '\0') return true;
    char numeric[96];
    if (widget == OD_WIDGET_CONFLICTS) {
        const OdServiceRow *row = &dashboard->services[raw_index];
        (void)snprintf(numeric, sizeof(numeric), "%u %u",
                       (unsigned)row->preferred_port, (unsigned)row->selected_port);
        return contains_case_insensitive(row->service, query) ||
               contains_case_insensitive(row->variable, query) ||
               contains_case_insensitive(row->conflict_detail, query) ||
               contains_case_insensitive(numeric, query);
    }
    if (widget == OD_WIDGET_LISTENERS) {
        const OdEndpoint *endpoint = &dashboard->snapshot->endpoints[raw_index];
        (void)snprintf(numeric, sizeof(numeric), "%u %ld %s",
                       (unsigned)endpoint->local_port, (long)endpoint->pid,
                       endpoint->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp");
        return contains_case_insensitive(endpoint->local_address, query) ||
               contains_case_insensitive(endpoint->process, query) ||
               contains_case_insensitive(endpoint->user, query) ||
               contains_case_insensitive(endpoint->command, query) ||
               contains_case_insensitive(numeric, query);
    }
    const OdDockerMapping *mapping = &dashboard->snapshot->docker_mappings[raw_index];
    (void)snprintf(numeric, sizeof(numeric), "%u %u %s",
                   (unsigned)mapping->host_port, (unsigned)mapping->container_port,
                   mapping->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp");
    return contains_case_insensitive(mapping->container, query) ||
           contains_case_insensitive(mapping->project, query) ||
           contains_case_insensitive(mapping->service, query) ||
           contains_case_insensitive(mapping->bind_address, query) ||
           contains_case_insensitive(numeric, query);
}

static int compare_secondary(const OdDashboard *dashboard,
                             OdDashboardWidget widget,
                             size_t left_index,
                             size_t right_index) {
    int comparison = 0;
    bool ascending = true;
    if (widget == OD_WIDGET_CONFLICTS) {
        const OdServiceRow *left = &dashboard->services[left_index];
        const OdServiceRow *right = &dashboard->services[right_index];
        comparison = left->preferred_port == right->preferred_port ?
                     strcmp(left->stable_id, right->stable_id) :
                     (left->preferred_port < right->preferred_port ? -1 : 1);
        ascending = dashboard->conflict_sort_ascending;
    } else if (widget == OD_WIDGET_LISTENERS) {
        const OdEndpoint *left = &dashboard->snapshot->endpoints[left_index];
        const OdEndpoint *right = &dashboard->snapshot->endpoints[right_index];
        comparison = left->local_port == right->local_port ?
                     strcmp(left->stable_id, right->stable_id) :
                     (left->local_port < right->local_port ? -1 : 1);
        ascending = dashboard->listener_sort_ascending;
    } else {
        const OdDockerMapping *left = &dashboard->snapshot->docker_mappings[left_index];
        const OdDockerMapping *right = &dashboard->snapshot->docker_mappings[right_index];
        comparison = left->host_port == right->host_port ?
                     strcmp(left->container, right->container) :
                     (left->host_port < right->host_port ? -1 : 1);
        ascending = dashboard->docker_sort_ascending;
    }
    return ascending ? comparison : -comparison;
}

static void sort_secondary(OdDashboard *dashboard,
                           OdDashboardWidget widget,
                           size_t *order,
                           size_t count) {
    if (count < 2U) return;
    size_t *temporary = malloc(count * sizeof(*temporary));
    if (temporary == NULL) return;
    for (size_t width = 1U; width < count; width *= 2U) {
        for (size_t begin = 0U; begin < count; begin += 2U * width) {
            size_t middle = begin + width < count ? begin + width : count;
            size_t end = begin + 2U * width < count ? begin + 2U * width : count;
            size_t left = begin;
            size_t right = middle;
            size_t output = begin;
            while (left < middle && right < end) {
                if (compare_secondary(dashboard, widget, order[left], order[right]) <= 0) {
                    temporary[output++] = order[left++];
                } else {
                    temporary[output++] = order[right++];
                }
            }
            while (left < middle) temporary[output++] = order[left++];
            while (right < end) temporary[output++] = order[right++];
            for (size_t index = begin; index < end; ++index) order[index] = temporary[index];
        }
        if (width > count / 2U) break;
    }
    free(temporary);
}

static size_t selected_secondary_raw(const OdDashboard *dashboard,
                                     OdDashboardWidget widget) {
    if (widget == OD_WIDGET_CONFLICTS &&
        dashboard->conflict_selected < dashboard->conflict_visible_count) {
        return dashboard->conflict_order[dashboard->conflict_selected];
    }
    if (widget == OD_WIDGET_LISTENERS &&
        dashboard->listener_selected < dashboard->listener_visible_count) {
        return dashboard->listener_order[dashboard->listener_selected];
    }
    if (widget == OD_WIDGET_DOCKER &&
        dashboard->docker_selected < dashboard->docker_visible_count) {
        return dashboard->docker_order[dashboard->docker_selected];
    }
    return SIZE_MAX;
}

static void rebuild_secondary(OdDashboard *dashboard, OdDashboardWidget widget) {
    size_t selected_raw = selected_secondary_raw(dashboard, widget);
    size_t *order = NULL;
    size_t *visible_count = NULL;
    size_t *selected = NULL;
    size_t *page_start = NULL;
    const char *query = "";
    size_t raw_count = 0U;
    if (widget == OD_WIDGET_CONFLICTS) {
        order = dashboard->conflict_order;
        visible_count = &dashboard->conflict_visible_count;
        selected = &dashboard->conflict_selected;
        page_start = &dashboard->conflict_page_start;
        query = dashboard->conflict_search;
        raw_count = dashboard->service_count;
    } else if (widget == OD_WIDGET_LISTENERS) {
        order = dashboard->listener_order;
        visible_count = &dashboard->listener_visible_count;
        selected = &dashboard->listener_selected;
        page_start = &dashboard->listener_page_start;
        query = dashboard->listener_search;
        raw_count = dashboard->snapshot->endpoint_count;
    } else if (widget == OD_WIDGET_DOCKER) {
        order = dashboard->docker_order;
        visible_count = &dashboard->docker_visible_count;
        selected = &dashboard->docker_selected;
        page_start = &dashboard->docker_page_start;
        query = dashboard->docker_search;
        raw_count = dashboard->snapshot->docker_mapping_count;
    } else {
        return;
    }
    *visible_count = 0U;
    for (size_t raw = 0U; raw < raw_count; ++raw) {
        if (widget == OD_WIDGET_CONFLICTS && !dashboard->services[raw].conflict) continue;
        if (secondary_row_matches(dashboard, widget, raw, query)) {
            order[(*visible_count)++] = raw;
        }
    }
    sort_secondary(dashboard, widget, order, *visible_count);
    *selected = 0U;
    if (selected_raw != SIZE_MAX) {
        for (size_t index = 0U; index < *visible_count; ++index) {
            if (order[index] == selected_raw) { *selected = index; break; }
        }
    }
    *page_start = 0U;
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
        dashboard->selected_id = NULL;
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
    dashboard->selected_id =
        dashboard->services[dashboard->visible_order[dashboard->selected_visible]].stable_id;
}

static OdStatus rebuild_visible(OdDashboard *dashboard, OdError *error) {
    const char *previous = dashboard->selected_id;
    dashboard->visible_count = 0U;
    for (size_t index = 0U; index < dashboard->service_count; ++index) {
        if (row_matches(&dashboard->services[index], dashboard->search)) {
            dashboard->visible_order[dashboard->visible_count++] = index;
        }
    }
    OdStatus status = sort_visible(dashboard, error);
    if (status != OD_OK) return status;
    dashboard->selected_visible = 0U;
    if (previous != NULL && previous[0] != '\0') {
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
    dashboard->profile_saved = true;
    dashboard->refresh_seconds = 5U;
    dashboard->focused = OD_WIDGET_SERVICES;
    dashboard->conflict_sort_ascending = true;
    dashboard->listener_sort_ascending = true;
    dashboard->docker_sort_ascending = true;
    if (profile->service_count > 0U) {
        dashboard->services = calloc(profile->service_count, sizeof(*dashboard->services));
        dashboard->visible_order = calloc(profile->service_count, sizeof(*dashboard->visible_order));
        if (dashboard->services == NULL || dashboard->visible_order == NULL) {
            od_dashboard_free(dashboard);
            od_error_set(error, OD_ERROR_MEMORY, "unable to allocate dashboard services");
            return OD_ERROR_MEMORY;
        }
    }
    if (profile->service_count > 0U) {
        dashboard->conflict_order = calloc(profile->service_count,
                                            sizeof(*dashboard->conflict_order));
    }
    if (snapshot->endpoint_count > 0U) {
        dashboard->listener_order = calloc(snapshot->endpoint_count,
                                            sizeof(*dashboard->listener_order));
    }
    if (snapshot->docker_mapping_count > 0U) {
        dashboard->docker_order = calloc(snapshot->docker_mapping_count,
                                          sizeof(*dashboard->docker_order));
    }
    if ((profile->service_count > 0U && dashboard->conflict_order == NULL) ||
        (snapshot->endpoint_count > 0U && dashboard->listener_order == NULL) ||
        (snapshot->docker_mapping_count > 0U && dashboard->docker_order == NULL)) {
        od_dashboard_free(dashboard);
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate dashboard table indexes");
        return OD_ERROR_MEMORY;
    }
    dashboard->summary.managed = profile->service_count;
    dashboard->summary.listeners = snapshot->endpoint_count;
    dashboard->summary.docker_mappings = snapshot->docker_mapping_count;
    for (size_t index = 0U; index < profile->service_count; ++index) {
        const OdService *service = &profile->services[index];
        OdServiceRow *row = &dashboard->services[index];
        row->stable_id = service->id;
        row->service = service->name;
        row->group = service->group;
        row->variable = service->variable;
        row->preferred_port = service->preferred_port;
        const OdAllocation *allocation = find_allocation(plan, service->id);
        row->selected_port = allocation == NULL ?
                             (service->selected_port == 0U ? service->preferred_port : service->selected_port) :
                             (allocation->old_port == 0U ? allocation->new_port : allocation->old_port);
        row->status = OD_SERVICE_AVAILABLE;
        if (allocation != NULL && allocation->reason == OD_ALLOC_REASSIGNED) {
            row->status = allocation->old_port == 0U ?
                          OD_SERVICE_REASSIGNED : OD_SERVICE_STALE;
            ++dashboard->summary.reassigned;
        } else if (allocation != NULL && allocation->reason == OD_ALLOC_PRESERVED) {
            row->status = OD_SERVICE_SAVED;
        }
        row->conflict = port_occupied(snapshot, row->preferred_port,
                                      row->conflict_detail, sizeof(row->conflict_detail));
        if (row->conflict) ++dashboard->summary.conflicts;
        char selected_detail[192];
        if (port_occupied(snapshot, row->selected_port, selected_detail,
                          sizeof(selected_detail))) {
            row->status = OD_SERVICE_IN_USE;
            (void)snprintf(row->conflict_detail, sizeof(row->conflict_detail), "%s", selected_detail);
            if (!row->conflict) ++dashboard->summary.conflicts;
            row->conflict = true;
        }
        if (row->status == OD_SERVICE_AVAILABLE) ++dashboard->summary.available;
    }
    rebuild_secondary(dashboard, OD_WIDGET_CONFLICTS);
    rebuild_secondary(dashboard, OD_WIDGET_LISTENERS);
    rebuild_secondary(dashboard, OD_WIDGET_DOCKER);
    OdStatus status = rebuild_visible(dashboard, error);
    if (status != OD_OK) od_dashboard_free(dashboard);
    return status;
}

void od_dashboard_free(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    free(dashboard->services);
    free(dashboard->visible_order);
    free(dashboard->conflict_order);
    free(dashboard->listener_order);
    free(dashboard->docker_order);
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

OdStatus od_dashboard_search_focused(OdDashboard *dashboard,
                                     const char *query,
                                     OdError *error) {
    if (dashboard == NULL || query == NULL || strlen(query) >= 128U) {
        od_error_set(error, OD_ERROR_INVALID, "dashboard search is too long");
        return OD_ERROR_INVALID;
    }
    if (dashboard->focused == OD_WIDGET_SERVICES) {
        return od_dashboard_search(dashboard, query, error);
    }
    char *destination = dashboard->focused == OD_WIDGET_CONFLICTS ? dashboard->conflict_search :
                        (dashboard->focused == OD_WIDGET_LISTENERS ? dashboard->listener_search :
                                                                    dashboard->docker_search);
    (void)strcpy(destination, query);
    rebuild_secondary(dashboard, dashboard->focused);
    od_error_clear(error);
    return OD_OK;
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

void od_dashboard_sort_focused(OdDashboard *dashboard) {
    if (dashboard == NULL) return;
    if (dashboard->focused == OD_WIDGET_SERVICES) {
        OdServiceSort next = (OdServiceSort)(((unsigned)dashboard->sort + 1U) % 5U);
        od_dashboard_sort(dashboard, next);
        return;
    }
    if (dashboard->focused == OD_WIDGET_CONFLICTS) {
        dashboard->conflict_sort_ascending = !dashboard->conflict_sort_ascending;
    } else if (dashboard->focused == OD_WIDGET_LISTENERS) {
        dashboard->listener_sort_ascending = !dashboard->listener_sort_ascending;
    } else if (dashboard->focused == OD_WIDGET_DOCKER) {
        dashboard->docker_sort_ascending = !dashboard->docker_sort_ascending;
    }
    rebuild_secondary(dashboard, dashboard->focused);
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
    return dashboard->conflict_visible_count;
}

static size_t widget_row_count(const OdDashboard *dashboard, OdDashboardWidget widget) {
    switch (widget) {
        case OD_WIDGET_SERVICES: return dashboard->visible_count;
        case OD_WIDGET_CONFLICTS: return conflict_count(dashboard);
        case OD_WIDGET_LISTENERS: return dashboard->listener_visible_count;
        case OD_WIDGET_DOCKER: return dashboard->docker_visible_count;
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

static bool same_endpoint_identity(const OdEndpoint *left, const OdEndpoint *right) {
    if (left->stable_id[0] != '\0' && right->stable_id[0] != '\0') {
        return strcmp(left->stable_id, right->stable_id) == 0;
    }
    return left->family == right->family && left->protocol == right->protocol &&
           left->local_port == right->local_port && left->inode == right->inode &&
           strcmp(left->local_address, right->local_address) == 0;
}

static bool same_docker_identity(const OdDockerMapping *left,
                                 const OdDockerMapping *right) {
    return left->host_port == right->host_port &&
           left->container_port == right->container_port &&
           left->protocol == right->protocol &&
           strcmp(left->container_id, right->container_id) == 0 &&
           strcmp(left->bind_address, right->bind_address) == 0;
}

void od_dashboard_restore_secondary_selection(OdDashboard *destination,
                                              const OdDashboard *source) {
    if (destination == NULL || source == NULL) return;
    size_t source_conflict_raw = selected_secondary_raw(source, OD_WIDGET_CONFLICTS);
    size_t source_listener_raw = selected_secondary_raw(source, OD_WIDGET_LISTENERS);
    size_t source_docker_raw = selected_secondary_raw(source, OD_WIDGET_DOCKER);
    (void)snprintf(destination->conflict_search, sizeof(destination->conflict_search),
                   "%s", source->conflict_search);
    (void)snprintf(destination->listener_search, sizeof(destination->listener_search),
                   "%s", source->listener_search);
    (void)snprintf(destination->docker_search, sizeof(destination->docker_search),
                   "%s", source->docker_search);
    destination->conflict_sort_ascending = source->conflict_sort_ascending;
    destination->listener_sort_ascending = source->listener_sort_ascending;
    destination->docker_sort_ascending = source->docker_sort_ascending;
    rebuild_secondary(destination, OD_WIDGET_CONFLICTS);
    rebuild_secondary(destination, OD_WIDGET_LISTENERS);
    rebuild_secondary(destination, OD_WIDGET_DOCKER);
    destination->conflict_page_size = source->conflict_page_size;
    destination->listener_page_size = source->listener_page_size;
    destination->docker_page_size = source->docker_page_size;

    if (source_conflict_raw < source->service_count) {
        const char *stable_id = source->services[source_conflict_raw].stable_id;
        for (size_t visible = 0U; visible < destination->conflict_visible_count; ++visible) {
            size_t raw = destination->conflict_order[visible];
            if (strcmp(destination->services[raw].stable_id, stable_id) == 0) {
                destination->conflict_selected = visible;
                break;
            }
        }
    }
    if (source->snapshot != NULL && destination->snapshot != NULL &&
        source_listener_raw < source->snapshot->endpoint_count) {
        const OdEndpoint *selected = &source->snapshot->endpoints[source_listener_raw];
        for (size_t visible = 0U; visible < destination->listener_visible_count; ++visible) {
            size_t raw = destination->listener_order[visible];
            if (same_endpoint_identity(selected, &destination->snapshot->endpoints[raw])) {
                destination->listener_selected = visible;
                break;
            }
        }
    }
    if (source->snapshot != NULL && destination->snapshot != NULL &&
        source_docker_raw < source->snapshot->docker_mapping_count) {
        const OdDockerMapping *selected = &source->snapshot->docker_mappings[source_docker_raw];
        for (size_t visible = 0U; visible < destination->docker_visible_count; ++visible) {
            size_t raw = destination->docker_order[visible];
            if (same_docker_identity(selected, &destination->snapshot->docker_mappings[raw])) {
                destination->docker_selected = visible;
                break;
            }
        }
    }
    ensure_widget_page(destination, OD_WIDGET_CONFLICTS);
    ensure_widget_page(destination, OD_WIDGET_LISTENERS);
    ensure_widget_page(destination, OD_WIDGET_DOCKER);
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
