#include "opendoor/screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const banner[] = {
    "   ____  ____  _______   __   ____  ____  ____  ____",
    "  / __ \\/ __ \\/ ____/ | / /  / __ \\/ __ \\/ __ \\/ __ \\",
    " / / / / /_/ / __/ /  |/ /  / / / / / / / / / / /_/ /",
    "/ /_/ / ____/ /___/ /|  /  / /_/ / /_/ / /_/ / _, _/",
    "\\____/_/   /_____/_/ |_/   \\____/_____/\\____/_/ |_|  OPEN DOOR"
};

const char *const *od_banner_lines(void) {
    return banner;
}

size_t od_banner_line_count(void) {
    return sizeof(banner) / sizeof(banner[0]);
}

static int maximum(int left, int right) {
    return left > right ? left : right;
}

void od_render_resize_required(OdCanvas *canvas) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    int middle = (int)(canvas->height / 2U);
    od_canvas_write_centered(canvas, middle - 1, "Terminal too small",
                             OD_ROLE_WARNING, 1U);
    od_canvas_write_centered(canvas, middle + 1, "Resize to at least 60 x 18",
                             OD_ROLE_MUTED, 0U);
}

void od_render_loading(OdCanvas *canvas,
                       OdLoadingStage stage,
                       unsigned spinner_frame,
                       unsigned elapsed_milliseconds,
                       bool reduced_motion,
                       bool ascii,
                       size_t warning_count) {
    static const char *const labels[] = {
        "Project files",
        "Kernel sockets",
        "Process ownership",
        "Docker",
        "Candidate reconciliation"
    };
    static const char *const ascii_spinner[] = {"|", "/", "-", "\\"};
    static const char *const unicode_spinner[] = {"◐", "◓", "◑", "◒"};
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    int start = maximum(1, (int)(canvas->height - OD_LOAD_STAGE_COUNT - 8U) / 2);
    od_canvas_write_centered(canvas, start, "OPEN DOOR", OD_ROLE_PRIMARY, 1U);
    od_canvas_write_centered(canvas, start + 2, "Discovering ports", OD_ROLE_DEFAULT, 1U);
    int left = maximum(1, (int)(canvas->width / 2U) - 18);
    for (size_t index = 0U; index < OD_LOAD_STAGE_COUNT; ++index) {
        char line[96];
        const char *marker;
        if (index < (size_t)stage) {
            marker = "[done]";
        } else if (index == (size_t)stage) {
            if (reduced_motion) {
                marker = "[....]";
            } else {
                const char *spinner = ascii ? ascii_spinner[spinner_frame % 4U] :
                                               unicode_spinner[spinner_frame % 4U];
                (void)snprintf(line, sizeof(line), "[ %s ] %s", spinner, labels[index]);
                marker = NULL;
            }
        } else {
            marker = "[    ]";
        }
        if (marker != NULL) (void)snprintf(line, sizeof(line), "%s %s", marker, labels[index]);
        OdThemeRole role = index < (size_t)stage ? OD_ROLE_SUCCESS :
                           (index == (size_t)stage ? OD_ROLE_PRIMARY : OD_ROLE_MUTED);
        od_canvas_write(canvas, left, start + 4 + (int)index, line,
                        canvas->width > (size_t)left ? canvas->width - (size_t)left : 0U,
                        role, index == (size_t)stage ? 1U : 0U);
    }
    char footer[96];
    (void)snprintf(footer, sizeof(footer), "Elapsed: %u ms  Warnings: %zu",
                   elapsed_milliseconds, warning_count);
    od_canvas_write_centered(canvas, (int)canvas->height - 2, footer,
                             warning_count == 0U ? OD_ROLE_MUTED : OD_ROLE_WARNING, 0U);
    od_canvas_write_centered(canvas, (int)canvas->height - 1,
                             "Esc Skip animation", OD_ROLE_MUTED, 0U);
}

void od_render_main_menu(OdCanvas *canvas, const OdMenuView *view, bool ascii) {
    static const char *const first_run[] = {
        "Discover this project",
        "Open listener explorer",
        "Settings and appearance",
        "Help",
        "Quit"
    };
    static const char *const configured[] = {
        "Open dashboard",
        "Resolve conflicts",
        "Rescan",
        "Edit project profile",
        "Settings and appearance",
        "Help",
        "Quit"
    };
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    for (size_t index = 0U; index < od_banner_line_count(); ++index) {
        od_canvas_write_centered(canvas, 1 + (int)index, banner[index],
                                 OD_ROLE_PRIMARY, 1U);
    }
    char project[256];
    (void)snprintf(project, sizeof(project), "Project: %s  •  %s",
                   view->project_name == NULL ? "current directory" : view->project_name,
                   view->configured ? "profile configured" : "new project");
    od_canvas_write_centered(canvas, 7, project, OD_ROLE_MUTED, 0U);

    const char *const *items = view->configured ? configured : first_run;
    size_t item_count = view->configured ? sizeof(configured) / sizeof(configured[0]) :
                                          sizeof(first_run) / sizeof(first_run[0]);
    int box_width = 50;
    if ((size_t)box_width > canvas->width - 4U) box_width = (int)canvas->width - 4;
    int box_height = (int)item_count + 4;
    int box_x = ((int)canvas->width - box_width) / 2;
    int box_y = 9;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    for (size_t index = 0U; index < item_count; ++index) {
        char line[96];
        bool selected = index == view->selected_item;
        (void)snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", items[index]);
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)index, line,
                        (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    if (view->status != NULL) {
        od_canvas_write(canvas, 1, (int)canvas->height - 2, view->status,
                        canvas->width - 2U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Navigate  Enter Select  ? Help  q Quit",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_onboarding(OdCanvas *canvas,
                          const OdOnboarding *onboarding,
                          bool ascii,
                          const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Discover this project", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    size_t reviewed_count = 0U;
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        if (onboarding->reviewed[index]) ++reviewed_count;
    }
    char summary[160];
    (void)snprintf(summary, sizeof(summary),
                   "Review every candidate • %zu of %zu reviewed • Space toggles use",
                   reviewed_count, onboarding->candidates.count);
    od_canvas_write(canvas, 2, 2, summary, canvas->width - 4U,
                    reviewed_count == onboarding->candidates.count ? OD_ROLE_SUCCESS : OD_ROLE_MUTED,
                    0U);

    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "USE REVIEW CONFIDENCE   NAME                 VARIABLE                 PORT",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);

    size_t start = od_onboarding_page_start(onboarding);
    size_t end = start + onboarding->page_size;
    if (end > onboarding->candidates.count) end = onboarding->candidates.count;
    for (size_t index = start; index < end; ++index) {
        const OdCandidate *candidate = &onboarding->candidates.items[index];
        const char *confidence = candidate->confidence == OD_CONFIDENCE_CONFIRMED ? "Confirmed" :
                                 (candidate->confidence == OD_CONFIDENCE_LIKELY ? "Likely" : "Possible");
        const char *use = candidate->selected ? (ascii ? "[x]" : "[✓]") : "[ ]";
        const char *review = onboarding->reviewed[index] ? (ascii ? "yes" : "✓") : "—";
        char row[320];
        (void)snprintf(row, sizeof(row), "%-3s %-6s %-12s %-20.20s %-24.24s %5u",
                       use, review, confidence, candidate->name, candidate->variable,
                       (unsigned)candidate->port);
        int row_y = box_y + 2 + (int)(index - start);
        bool selected = index == onboarding->selected;
        od_canvas_write(canvas, box_x + 2, row_y, row, (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    if (onboarding->candidates.count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No candidates found • press a to add a service",
                                 OD_ROLE_MUTED, 0U);
    }
    char page[80];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu",
                   od_onboarding_page(onboarding) + 1U,
                   od_onboarding_page_count(onboarding));
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 3, status,
                        canvas->width - 4U, OD_ROLE_WARNING, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Select  PgUp/PgDn Page  Space Use  Enter Review  e Edit  a Add  s Continue  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_dashboard(OdCanvas *canvas,
                         OdDashboard *dashboard,
                         bool ascii,
                         OdHitMap *hit_map,
                         const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    od_hitmap_free(hit_map);
    od_hitmap_init(hit_map);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    char header[256];
    (void)snprintf(header, sizeof(header), "OPEN DOOR  /  %s  /  scan #%llu%s",
                   dashboard->project_name,
                   (unsigned long long)dashboard->snapshot->generation,
                   dashboard->snapshot->process_permissions_limited ?
                       "  [process details limited]" : "");
    od_canvas_write(canvas, 1, 0, header, canvas->width - 2U, OD_ROLE_PRIMARY, 1U);

    char summary[256];
    (void)snprintf(summary, sizeof(summary),
                   "Managed %zu  Available %zu  Conflicts %zu  Reassigned %zu  Listeners %zu  Docker %zu",
                   dashboard->summary.managed, dashboard->summary.available,
                   dashboard->summary.conflicts, dashboard->summary.reassigned,
                   dashboard->summary.listeners, dashboard->summary.docker_mappings);
    od_canvas_write(canvas, 1, 2, summary, canvas->width - 2U,
                    dashboard->summary.conflicts == 0U ? OD_ROLE_SUCCESS : OD_ROLE_WARNING, 0U);

    const int footer_y = (int)canvas->height - 1;
    const int status_y = footer_y - 1;
    const int content_y = 4;
    const int content_height = status_y - content_y;
    if (status != NULL) {
        od_canvas_write(canvas, 1, status_y, status, canvas->width - 2U,
                        OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, footer_y,
                    "PgUp/PgDn Page  Up/Down Select  Tab Focus  e Expand  / Search  s Sort  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);

    static const char *const widget_names[] = {
        "Services", "Conflicts", "Host listeners", "Docker mappings"
    };

    /* Keep hit-region allocation deliberately local to rendering. A missed mouse
       region after allocation failure never affects the keyboard-complete UI. */
#define ADD_HIT(X, Y, W, H, ACTION, WIDGET, TARGET)                              \
    do {                                                                          \
        OdHitRegion *grown__ = realloc(hit_map->items,                             \
            (hit_map->count + 1U) * sizeof(*hit_map->items));                      \
        if (grown__ != NULL) {                                                     \
            hit_map->items = grown__;                                              \
            hit_map->items[hit_map->count++] = (OdHitRegion){                      \
                (X), (Y), (W), (H), (ACTION), (WIDGET), (TARGET)                   \
            };                                                                     \
        }                                                                          \
    } while (0)

#define DRAW_TITLE(X, Y, W, WIDGET)                                                \
    do {                                                                          \
        char title__[96];                                                          \
        (void)snprintf(title__, sizeof(title__), "%s%s",                          \
                       widget_names[(size_t)(WIDGET)],                              \
                       dashboard->focused == (WIDGET) ? "  [focused]" : "");      \
        od_canvas_write(canvas, (X) + 2, (Y), title__,                             \
                        (size_t)((W) > 4 ? (W) - 4 : 0),                           \
                        dashboard->focused == (WIDGET) ? OD_ROLE_PRIMARY :         \
                                                         OD_ROLE_MUTED, 1U);        \
        ADD_HIT((X), (Y), (W), 1, OD_HIT_TOGGLE_EXPAND, (WIDGET), 0U);             \
    } while (0)

#define DRAW_BOX(X, Y, W, H, WIDGET)                                              \
    do {                                                                          \
        od_canvas_box(canvas, (X), (Y), (W), (H), ascii,                           \
                      dashboard->focused == (WIDGET) ? OD_ROLE_FOCUSED_BORDER :   \
                                                         OD_ROLE_INACTIVE_BORDER); \
        DRAW_TITLE((X), (Y), (W), (WIDGET));                                      \
    } while (0)

#define DRAW_SERVICES(X, Y, W, H)                                                  \
    do {                                                                          \
        int inner_width__ = (W) - 4;                                               \
        size_t rows__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                         \
        od_dashboard_set_page_size(dashboard, rows__);                             \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_SERVICES);                         \
        const char *columns__ = inner_width__ >= 72 ?                              \
            "SERVICE             GROUP      VARIABLE             PREF  PORT STATUS" : \
            "SERVICE          VARIABLE      PREF  PORT STATUS";                   \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, columns__,                       \
                        (size_t)(inner_width__ > 0 ? inner_width__ : 0),            \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_HIT((X) + 2, (Y) + 2, (W) - 4, 1, OD_HIT_SORT_COLUMN,                 \
                OD_WIDGET_SERVICES, (size_t)dashboard->sort);                      \
        size_t end__ = dashboard->page_start + rows__;                            \
        if (end__ > dashboard->visible_count) end__ = dashboard->visible_count;   \
        for (size_t visible__ = dashboard->page_start; visible__ < end__;          \
             ++visible__) {                                                        \
            const OdServiceRow *row__ =                                            \
                &dashboard->services[dashboard->visible_order[visible__]];          \
            char line__[384];                                                      \
            if (inner_width__ >= 72) {                                             \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-19.19s %-10.10s %-19.19s %5u %5u %-10.10s",                \
                    row__->service, row__->group, row__->variable,                 \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status));                        \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-16.16s %-13.13s %5u %5u %-10.10s",                         \
                    row__->service, row__->variable,                               \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status));                        \
            }                                                                      \
            bool selected__ = visible__ == dashboard->selected_visible;            \
            OdThemeRole role__ = selected__ ? OD_ROLE_SELECTED :                   \
                (row__->conflict ? OD_ROLE_DANGER :                                \
                 (row__->status == OD_SERVICE_REASSIGNED ? OD_ROLE_WARNING :       \
                                                            OD_ROLE_DEFAULT));      \
            int row_y__ = (Y) + 3 + (int)(visible__ - dashboard->page_start);      \
            od_canvas_write(canvas, (X) + 2, row_y__, line__,                      \
                            (size_t)(inner_width__ > 0 ? inner_width__ : 0),        \
                            role__, selected__ ? 2U : 0U);                          \
            ADD_HIT((X) + 1, row_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,               \
                    OD_WIDGET_SERVICES, visible__);                                \
        }                                                                          \
        if (dashboard->visible_count == 0U) {                                      \
            od_canvas_write_centered(canvas, (Y) + (H) / 2,                       \
                dashboard->search[0] == '\0' ? "No managed services" :            \
                                                "No services match this search",   \
                OD_ROLE_MUTED, 0U);                                                \
        }                                                                          \
        char page__[192];                                                          \
        size_t page_number__ = dashboard->visible_count == 0U ? 0U :               \
            dashboard->page_start / dashboard->page_size + 1U;                    \
        size_t page_count__ = dashboard->visible_count == 0U ? 0U :                \
            (dashboard->visible_count + dashboard->page_size - 1U) /               \
                dashboard->page_size;                                              \
        const OdServiceRow *selected_row__ = od_dashboard_selected_service(dashboard);\
        (void)snprintf(page__, sizeof(page__),                                     \
                       "Page %zu/%zu  %zu shown%s%s%s",                           \
                       page_number__, page_count__, dashboard->visible_count,       \
                       dashboard->search[0] == '\0' ? "" : "  Search: ",          \
                       dashboard->search[0] == '\0' ? "" : dashboard->search,     \
                       selected_row__ != NULL && selected_row__->conflict ?         \
                           "  ! conflict" : "");                                  \
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)(inner_width__ > 0 ? inner_width__ : 0),            \
                        selected_row__ != NULL && selected_row__->conflict ?        \
                            OD_ROLE_WARNING : OD_ROLE_MUTED, 0U);                   \
    } while (0)

#define DRAW_CONFLICTS(X, Y, W, H)                                                 \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_CONFLICTS);                        \
        size_t rows__ = (H) > 4 ? (size_t)((H) - 4) : 1U;                         \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_CONFLICTS, rows__); \
        size_t conflict_ordinal__ = 0U;                                            \
        size_t total__ = 0U;                                                       \
        for (size_t count_index__ = 0U; count_index__ < dashboard->service_count; \
             ++count_index__) {                                                    \
            if (dashboard->services[count_index__].conflict) ++total__;            \
        }                                                                          \
        size_t end__ = dashboard->conflict_page_start + rows__;                   \
        if (end__ > total__) end__ = total__;                                      \
        for (size_t row_index__ = 0U; row_index__ < dashboard->service_count;     \
             ++row_index__) {                                                      \
            const OdServiceRow *row__ = &dashboard->services[row_index__];          \
            if (!row__->conflict) continue;                                        \
            if (conflict_ordinal__ < dashboard->conflict_page_start ||             \
                conflict_ordinal__ >= end__) {                                     \
                ++conflict_ordinal__;                                              \
                continue;                                                          \
            }                                                                      \
            char line__[320];                                                      \
            (void)snprintf(line__, sizeof(line__), "! %.96s  %u  %.160s",         \
                           row__->service, (unsigned)row__->preferred_port,         \
                           row__->conflict_detail);                                \
            int line_y__ = (Y) + 2 + (int)(conflict_ordinal__ -                   \
                                             dashboard->conflict_page_start);       \
            bool selected__ = conflict_ordinal__ == dashboard->conflict_selected;  \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED : OD_ROLE_DANGER,         \
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_CONFLICTS, conflict_ordinal__);                      \
            ++conflict_ordinal__;                                                  \
        }                                                                          \
        if (total__ == 0U)                                                         \
            od_canvas_write(canvas, (X) + 2, (Y) + 2, "No conflicts detected",    \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            OD_ROLE_SUCCESS, 0U);                                  \
        char page__[64];                                                           \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu conflict(s)",   \
            total__ == 0U ? 0U : dashboard->conflict_page_start / rows__ + 1U,    \
            total__ == 0U ? 0U : (total__ + rows__ - 1U) / rows__, total__);       \
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_LISTENERS(X, Y, W, H)                                                 \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_LISTENERS);                        \
        size_t capacity__ = (H) > 4 ? (size_t)((H) - 4) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_LISTENERS,          \
                                          capacity__);                             \
        size_t start__ = dashboard->listener_page_start;                          \
        if (start__ >= dashboard->snapshot->endpoint_count) start__ = 0U;          \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->snapshot->endpoint_count; ++offset__) {\
            const OdEndpoint *endpoint__ =                                         \
                &dashboard->snapshot->endpoints[start__ + offset__];               \
            char line__[384];                                                      \
            (void)snprintf(line__, sizeof(line__), "%s %s:%u  %s%s%ld",          \
                endpoint__->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP",          \
                endpoint__->local_address, (unsigned)endpoint__->local_port,       \
                endpoint__->process[0] == '\0' ? "unknown owner" : endpoint__->process,\
                endpoint__->pid == 0 ? "" : " pid ",                             \
                endpoint__->pid == 0 ? 0L : (long)endpoint__->pid);                \
            int line_y__ = (Y) + 2 + (int)offset__;                               \
            bool selected__ = start__ + offset__ == dashboard->listener_selected;  \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED :                         \
                                (endpoint__->permission_limited ? OD_ROLE_WARNING :\
                                                                  OD_ROLE_DEFAULT),\
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_LISTENERS, start__ + offset__);                      \
        }                                                                          \
        if (dashboard->snapshot->endpoint_count == 0U)                             \
            od_canvas_write(canvas, (X) + 2, (Y) + 2, "No host listeners found",  \
                            (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);   \
        char page__[64];                                                           \
        size_t total__ = dashboard->snapshot->endpoint_count;                      \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu listener(s)",   \
            total__ == 0U ? 0U : start__ / capacity__ + 1U,                       \
            total__ == 0U ? 0U : (total__ + capacity__ - 1U) / capacity__, total__);\
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_DOCKER(X, Y, W, H)                                                    \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_DOCKER);                           \
        size_t capacity__ = (H) > 4 ? (size_t)((H) - 4) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_DOCKER, capacity__);\
        size_t start__ = dashboard->docker_page_start;                            \
        if (start__ >= dashboard->snapshot->docker_mapping_count) start__ = 0U;    \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->snapshot->docker_mapping_count;       \
             ++offset__) {                                                         \
            const OdDockerMapping *mapping__ =                                     \
                &dashboard->snapshot->docker_mappings[start__ + offset__];          \
            char line__[320];                                                      \
            (void)snprintf(line__, sizeof(line__), "%s  %s:%u -> %u/%s",         \
                mapping__->container,                                              \
                mapping__->bind_address[0] == '\0' ? "0.0.0.0" :                  \
                                                     mapping__->bind_address,       \
                (unsigned)mapping__->host_port, (unsigned)mapping__->container_port,\
                mapping__->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp");          \
            int line_y__ = (Y) + 2 + (int)offset__;                               \
            bool selected__ = start__ + offset__ == dashboard->docker_selected;    \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,        \
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_DOCKER, start__ + offset__);                         \
        }                                                                          \
        if (dashboard->snapshot->docker_mapping_count == 0U)                       \
            od_canvas_write(canvas, (X) + 2, (Y) + 2,                              \
                            dashboard->snapshot->docker_available ?                \
                                "No published Docker ports" : "Docker unavailable",\
                            (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);   \
        char page__[64];                                                           \
        size_t total__ = dashboard->snapshot->docker_mapping_count;                \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu mapping(s)",    \
            total__ == 0U ? 0U : start__ / capacity__ + 1U,                       \
            total__ == 0U ? 0U : (total__ + capacity__ - 1U) / capacity__, total__);\
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_WIDGET(WIDGET, X, Y, W, H)                                            \
    do {                                                                          \
        switch (WIDGET) {                                                         \
            case OD_WIDGET_SERVICES: DRAW_SERVICES((X), (Y), (W), (H)); break;    \
            case OD_WIDGET_CONFLICTS: DRAW_CONFLICTS((X), (Y), (W), (H)); break;  \
            case OD_WIDGET_LISTENERS: DRAW_LISTENERS((X), (Y), (W), (H)); break;  \
            case OD_WIDGET_DOCKER: DRAW_DOCKER((X), (Y), (W), (H)); break;        \
            case OD_WIDGET_COUNT: break;                                          \
        }                                                                          \
    } while (0)

    if (dashboard->expanded) {
        DRAW_WIDGET(dashboard->focused, 1, content_y, (int)canvas->width - 2,
                    content_height);
    } else if (canvas->width >= 120U) {
        int available_width = (int)canvas->width - 3;
        int service_width = (available_width * 2) / 3;
        int secondary_x = 2 + service_width;
        int secondary_width = (int)canvas->width - secondary_x - 1;
        int first_height = content_height / 3;
        int second_height = content_height / 3;
        int third_height = content_height - first_height - second_height;
        DRAW_SERVICES(1, content_y, service_width, content_height);
        DRAW_CONFLICTS(secondary_x, content_y, secondary_width, first_height);
        DRAW_LISTENERS(secondary_x, content_y + first_height,
                       secondary_width, second_height);
        DRAW_DOCKER(secondary_x, content_y + first_height + second_height,
                    secondary_width, third_height);
    } else if (canvas->width >= 80U) {
        int primary_height = (content_height * 2) / 3;
        int secondary_height = content_height - primary_height;
        OdDashboardWidget secondary = dashboard->focused == OD_WIDGET_SERVICES ?
            OD_WIDGET_CONFLICTS : dashboard->focused;
        DRAW_SERVICES(1, content_y, (int)canvas->width - 2, primary_height);
        DRAW_WIDGET(secondary, 1, content_y + primary_height,
                    (int)canvas->width - 2, secondary_height);
    } else {
        char tabs[128];
        (void)snprintf(tabs, sizeof(tabs), "[%s]  %s  %s  %s",
                       widget_names[(size_t)dashboard->focused],
                       widget_names[((size_t)dashboard->focused + 1U) % OD_WIDGET_COUNT],
                       widget_names[((size_t)dashboard->focused + 2U) % OD_WIDGET_COUNT],
                       widget_names[((size_t)dashboard->focused + 3U) % OD_WIDGET_COUNT]);
        od_canvas_write(canvas, 1, 3, tabs, canvas->width - 2U, OD_ROLE_MUTED, 0U);
        DRAW_WIDGET(dashboard->focused, 1, content_y,
                    (int)canvas->width - 2, content_height);
    }

#undef DRAW_WIDGET
#undef DRAW_DOCKER
#undef DRAW_LISTENERS
#undef DRAW_CONFLICTS
#undef DRAW_SERVICES
#undef DRAW_BOX
#undef DRAW_TITLE
#undef ADD_HIT
}
