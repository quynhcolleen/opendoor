#include "opendoor/allocation.h"
#include "opendoor/dashboard.h"
#include "opendoor/model.h"
#include "opendoor/screens.h"
#include "opendoor/scan.h"
#include "opendoor/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static char *duplicate(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static void add_service(OdProfile *profile, size_t index, OdError *error) {
    char id[64];
    char name[64];
    char variable[64];
    (void)snprintf(id, sizeof(id), "service-%02zu", index);
    (void)snprintf(name, sizeof(name), "Service %02zu", index);
    (void)snprintf(variable, sizeof(variable), "SERVICE_%02zu_PORT", index);
    OdService service = {0};
    service.id = id;
    service.name = name;
    service.group = index % 2U == 0U ? "backend" : "frontend";
    service.variable = variable;
    service.preferred_port = (uint16_t)(3000U + index);
    service.protocols = OD_PROTOCOL_TCP;
    service.managed = true;
    CHECK(od_profile_add_service(profile, &service, error) == OD_OK);
}

static void make_dashboard_inputs(OdProfile *profile,
                                  OdScanSnapshot *snapshot,
                                  OdAllocationPlan *plan) {
    OdError error;
    od_profile_init(profile);
    profile->project_name = duplicate("Dashboard demo");
    profile->assignment_file = duplicate(".ports.env");
    profile->port_min = 1024U;
    profile->port_max = 65535U;
    for (size_t index = 0U; index < 18U; ++index) add_service(profile, index, &error);
    od_scan_snapshot_init(snapshot, 9U);
    snapshot->endpoints = calloc(3U, sizeof(*snapshot->endpoints));
    snapshot->endpoint_count = 3U;
    snapshot->endpoints[0].local_port = 3000U;
    snapshot->endpoints[0].protocol = OD_PROTOCOL_TCP;
    (void)strcpy(snapshot->endpoints[0].local_address, "0.0.0.0");
    (void)strcpy(snapshot->endpoints[0].process, "foreign-api");
    snapshot->endpoints[0].pid = 111;
    snapshot->endpoints[1].local_port = 4000U;
    snapshot->endpoints[1].protocol = OD_PROTOCOL_TCP;
    (void)strcpy(snapshot->endpoints[1].local_address, "127.0.0.1");
    snapshot->endpoints[2].local_port = 5000U;
    snapshot->endpoints[2].protocol = OD_PROTOCOL_UDP;
    (void)strcpy(snapshot->endpoints[2].local_address, "::");
    snapshot->docker_mappings = calloc(1U, sizeof(*snapshot->docker_mappings));
    snapshot->docker_mapping_count = 1U;
    (void)strcpy(snapshot->docker_mappings[0].container, "demo-db");
    (void)strcpy(snapshot->docker_mappings[0].project, "demo");
    snapshot->docker_mappings[0].host_port = 5432U;
    snapshot->docker_mappings[0].container_port = 5432U;
    snapshot->docker_mappings[0].protocol = OD_PROTOCOL_TCP;

    plan->items = calloc(profile->service_count, sizeof(*plan->items));
    plan->count = profile->service_count;
    for (size_t index = 0U; index < plan->count; ++index) {
        plan->items[index].service_id = duplicate(profile->services[index].id);
        plan->items[index].variable = duplicate(profile->services[index].variable);
        plan->items[index].new_port = index == 0U ? 3100U : profile->services[index].preferred_port;
        plan->items[index].reason = index == 0U ? OD_ALLOC_REASSIGNED : OD_ALLOC_PREFERRED;
    }
}

static void test_selection_survives_sort_search_and_pages(void) {
    OdProfile profile;
    OdScanSnapshot snapshot;
    OdAllocationPlan plan;
    make_dashboard_inputs(&profile, &snapshot, &plan);
    OdDashboard dashboard;
    OdError error;
    OdStatus status = od_dashboard_init(&dashboard, &profile, &snapshot, &plan, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        od_allocation_plan_free(&plan);
        od_scan_snapshot_free(&snapshot);
        od_profile_free(&profile);
        return;
    }
    CHECK(dashboard.summary.managed == 18U);
    CHECK(dashboard.summary.reassigned == 1U);
    CHECK(dashboard.summary.listeners == 3U);
    CHECK(dashboard.summary.docker_mappings == 1U);
    od_dashboard_set_page_size(&dashboard, 5U);
    od_dashboard_move(&dashboard, 7);
    const OdServiceRow *selected = od_dashboard_selected_service(&dashboard);
    CHECK(selected != NULL);
    char selected_id[64];
    (void)strcpy(selected_id, selected->stable_id);
    CHECK(dashboard.page_start == 5U);
    od_dashboard_sort(&dashboard, OD_SERVICE_SORT_SELECTED);
    selected = od_dashboard_selected_service(&dashboard);
    CHECK(selected != NULL && strcmp(selected->stable_id, selected_id) == 0);
    CHECK(od_dashboard_search(&dashboard, "Service 07", &error) == OD_OK);
    CHECK(dashboard.visible_count == 1U);
    selected = od_dashboard_selected_service(&dashboard);
    CHECK(selected != NULL && strcmp(selected->stable_id, selected_id) == 0);
    CHECK(od_dashboard_search(&dashboard, "no match", &error) == OD_OK);
    CHECK(dashboard.visible_count == 0U);
    CHECK(od_dashboard_selected_service(&dashboard) == NULL);
    CHECK(od_dashboard_search(&dashboard, "", &error) == OD_OK);
    CHECK(dashboard.visible_count == 18U);
    od_dashboard_end(&dashboard);
    CHECK(dashboard.selected_visible == 17U);
    CHECK(dashboard.page_start == 15U);
    od_dashboard_home(&dashboard);
    CHECK(dashboard.selected_visible == 0U);
    CHECK(dashboard.page_start == 0U);

    dashboard.focused = OD_WIDGET_LISTENERS;
    od_dashboard_set_widget_page_size(&dashboard, OD_WIDGET_LISTENERS, 2U);
    od_dashboard_move_focused(&dashboard, 2);
    CHECK(dashboard.listener_selected == 2U);
    CHECK(dashboard.listener_page_start == 2U);
    od_dashboard_move_focused_page(&dashboard, -1);
    CHECK(dashboard.listener_selected == 0U);
    od_dashboard_end_focused(&dashboard);
    CHECK(dashboard.listener_selected == 2U);
    od_dashboard_home_focused(&dashboard);
    CHECK(dashboard.listener_selected == 0U);
    od_dashboard_free(&dashboard);
    od_allocation_plan_free(&plan);
    od_scan_snapshot_free(&snapshot);
    od_profile_free(&profile);
}

static size_t count_newlines(const char *text) {
    size_t count = 0U;
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '\n') ++count;
    }
    return count;
}

static void render_and_check(OdDashboard *dashboard,
                             size_t width,
                             size_t height,
                             const char *required,
                             const char *status) {
    OdCanvas canvas;
    OdHitMap hit_map;
    OdError error;
    od_hitmap_init(&hit_map);
    CHECK(od_canvas_init(&canvas, width, height, &error) == OD_OK);
    od_render_dashboard(&canvas, dashboard, true, &hit_map, status);
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(count_newlines(text) == height);
        CHECK(strstr(text, required) != NULL);
        CHECK(strstr(text, status) != NULL);
        CHECK(strstr(text, "PgUp/PgDn Page") != NULL);
        free(text);
    }
    CHECK(hit_map.count > 0U);
    if (hit_map.count > 0U) {
        const OdHitRegion *hit = od_hitmap_at(&hit_map, hit_map.items[0].x, hit_map.items[0].y);
        CHECK(hit == &hit_map.items[0]);
    }
    od_hitmap_free(&hit_map);
    od_canvas_free(&canvas);
}

static void test_responsive_dashboard_snapshots_and_focus(void) {
    OdProfile profile;
    OdScanSnapshot snapshot;
    OdAllocationPlan plan;
    make_dashboard_inputs(&profile, &snapshot, &plan);
    OdDashboard dashboard;
    OdError error;
    OdStatus status = od_dashboard_init(&dashboard, &profile, &snapshot, &plan, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        od_allocation_plan_free(&plan);
        od_scan_snapshot_free(&snapshot);
        od_profile_free(&profile);
        return;
    }
    render_and_check(&dashboard, 132U, 32U, "Host listeners", "Live scan ready");
    render_and_check(&dashboard, 100U, 26U, "Services", "Medium layout");
    dashboard.focused = OD_WIDGET_DOCKER;
    render_and_check(&dashboard, 70U, 22U, "Docker mappings", "Compact layout");
    od_dashboard_toggle_expand(&dashboard);
    CHECK(dashboard.expanded);
    render_and_check(&dashboard, 100U, 24U, "Docker mappings", "Expanded widget");
    od_dashboard_focus_next(&dashboard, 1);
    CHECK(dashboard.focused == OD_WIDGET_SERVICES);
    CHECK(!dashboard.expanded);
    od_dashboard_free(&dashboard);
    od_allocation_plan_free(&plan);
    od_scan_snapshot_free(&snapshot);
    od_profile_free(&profile);
}

int main(void) {
    test_selection_survives_sort_search_and_pages();
    test_responsive_dashboard_snapshots_and_focus();
    if (failures != 0) {
        fprintf(stderr, "%d dashboard checks failed\n", failures);
        return 1;
    }
    puts("dashboard checks passed");
    return 0;
}
