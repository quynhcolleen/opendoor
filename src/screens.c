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

size_t od_menu_page_size(size_t viewport_height, size_t item_count) {
    size_t available = viewport_height > 14U ? viewport_height - 14U : 1U;
    return available < item_count ? available : item_count;
}

size_t od_menu_page_start(size_t selected, size_t page_size) {
    return page_size == 0U ? 0U : selected / page_size * page_size;
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
    size_t visible_count = od_menu_page_size(canvas->height, item_count);
    size_t page_start = od_menu_page_start(view->selected_item, visible_count);
    size_t page_end = page_start + visible_count;
    if (page_end > item_count) page_end = item_count;
    int box_height = (int)visible_count + 4;
    int box_x = ((int)canvas->width - box_width) / 2;
    int box_y = 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    for (size_t index = page_start; index < page_end; ++index) {
        char line[96];
        bool selected = index == view->selected_item;
        (void)snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", items[index]);
        od_canvas_write(canvas, box_x + 2,
                        box_y + 2 + (int)(index - page_start), line,
                        (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    char page[64];
    size_t page_count = (item_count + visible_count - 1U) / visible_count;
    (void)snprintf(page, sizeof(page), "Menu page %zu/%zu",
                   page_start / visible_count + 1U, page_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
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

    char header[384];
    char refresh[64];
    (void)snprintf(refresh, sizeof(refresh),
                   dashboard->auto_refresh ? "auto %us" : "manual",
                   dashboard->refresh_seconds);
    (void)snprintf(header, sizeof(header),
                   "OPEN DOOR / %s / profile: %s / scan #%llu / refresh: %s%s",
                   dashboard->project_name,
                   dashboard->profile_saved ? "saved" : "listener-only",
                   (unsigned long long)dashboard->snapshot->generation,
                   refresh,
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
                    "PgUp/PgDn Page  Up/Down Select  Tab Focus  e Expand  d Details  / Search  Esc Back",
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

#define DRAW_SCROLL_CONTROLS(X, Y, W, WIDGET)                                     \
    do {                                                                          \
        int up_x__ = (X) + (W) - 8;                                               \
        int down_x__ = (X) + (W) - 4;                                             \
        od_canvas_write(canvas, up_x__, (Y), ascii ? "[^]" : "[↑]", 3U,        \
                        OD_ROLE_MUTED, 0U);                                        \
        od_canvas_write(canvas, down_x__, (Y), ascii ? "[v]" : "[↓]", 3U,      \
                        OD_ROLE_MUTED, 0U);                                        \
        ADD_HIT(up_x__, (Y), 3, 1, OD_HIT_SCROLL_UP, (WIDGET), 0U);                \
        ADD_HIT(down_x__, (Y), 3, 1, OD_HIT_SCROLL_DOWN, (WIDGET), 0U);            \
    } while (0)

#define DRAW_SERVICES(X, Y, W, H)                                                  \
    do {                                                                          \
        int inner_width__ = (W) - 4;                                               \
        size_t rows__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                         \
        od_dashboard_set_page_size(dashboard, rows__);                             \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_SERVICES);                         \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_SERVICES);                  \
        const char *columns__ = inner_width__ >= 72 ?                              \
            "SERVICE        GROUP   VARIABLE      PREF  PORT STATUS   CONFLICT" : \
            "SERVICE       VARIABLE      PREF  PORT STATUS   CFL";                \
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
                    "%-14.14s %-7.7s %-12.12s %5u %5u %-8.8s %-8s",               \
                    row__->service, row__->group, row__->variable,                 \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status),                         \
                    row__->conflict ? "YES" : "NO");                             \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-13.13s %-11.11s %5u %5u %-8.8s %-3s",                      \
                    row__->service, row__->variable,                               \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status),                         \
                    row__->conflict ? "YES" : "NO");                             \
            }                                                                      \
            bool selected__ = visible__ == dashboard->selected_visible;            \
            OdThemeRole role__ = selected__ ? OD_ROLE_SELECTED :                   \
                (row__->conflict ? OD_ROLE_DANGER :                                \
                 ((row__->status == OD_SERVICE_REASSIGNED ||                       \
                   row__->status == OD_SERVICE_STALE) ? OD_ROLE_WARNING :          \
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
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_CONFLICTS);                 \
        size_t rows__ = (H) > 4 ? (size_t)((H) - 4) : 1U;                         \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_CONFLICTS, rows__); \
        size_t conflict_ordinal__ = 0U;                                            \
        size_t total__ = dashboard->conflict_visible_count;                        \
        size_t end__ = dashboard->conflict_page_start + rows__;                   \
        if (end__ > total__) end__ = total__;                                      \
        for (size_t visible__ = 0U; visible__ < total__; ++visible__) {             \
            const OdServiceRow *row__ =                                             \
                &dashboard->services[dashboard->conflict_order[visible__]];         \
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
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_LISTENERS);                 \
        size_t capacity__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_LISTENERS,          \
                                          capacity__);                             \
        int listener_inner__ = (W) - 4;                                            \
        const char *listener_columns__ = listener_inner__ >= 72 ?                  \
            " PORT PROTO BIND             PROCESS             PID USER       SOURCE" :\
            " PORT PROTO BIND             PROCESS";                              \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, listener_columns__,              \
                        (size_t)(listener_inner__ > 0 ? listener_inner__ : 0),      \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_HIT((X) + 2, (Y) + 2, (W) - 4, 1, OD_HIT_SORT_COLUMN,                 \
                OD_WIDGET_LISTENERS, 0U);                                         \
        size_t start__ = dashboard->listener_page_start;                          \
        if (start__ >= dashboard->listener_visible_count) start__ = 0U;             \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->listener_visible_count; ++offset__) { \
            const OdEndpoint *endpoint__ =                                         \
                &dashboard->snapshot->endpoints[                                   \
                    dashboard->listener_order[start__ + offset__]];                \
            char line__[384];                                                      \
            if (listener_inner__ >= 72) {                                          \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%5u %-5s %-16.16s %-19.19s %6ld %-10.10s %-8s",             \
                    (unsigned)endpoint__->local_port,                              \
                    endpoint__->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP",      \
                    endpoint__->local_address,                                    \
                    endpoint__->process[0] == '\0' ? "unknown owner" :            \
                                                       endpoint__->process,        \
                    (long)endpoint__->pid, endpoint__->user,                       \
                    endpoint__->permission_limited ? "limited" : "kernel");      \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__), "%5u %-4s %.18s  %.20s",  \
                    (unsigned)endpoint__->local_port,                              \
                    endpoint__->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP",      \
                    endpoint__->local_address,                                    \
                    endpoint__->process[0] == '\0' ? "unknown owner" :            \
                                                       endpoint__->process);       \
            }                                                                      \
            int line_y__ = (Y) + 3 + (int)offset__;                               \
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
        if (dashboard->listener_visible_count == 0U)                               \
            od_canvas_write(canvas, (X) + 2, (Y) + 3,                              \
                            dashboard->listener_search[0] == '\0' ?                \
                                "No host listeners found" : "No listeners match",\
                            (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);   \
        char page__[64];                                                           \
        size_t total__ = dashboard->listener_visible_count;                        \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu listener(s)",   \
            total__ == 0U ? 0U : start__ / capacity__ + 1U,                       \
            total__ == 0U ? 0U : (total__ + capacity__ - 1U) / capacity__, total__);\
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_DOCKER(X, Y, W, H)                                                    \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_DOCKER);                           \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_DOCKER);                    \
        size_t capacity__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_DOCKER, capacity__);\
        int docker_inner__ = (W) - 4;                                              \
        const char *docker_columns__ = docker_inner__ >= 72 ?                      \
            "CONTAINER        HOST PORT  CONTAINER PORT  PROTOCOL PROJECT" :      \
            "CONTAINER          HOST -> CONTAINER/PROTO";                         \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, docker_columns__,                \
                        (size_t)(docker_inner__ > 0 ? docker_inner__ : 0),          \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_HIT((X) + 2, (Y) + 2, (W) - 4, 1, OD_HIT_SORT_COLUMN,                 \
                OD_WIDGET_DOCKER, 0U);                                            \
        size_t start__ = dashboard->docker_page_start;                            \
        if (start__ >= dashboard->docker_visible_count) start__ = 0U;              \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->docker_visible_count;                 \
             ++offset__) {                                                         \
            const OdDockerMapping *mapping__ =                                     \
                &dashboard->snapshot->docker_mappings[                             \
                    dashboard->docker_order[start__ + offset__]];                  \
            char line__[320];                                                      \
            if (docker_inner__ >= 72) {                                            \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-16.16s %9u %15u %-8s %-16.16s",                            \
                    mapping__->container, (unsigned)mapping__->host_port,           \
                    (unsigned)mapping__->container_port,                           \
                    mapping__->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp",       \
                    mapping__->project);                                           \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__), "%-18.18s %5u -> %5u/%s",  \
                    mapping__->container, (unsigned)mapping__->host_port,           \
                    (unsigned)mapping__->container_port,                           \
                    mapping__->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp");      \
            }                                                                      \
            int line_y__ = (Y) + 3 + (int)offset__;                               \
            bool selected__ = start__ + offset__ == dashboard->docker_selected;    \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,        \
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_DOCKER, start__ + offset__);                         \
        }                                                                          \
        if (dashboard->docker_visible_count == 0U)                                 \
            od_canvas_write(canvas, (X) + 2, (Y) + 3,                              \
                            dashboard->docker_search[0] != '\0' ?                  \
                                "No Docker mappings match" :                      \
                                (dashboard->snapshot->docker_available ?           \
                                    "No published Docker ports" : "Docker unavailable"),\
                            (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);   \
        char page__[64];                                                           \
        size_t total__ = dashboard->docker_visible_count;                          \
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
    } else if (canvas->height >= 21U && canvas->width >= 120U) {
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
    } else if (canvas->height >= 21U && canvas->width >= 80U) {
        int primary_height = (content_height * 2) / 3;
        int secondary_height = content_height - primary_height;
        OdDashboardWidget secondary = dashboard->focused == OD_WIDGET_SERVICES ?
            OD_WIDGET_CONFLICTS : dashboard->focused;
        DRAW_SERVICES(1, content_y, (int)canvas->width - 2, primary_height);
        DRAW_WIDGET(secondary, 1, content_y + primary_height,
                    (int)canvas->width - 2, secondary_height);
    } else {
        int tab_x = 1;
        for (size_t offset = 0U; offset < OD_WIDGET_COUNT; ++offset) {
            OdDashboardWidget widget = (OdDashboardWidget)(
                ((size_t)dashboard->focused + offset) % OD_WIDGET_COUNT);
            char tab[48];
            (void)snprintf(tab, sizeof(tab), offset == 0U ? "[%s]" : "%s",
                           widget_names[(size_t)widget]);
            int tab_width = (int)strlen(tab);
            if (tab_x + tab_width >= (int)canvas->width - 1) break;
            od_canvas_write(canvas, tab_x, 3, tab, (size_t)tab_width,
                            offset == 0U ? OD_ROLE_PRIMARY : OD_ROLE_MUTED,
                            offset == 0U ? 1U : 0U);
            ADD_HIT(tab_x, 3, tab_width, 1, OD_HIT_FOCUS_WIDGET, widget, 0U);
            tab_x += tab_width + 2;
        }
        DRAW_WIDGET(dashboard->focused, 1, content_y,
                    (int)canvas->width - 2, content_height);
    }

#undef DRAW_WIDGET
#undef DRAW_DOCKER
#undef DRAW_LISTENERS
#undef DRAW_CONFLICTS
#undef DRAW_SERVICES
#undef DRAW_BOX
#undef DRAW_SCROLL_CONTROLS
#undef DRAW_TITLE
#undef ADD_HIT
}

static const OdServiceRow *selected_conflict(const OdDashboard *dashboard) {
    if (dashboard->conflict_selected >= dashboard->conflict_visible_count) return NULL;
    return &dashboard->services[dashboard->conflict_order[dashboard->conflict_selected]];
}

static void append_detail(char *text,
                          size_t capacity,
                          size_t *used,
                          const char *label,
                          const char *value) {
    if (*used >= capacity) return;
    int count = snprintf(text + *used, capacity - *used, "%s: %s\n", label,
                         value == NULL || value[0] == '\0' ? "—" : value);
    if (count < 0) return;
    size_t added = (size_t)count;
    *used += added < capacity - *used ? added : capacity - *used;
}

static void append_detail_number(char *text,
                                 size_t capacity,
                                 size_t *used,
                                 const char *label,
                                 unsigned long long value) {
    char number[48];
    (void)snprintf(number, sizeof(number), "%llu", value);
    append_detail(text, capacity, used, label, number);
}

static size_t detail_chunk(const char *text, size_t length, size_t width) {
    size_t chunk = length < width ? length : width;
    if (chunk == length) return chunk;
    while (chunk > 0U && (((unsigned char)text[chunk] & 0xc0U) == 0x80U)) --chunk;
    return chunk == 0U ? (length < width ? length : width) : chunk;
}

static size_t wrapped_detail_rows(const char *text, size_t width) {
    size_t rows = 0U;
    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline == NULL ? strlen(cursor) : (size_t)(newline - cursor);
        if (length == 0U) {
            ++rows;
        } else {
            size_t consumed = 0U;
            while (consumed < length) {
                consumed += detail_chunk(cursor + consumed, length - consumed, width);
                ++rows;
            }
        }
        if (newline == NULL) break;
        cursor = newline + 1;
    }
    return rows;
}

static void build_dashboard_detail(const OdDashboard *dashboard,
                                   char *title,
                                   size_t title_capacity,
                                   char *text,
                                   size_t text_capacity) {
    size_t used = 0U;
    text[0] = '\0';
    if (dashboard->focused == OD_WIDGET_SERVICES ||
        dashboard->focused == OD_WIDGET_CONFLICTS) {
        const OdServiceRow *row = dashboard->focused == OD_WIDGET_SERVICES ?
            od_dashboard_selected_service(dashboard) : selected_conflict(dashboard);
        (void)snprintf(title, title_capacity, "%s",
                       dashboard->focused == OD_WIDGET_SERVICES ?
                           "Service details" : "Conflict details");
        if (row == NULL) {
            append_detail(text, text_capacity, &used, "Selection", "No row selected");
            return;
        }
        append_detail(text, text_capacity, &used, "Stable ID", row->stable_id);
        append_detail(text, text_capacity, &used, "Service", row->service);
        append_detail(text, text_capacity, &used, "Group", row->group);
        append_detail(text, text_capacity, &used, "Variable", row->variable);
        append_detail_number(text, text_capacity, &used, "Preferred port",
                             row->preferred_port);
        append_detail_number(text, text_capacity, &used, "Selected port",
                             row->selected_port);
        append_detail(text, text_capacity, &used, "Status",
                      od_service_status_name(row->status));
        append_detail(text, text_capacity, &used, "Conflict",
                      row->conflict ? row->conflict_detail : "None");
        return;
    }
    if (dashboard->focused == OD_WIDGET_LISTENERS) {
        (void)snprintf(title, title_capacity, "Listener details");
        if (dashboard->listener_selected >= dashboard->listener_visible_count) {
            append_detail(text, text_capacity, &used, "Selection", "No row selected");
            return;
        }
        const OdEndpoint *endpoint =
            &dashboard->snapshot->endpoints[
                dashboard->listener_order[dashboard->listener_selected]];
        append_detail(text, text_capacity, &used, "Stable ID", endpoint->stable_id);
        append_detail(text, text_capacity, &used, "Protocol",
                      endpoint->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP");
        append_detail(text, text_capacity, &used, "Local address", endpoint->local_address);
        append_detail_number(text, text_capacity, &used, "Local port", endpoint->local_port);
        append_detail(text, text_capacity, &used, "Remote address", endpoint->remote_address);
        append_detail_number(text, text_capacity, &used, "Remote port", endpoint->remote_port);
        append_detail(text, text_capacity, &used, "Process", endpoint->process);
        append_detail_number(text, text_capacity, &used, "PID",
                             (unsigned long long)endpoint->pid);
        append_detail(text, text_capacity, &used, "User", endpoint->user);
        append_detail_number(text, text_capacity, &used, "UID",
                             (unsigned long long)endpoint->uid);
        append_detail_number(text, text_capacity, &used, "Socket inode", endpoint->inode);
        append_detail(text, text_capacity, &used, "Executable", endpoint->executable);
        append_detail(text, text_capacity, &used, "Command", endpoint->command);
        append_detail(text, text_capacity, &used, "Ownership",
                      endpoint->permission_limited ?
                          "Process details limited by permissions" : "Resolved");
        return;
    }
    (void)snprintf(title, title_capacity, "Docker mapping details");
    if (dashboard->docker_selected >= dashboard->docker_visible_count) {
        append_detail(text, text_capacity, &used, "Selection", "No row selected");
        return;
    }
    const OdDockerMapping *mapping =
        &dashboard->snapshot->docker_mappings[
            dashboard->docker_order[dashboard->docker_selected]];
    append_detail(text, text_capacity, &used, "Container", mapping->container);
    append_detail(text, text_capacity, &used, "Container ID", mapping->container_id);
    append_detail(text, text_capacity, &used, "Compose project", mapping->project);
    append_detail(text, text_capacity, &used, "Compose service", mapping->service);
    append_detail(text, text_capacity, &used, "Bind address", mapping->bind_address);
    append_detail_number(text, text_capacity, &used, "Host port", mapping->host_port);
    append_detail_number(text, text_capacity, &used, "Container port",
                         mapping->container_port);
    append_detail(text, text_capacity, &used, "Protocol",
                  mapping->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP");
}

size_t od_render_dashboard_detail(OdCanvas *canvas,
                                  const OdDashboard *dashboard,
                                  size_t page,
                                  bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return 1U;
    }
    char title[96];
    char detail[8192];
    build_dashboard_detail(dashboard, title, sizeof(title), detail, sizeof(detail));
    od_canvas_write(canvas, 2, 1, title, canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Full selected-row values • content wraps inside this viewport",
                    canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 7;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    size_t line_width = (size_t)(box_width - 4);
    size_t page_size = box_height > 3 ? (size_t)(box_height - 3) : 1U;
    size_t row_count = wrapped_detail_rows(detail, line_width);
    size_t page_count = row_count == 0U ? 1U : (row_count + page_size - 1U) / page_size;
    size_t current_page = page < page_count ? page : page_count - 1U;
    size_t first_row = current_page * page_size;
    size_t final_row = first_row + page_size;
    char *line = malloc(line_width + 1U);
    if (line != NULL) {
        size_t row = 0U;
        const char *cursor = detail;
        while (*cursor != '\0' && row < final_row) {
            const char *newline = strchr(cursor, '\n');
            size_t length = newline == NULL ? strlen(cursor) : (size_t)(newline - cursor);
            size_t consumed = 0U;
            do {
                size_t chunk = length == 0U ? 0U :
                    detail_chunk(cursor + consumed, length - consumed, line_width);
                if (row >= first_row && row < final_row) {
                    if (chunk > 0U) memcpy(line, cursor + consumed, chunk);
                    line[chunk] = '\0';
                    od_canvas_write(canvas, box_x + 2,
                                    box_y + 1 + (int)(row - first_row), line,
                                    line_width, OD_ROLE_DEFAULT, 0U);
                }
                consumed += chunk;
                ++row;
            } while (consumed < length && row < final_row);
            if (newline == NULL) break;
            cursor = newline + 1;
        }
        free(line);
    }
    char page_text[96];
    (void)snprintf(page_text, sizeof(page_text), "Page %zu/%zu  %zu wrapped line(s)",
                   current_page + 1U, page_count, row_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page_text,
                    line_width, OD_ROLE_MUTED, 0U);
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "PgUp/PgDn Page  Up/Down Page  Home/End  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
    return page_count;
}

void od_render_profile_editor(OdCanvas *canvas,
                              const OdProfileView *view,
                              bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Edit project profile", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    char paths[640];
    (void)snprintf(paths, sizeof(paths), "Profile: %s  •  Assignments: %s",
                   view->profile_path == NULL ? "unavailable" : view->profile_path,
                   view->assignment_path == NULL ? "unavailable" : view->assignment_path);
    od_canvas_write(canvas, 2, 2, paths, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 6;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "SERVICE               GROUP          VARIABLE             PORT PROTOCOL",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);
    size_t rows = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    size_t selected = view->selected_service;
    if (view->profile->service_count == 0U) selected = 0U;
    if (selected >= view->profile->service_count && view->profile->service_count > 0U) {
        selected = view->profile->service_count - 1U;
    }
    size_t page_start = (selected / rows) * rows;
    size_t end = page_start + rows;
    if (end > view->profile->service_count) end = view->profile->service_count;
    for (size_t index = page_start; index < end; ++index) {
        const OdService *service = &view->profile->services[index];
        const char *protocol = service->protocols == (OD_PROTOCOL_TCP | OD_PROTOCOL_UDP) ?
            "tcp,udp" : (service->protocols == OD_PROTOCOL_UDP ? "udp" : "tcp");
        char row[384];
        (void)snprintf(row, sizeof(row), "%-21.21s %-14.14s %-20.20s %5u %-7s",
                       service->name, service->group, service->variable,
                       (unsigned)service->preferred_port, protocol);
        bool is_selected = index == selected;
        od_canvas_write(canvas, box_x + 2,
                        box_y + 2 + (int)(index - page_start), row,
                        (size_t)(box_width - 4),
                        is_selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        is_selected ? 2U : 0U);
    }
    if (view->profile->service_count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No managed services • press a to add one",
                                 OD_ROLE_MUTED, 0U);
    }
    char page_text[96];
    size_t pages = view->profile->service_count == 0U ? 1U :
        (view->profile->service_count + rows - 1U) / rows;
    (void)snprintf(page_text, sizeof(page_text), "Page %zu/%zu  %zu service(s)",
                   view->profile->service_count == 0U ? 1U : page_start / rows + 1U,
                   pages, view->profile->service_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page_text,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (view->status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, view->status,
                        canvas->width - 4U, OD_ROLE_WARNING, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "x Reset assignments  e Edit  a Add  s Save  Up/Down/Pg Page  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_conflict_resolution(OdCanvas *canvas,
                                   const OdResolution *resolution,
                                   bool ascii,
                                   const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Resolve conflict", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    const OdResolutionItem *item = od_resolution_current(resolution);
    if (item == NULL) {
        od_canvas_write_centered(canvas, (int)canvas->height / 2,
                                 "Every conflict has been reviewed",
                                 OD_ROLE_SUCCESS, 1U);
    } else {
        char progress[96];
        (void)snprintf(progress, sizeof(progress), "Conflict %zu of %zu",
                       resolution->current + 1U, resolution->count);
        od_canvas_write(canvas, 2, 2, progress, canvas->width - 4U,
                        OD_ROLE_MUTED, 0U);
        int box_x = 2;
        int box_y = 4;
        int box_width = (int)canvas->width - 4;
        int box_height = (int)canvas->height - 9;
        od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                      OD_ROLE_FOCUSED_BORDER);
        const OdAllocation *allocation =
            &resolution->plan->items[item->allocation_index];
        char line[320];
        (void)snprintf(line, sizeof(line), "Service: %s", item->service);
        od_canvas_write(canvas, box_x + 2, box_y + 2, line,
                        (size_t)(box_width - 4), OD_ROLE_DEFAULT, 1U);
        (void)snprintf(line, sizeof(line), "Requested port: %u",
                       (unsigned)item->original_port);
        od_canvas_write(canvas, box_x + 2, box_y + 4, line,
                        (size_t)(box_width - 4), OD_ROLE_WARNING, 0U);
        (void)snprintf(line, sizeof(line), "Occupied by: %s", item->owner);
        od_canvas_write(canvas, box_x + 2, box_y + 5, line,
                        (size_t)(box_width - 4), OD_ROLE_DANGER, 0U);
        (void)snprintf(line, sizeof(line), "Recommended free port: %u",
                       (unsigned)allocation->new_port);
        od_canvas_write(canvas, box_x + 2, box_y + 7, line,
                        (size_t)(box_width - 4), OD_ROLE_SUCCESS, 1U);
    }
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Accept Enter] [Edit e] [Skip s] [Cancel Esc]",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

static const OdService *screen_find_service(const OdProfile *profile, const char *id) {
    for (size_t index = 0U; index < profile->service_count; ++index) {
        if (strcmp(profile->services[index].id, id) == 0) return &profile->services[index];
    }
    return NULL;
}

void od_render_change_review(OdCanvas *canvas,
                             const OdProfile *profile,
                             const OdAllocationPlan *plan,
                             size_t selected,
                             bool ascii,
                             const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Review assignment changes",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char summary[128];
    (void)snprintf(summary, sizeof(summary),
                   "%zu managed service(s) • no files change until you confirm",
                   plan->count);
    od_canvas_write(canvas, 2, 2, summary, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "SERVICE                    OLD     NEW  REASON",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);
    size_t page_size = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    if (plan->count > 0U && selected >= plan->count) selected = plan->count - 1U;
    size_t page_start = (selected / page_size) * page_size;
    size_t end = page_start + page_size;
    if (end > plan->count) end = plan->count;
    for (size_t index = page_start; index < end; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        const OdService *service = screen_find_service(profile, allocation->service_id);
        const char *reason = allocation->new_port == 0U ? "SKIPPED" :
            (allocation->reason == OD_ALLOC_PRESERVED ? "SAVED" :
             (allocation->reason == OD_ALLOC_REASSIGNED ? "REASSIGNED" : "PREFERRED"));
        char row[320];
        char old_port[16];
        char new_port[16];
        (void)snprintf(old_port, sizeof(old_port), allocation->old_port == 0U ? "-" : "%u",
                       (unsigned)allocation->old_port);
        (void)snprintf(new_port, sizeof(new_port), allocation->new_port == 0U ? "-" : "%u",
                       (unsigned)allocation->new_port);
        (void)snprintf(row, sizeof(row), "%-26.26s %7s %7s  %-10.10s",
                       service == NULL ? allocation->service_id : service->name,
                       old_port, new_port, reason);
        bool is_selected = index == selected;
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)(index - page_start),
                        row, (size_t)(box_width - 4),
                        is_selected ? OD_ROLE_SELECTED :
                            (allocation->new_port == 0U ? OD_ROLE_WARNING : OD_ROLE_DEFAULT),
                        is_selected ? 2U : 0U);
    }
    char page[80];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu",
                   plan->count == 0U ? 0U : page_start / page_size + 1U,
                   plan->count == 0U ? 0U : (plan->count + page_size - 1U) / page_size);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 3, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Save Enter] [Cancel Esc]  Up/Down Select  PgUp/PgDn Page",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_settings(OdCanvas *canvas, const OdSettingsView *view, bool ascii) {
    static const char *const unicode_names[] = {"auto", "always", "never"};
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Settings and appearance",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char path[320];
    (void)snprintf(path, sizeof(path), "Saved globally: %s",
                   view->settings_path == NULL ? "path unavailable" : view->settings_path);
    od_canvas_write(canvas, 2, 2, path, canvas->width - 4U, OD_ROLE_MUTED, 0U);

    int box_x = 2;
    int box_y = 4;
    int box_width = (int)canvas->width - 4;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    char rows[6][160];
    (void)snprintf(rows[0], sizeof(rows[0]), "Theme                 < %s >",
                   view->settings->theme);
    (void)snprintf(rows[1], sizeof(rows[1]), "Unicode               < %s >",
                   unicode_names[view->settings->unicode_mode]);
    (void)snprintf(rows[2], sizeof(rows[2]), "Reduce motion           [%s]",
                   view->settings->reduced_motion ? "on" : "off");
    (void)snprintf(rows[3], sizeof(rows[3]), "Mouse input             [%s]",
                   view->settings->mouse ? "on" : "off");
    (void)snprintf(rows[4], sizeof(rows[4]), "Refresh automatically   [%s]",
                   view->settings->auto_refresh ? "on" : "off");
    (void)snprintf(rows[5], sizeof(rows[5]), "Refresh interval       < %u seconds >",
                   view->settings->refresh_seconds);
    for (size_t index = 0U; index < 6U; ++index) {
        bool selected = index == view->selected_item;
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)index, rows[index],
                        (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 2U : 0U);
    }
    if (view->status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, view->status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Save Enter] [Cancel Esc]  Up/Down  Left/Right  Space Toggle",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_help(OdCanvas *canvas, OdHelp *help, bool ascii, const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Keyboard and workflow help",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char search[180];
    (void)snprintf(search, sizeof(search), "Search: %s%s",
                   help->query[0] == '\0' ? "all topics" : help->query,
                   help->query[0] == '\0' ? "" : "  (press / to change)");
    od_canvas_write(canvas, 2, 2, search, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    size_t rows = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    od_help_set_page_size(help, rows);
    size_t end = help->page_start + rows;
    if (end > help->visible_count) end = help->visible_count;
    for (size_t index = help->page_start; index < end; ++index) {
        char line[384];
        (void)snprintf(line, sizeof(line), "%-18.18s  %s",
                       od_help_key(help, index), od_help_description(help, index));
        bool selected = index == help->selected;
        od_canvas_write(canvas, box_x + 2,
                        box_y + 2 + (int)(index - help->page_start),
                        line, (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 2U : 0U);
    }
    if (help->visible_count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No help topics match • press / to change the search",
                                 OD_ROLE_MUTED, 0U);
    }
    char page[96];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu  %zu topic(s)",
                   help->visible_count == 0U ? 0U : help->page_start / rows + 1U,
                   help->visible_count == 0U ? 0U :
                       (help->visible_count + rows - 1U) / rows,
                   help->visible_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Select  PgUp/PgDn Page  / Search  Home/End  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}
