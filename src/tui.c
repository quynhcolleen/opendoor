#include "opendoor/tui.h"

#include "curses_compat.h"
#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/dashboard.h"
#include "opendoor/docker.h"
#include "opendoor/discovery.h"
#include "opendoor/onboarding.h"
#include "opendoor/screens.h"
#include "opendoor/scan.h"
#include "opendoor/theme.h"
#include "opendoor/ui.h"

#include <locale.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_t thread;
    atomic_int stage;
    atomic_bool done;
    atomic_size_t warning_count;
    OdScanSnapshot snapshot;
    OdError error;
    OdStatus status;
} StartupScan;

static uint64_t monotonic_milliseconds(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0U;
    return (uint64_t)time.tv_sec * UINT64_C(1000) +
           (uint64_t)time.tv_nsec / UINT64_C(1000000);
}

static void *startup_scan_main(void *argument) {
    StartupScan *scan = argument;
    atomic_store(&scan->stage, OD_LOAD_PROJECT_FILES);
    atomic_store(&scan->stage, OD_LOAD_KERNEL_SOCKETS);
    scan->status = od_scan_host(&scan->snapshot, &scan->error);
    atomic_store(&scan->warning_count, scan->snapshot.warning_count);
    atomic_store(&scan->stage, OD_LOAD_PROCESS_OWNERS);
    if (scan->status == OD_OK) {
        atomic_store(&scan->stage, OD_LOAD_DOCKER);
        scan->status = od_docker_scan("docker", 2500U, 4U * 1024U * 1024U,
                                     &scan->snapshot, &scan->error);
        atomic_store(&scan->warning_count, scan->snapshot.warning_count);
    }
    atomic_store(&scan->stage, OD_LOAD_RECONCILIATION);
    atomic_store(&scan->done, true);
    return NULL;
}

static int canvas_attributes(const OdCell *cell, bool use_color) {
    int attributes = 0;
    if (use_color) attributes |= COLOR_PAIR((int)cell->role + 1);
    if ((cell->attributes & 1U) != 0U) attributes |= A_BOLD;
    if ((cell->attributes & 2U) != 0U) attributes |= A_REVERSE;
    return attributes;
}

static void paint_canvas(WINDOW *window, const OdCanvas *canvas, bool use_color) {
    (void)werase(window);
    for (size_t y = 0U; y < canvas->height; ++y) {
        for (size_t x = 0U; x < canvas->width; ++x) {
            const OdCell *cell = &canvas->cells[y * canvas->width + x];
            (void)wattrset(window, canvas_attributes(cell, use_color));
            (void)wmove(window, (int)y, (int)x);
            (void)waddnstr(window, cell->glyph, -1);
        }
    }
    (void)wattrset(window, 0);
    (void)wnoutrefresh(window);
    (void)doupdate();
}

static bool initialize_colors(const OdTheme *theme, bool no_color) {
    if (no_color || !has_colors() || start_color() == ERR) return false;
    (void)use_default_colors();
    for (size_t role = 0U; role < OD_ROLE_COUNT; ++role) {
        if (init_pair((short)(role + 1U), theme->foreground[role],
                      theme->background[role]) == ERR) return false;
    }
    return true;
}

static const char *resolved_profile_path(const OpendoorOptions *options,
                                         char *default_path,
                                         size_t capacity) {
    if (options->profile_path != NULL) return options->profile_path;
    {
        const char *root = options->project_path == NULL ? "." : options->project_path;
        int count = snprintf(default_path, capacity, "%s/.opendoor/project.toml", root);
        if (count < 0 || (size_t)count >= capacity) return NULL;
    }
    return default_path;
}

static bool profile_exists(const OpendoorOptions *options) {
    char default_path[4096];
    const char *path = resolved_profile_path(options, default_path, sizeof(default_path));
    if (path == NULL) return false;
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static const char *project_label(const OpendoorOptions *options) {
    if (options->project_path != NULL) return options->project_path;
    return "current directory";
}

static size_t menu_item_count(bool configured) {
    return configured ? 7U : 5U;
}

static bool canvas_resize(OdCanvas *canvas, int width, int height, OdError *error) {
    if (width <= 0 || height <= 0) return false;
    if (canvas->cells != NULL && canvas->width == (size_t)width &&
        canvas->height == (size_t)height) return true;
    od_canvas_free(canvas);
    return od_canvas_init(canvas, (size_t)width, (size_t)height, error) == OD_OK;
}

static void menu_status(char *status, size_t capacity, bool configured, size_t selected) {
    static const char *const first_run_status[] = {
        "Review discovered candidates and create this project's profile.",
        "Inspect every host listener and its known owner.",
        "Choose theme, motion, mouse, and refresh preferences.",
        "Open searchable keyboard and workflow help.",
        "Exit OpenDoor without changing services or processes."
    };
    static const char *const configured_status[] = {
        "Open the saved service and listener dashboard.",
        "Review conflicts one at a time before any assignment is saved.",
        "Refresh host sockets, processes, and Docker mappings.",
        "Review the managed service profile.",
        "Choose theme, motion, mouse, and refresh preferences.",
        "Open searchable keyboard and workflow help.",
        "Exit OpenDoor without changing services or processes."
    };
    const char *message = configured ? configured_status[selected] : first_run_status[selected];
    (void)snprintf(status, capacity, "%s", message);
}

static bool prompt_text(WINDOW *window,
                        OdCanvas *canvas,
                        bool use_color,
                        bool ascii,
                        const char *title,
                        const char *label,
                        char *value,
                        size_t capacity) {
    size_t length = strlen(value);
    while (true) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        OdError error;
        if (!canvas_resize(canvas, width, height, &error)) return false;
        if (width < 60 || height < 18) {
            od_render_resize_required(canvas);
        } else {
            od_canvas_clear(canvas, OD_ROLE_DEFAULT);
            int box_width = width > 76 ? 72 : width - 4;
            int box_height = 9;
            int box_x = (width - box_width) / 2;
            int box_y = (height - box_height) / 2;
            od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                          OD_ROLE_FOCUSED_BORDER);
            od_canvas_write(canvas, box_x + 2, box_y + 1, title,
                            (size_t)(box_width - 4), OD_ROLE_PRIMARY, 1U);
            od_canvas_write(canvas, box_x + 2, box_y + 3, label,
                            (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
            char shown[512];
            (void)snprintf(shown, sizeof(shown), "> %s_", value);
            od_canvas_write(canvas, box_x + 2, box_y + 4, shown,
                            (size_t)(box_width - 4), OD_ROLE_SELECTED, 0U);
            od_canvas_write(canvas, box_x + 2, box_y + 7,
                            "Enter Accept  Esc Cancel  Backspace Delete",
                            (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
        }
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == 27) return false;
        if (input == '\n' || input == '\r') return true;
        if (input == 127 || input == 8) {
            if (length > 0U) value[--length] = '\0';
        } else if (input >= 0 && input <= 255 && isprint((unsigned char)input) &&
                   length + 1U < capacity) {
            value[length++] = (char)input;
            value[length] = '\0';
        }
    }
}

static bool prompt_candidate(WINDOW *window,
                             OdCanvas *canvas,
                             bool use_color,
                             bool ascii,
                             OdOnboarding *onboarding,
                             bool editing,
                             char *status,
                             size_t status_capacity) {
    char name[128] = {0};
    char group[128] = "default";
    char variable[128] = {0};
    char port_text[16] = {0};
    if (editing && onboarding->selected < onboarding->candidates.count) {
        const OdCandidate *candidate = &onboarding->candidates.items[onboarding->selected];
        (void)snprintf(name, sizeof(name), "%s", candidate->name);
        (void)snprintf(group, sizeof(group), "%s", candidate->group);
        (void)snprintf(variable, sizeof(variable), "%s", candidate->variable);
        (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)candidate->port);
    }
    const char *title = editing ? "Edit discovered service" : "Add managed service";
    if (!prompt_text(window, canvas, use_color, ascii, title, "Display name", name, sizeof(name)) ||
        !prompt_text(window, canvas, use_color, ascii, title, "Group", group, sizeof(group)) ||
        !prompt_text(window, canvas, use_color, ascii, title, "Environment variable", variable, sizeof(variable)) ||
        !prompt_text(window, canvas, use_color, ascii, title, "Preferred port", port_text, sizeof(port_text))) {
        (void)snprintf(status, status_capacity, "Edit cancelled; no candidate was changed.");
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long numeric = strtoul(port_text, &end, 10);
    if (errno != 0 || end == port_text || *end != '\0' || numeric > 65535UL) {
        (void)snprintf(status, status_capacity, "Preferred port must be a number from %u to %u.",
                       (unsigned)onboarding->port_min, (unsigned)onboarding->port_max);
        return false;
    }
    OdError error;
    OdStatus result = editing ?
        od_onboarding_edit_selected(onboarding, name, group, variable,
                                    (uint16_t)numeric, &error) :
        od_onboarding_add_manual(onboarding, name, group, variable,
                                 (uint16_t)numeric, OD_PROTOCOL_TCP, &error);
    if (result != OD_OK) {
        (void)snprintf(status, status_capacity, "%s", error.message);
        return false;
    }
    (void)snprintf(status, status_capacity,
                   editing ? "Candidate updated and marked reviewed." :
                             "Manual candidate added; review it before continuing.");
    return true;
}

static void project_display_name(const char *project_root, char *name, size_t capacity) {
    const char *end = project_root + strlen(project_root);
    while (end > project_root && end[-1] == '/') --end;
    const char *start = end;
    while (start > project_root && start[-1] != '/') --start;
    size_t length = (size_t)(end - start);
    if (length == 0U) {
        (void)snprintf(name, capacity, "Project");
    } else {
        if (length >= capacity) length = capacity - 1U;
        memcpy(name, start, length);
        name[length] = '\0';
    }
}

static bool run_onboarding(WINDOW *window,
                           OdCanvas *canvas,
                           bool use_color,
                           bool ascii,
                           const char *project_root,
                           OdProfile *profile) {
    OdCandidateList candidates;
    OdError error;
    od_candidate_list_init(&candidates);
    OdStatus discovery_status = od_discover_project(project_root, &candidates, &error);
    if (discovery_status != OD_OK) {
        od_candidate_list_free(&candidates);
        return false;
    }
    int height = getmaxy(window);
    size_t page_size = height > 14 ? (size_t)(height - 12) : 1U;
    OdOnboarding onboarding;
    if (od_onboarding_init(&onboarding, &candidates, page_size,
                           1024U, 65535U, &error) != OD_OK) {
        od_candidate_list_free(&candidates);
        return false;
    }
    od_candidate_list_free(&candidates);
    char status[256] = "Review every candidate before continuing.";
    bool finished = false;
    bool accepted = false;
    while (!finished) {
        int width = getmaxx(window);
        height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) break;
        onboarding.page_size = height > 14 ? (size_t)(height - 12) : 1U;
        od_render_onboarding(canvas, &onboarding, ascii, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == 27 || input == 'q') {
            finished = true;
        } else if (input == KEY_UP || input == 'k') {
            od_onboarding_move(&onboarding, -1);
        } else if (input == KEY_DOWN || input == 'j') {
            od_onboarding_move(&onboarding, 1);
        } else if (input == KEY_PPAGE) {
            od_onboarding_move_page(&onboarding, -1);
        } else if (input == KEY_NPAGE) {
            od_onboarding_move_page(&onboarding, 1);
        } else if (input == KEY_HOME) {
            onboarding.selected = 0U;
        } else if (input == KEY_END && onboarding.candidates.count > 0U) {
            onboarding.selected = onboarding.candidates.count - 1U;
        } else if (input == ' ') {
            od_onboarding_toggle_selected(&onboarding);
            (void)snprintf(status, sizeof(status), "Selection changed; press Enter to mark this row reviewed.");
        } else if (input == '\n' || input == '\r') {
            od_onboarding_review_selected(&onboarding);
            (void)snprintf(status, sizeof(status), "Candidate reviewed.");
        } else if (input == 'a') {
            (void)prompt_candidate(window, canvas, use_color, ascii, &onboarding,
                                   false, status, sizeof(status));
        } else if (input == 'e') {
            if (onboarding.candidates.count == 0U) {
                (void)snprintf(status, sizeof(status), "There is no candidate to edit; press a to add one.");
            } else {
                (void)prompt_candidate(window, canvas, use_color, ascii, &onboarding,
                                       true, status, sizeof(status));
            }
        } else if (input == 's') {
            if (!od_onboarding_all_reviewed(&onboarding)) {
                (void)snprintf(status, sizeof(status),
                               "Review every candidate before continuing.");
            } else {
                char name[128];
                project_display_name(project_root, name, sizeof(name));
                OdStatus profile_status = od_onboarding_build_profile(
                    &onboarding, name, ".ports.env", profile, &error);
                if (profile_status == OD_OK) {
                    accepted = true;
                    finished = true;
                } else {
                    (void)snprintf(status, sizeof(status), "%s", error.message);
                }
            }
        }
    }
    od_onboarding_free(&onboarding);
    return accepted;
}

static bool create_dashboard(const OdProfile *profile,
                             const OdScanSnapshot *snapshot,
                             OdDashboard *dashboard,
                             OdAllocationPlan *plan,
                             OdError *error) {
    size_t occupied_count = snapshot->endpoint_count + snapshot->docker_mapping_count;
    OdOccupiedPort *occupied = NULL;
    if (occupied_count > 0U) {
        occupied = calloc(occupied_count, sizeof(*occupied));
        if (occupied == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to prepare occupied ports");
            return false;
        }
    }
    size_t output = 0U;
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        occupied[output++] = (OdOccupiedPort){
            snapshot->endpoints[index].local_port,
            snapshot->endpoints[index].protocol
        };
    }
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        occupied[output++] = (OdOccupiedPort){
            snapshot->docker_mappings[index].host_port,
            snapshot->docker_mappings[index].protocol
        };
    }
    OdStatus allocation_status = od_allocate(profile, occupied, occupied_count,
                                              NULL, plan, error);
    free(occupied);
    if (allocation_status != OD_OK) return false;
    OdStatus dashboard_status = od_dashboard_init(dashboard, profile, snapshot, plan, error);
    if (dashboard_status != OD_OK) {
        od_allocation_plan_free(plan);
        return false;
    }
    return true;
}

static void select_mouse_target(OdDashboard *dashboard, const OdHitRegion *hit) {
    dashboard->focused = hit->widget;
    if (hit->action == OD_HIT_TOGGLE_EXPAND) {
        od_dashboard_toggle_expand(dashboard);
    } else if (hit->action == OD_HIT_SELECT_ROW) {
        switch (hit->widget) {
            case OD_WIDGET_SERVICES:
                if (hit->target < dashboard->visible_count) {
                    dashboard->selected_visible = hit->target;
                    od_dashboard_move(dashboard, 0);
                }
                break;
            case OD_WIDGET_CONFLICTS:
                dashboard->conflict_selected = hit->target;
                od_dashboard_move_focused(dashboard, 0);
                break;
            case OD_WIDGET_LISTENERS:
                dashboard->listener_selected = hit->target;
                od_dashboard_move_focused(dashboard, 0);
                break;
            case OD_WIDGET_DOCKER:
                dashboard->docker_selected = hit->target;
                od_dashboard_move_focused(dashboard, 0);
                break;
            case OD_WIDGET_COUNT:
                break;
        }
    } else if (hit->action == OD_HIT_SORT_COLUMN &&
               hit->widget == OD_WIDGET_SERVICES) {
        od_dashboard_sort(dashboard, dashboard->sort);
    }
}

static void run_dashboard(WINDOW *window,
                          OdCanvas *canvas,
                          bool use_color,
                          bool ascii,
                          const OdProfile *profile,
                          const OdScanSnapshot *snapshot) {
    OdDashboard dashboard;
    OdAllocationPlan plan = {0};
    OdError error;
    if (!create_dashboard(profile, snapshot, &dashboard, &plan, &error)) return;
    OdHitMap hit_map;
    od_hitmap_init(&hit_map);
    char status[256];
    (void)snprintf(status, sizeof(status), "Live scan ready • %zu warning(s)",
                   snapshot->warning_count);
    bool done = false;
    while (!done) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) break;
        od_render_dashboard(canvas, &dashboard, ascii, &hit_map, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == 27 || input == 'q' || input == 'Q') {
            done = true;
        } else if (input == KEY_UP || input == 'k') {
            od_dashboard_move_focused(&dashboard, -1);
        } else if (input == KEY_DOWN || input == 'j') {
            od_dashboard_move_focused(&dashboard, 1);
        } else if (input == KEY_PPAGE) {
            od_dashboard_move_focused_page(&dashboard, -1);
        } else if (input == KEY_NPAGE) {
            od_dashboard_move_focused_page(&dashboard, 1);
        } else if (input == KEY_HOME) {
            od_dashboard_home_focused(&dashboard);
        } else if (input == KEY_END) {
            od_dashboard_end_focused(&dashboard);
        } else if (input == '\t' || input == KEY_RIGHT || input == 'l') {
            od_dashboard_focus_next(&dashboard, 1);
        } else if (input == KEY_LEFT || input == 'h') {
            od_dashboard_focus_next(&dashboard, -1);
        } else if (input == 'e' || input == 'E' || input == '\n' || input == '\r') {
            od_dashboard_toggle_expand(&dashboard);
        } else if (input == '/') {
            char query[128];
            (void)snprintf(query, sizeof(query), "%s", dashboard.search);
            if (prompt_text(window, canvas, use_color, ascii,
                            "Search services", "Name, group, variable, or status",
                            query, sizeof(query))) {
                if (od_dashboard_search(&dashboard, query, &error) == OD_OK) {
                    (void)snprintf(status, sizeof(status),
                                   query[0] == '\0' ? "Search cleared." :
                                                      "Search applied: %.200s",
                                   query);
                } else {
                    (void)snprintf(status, sizeof(status), "%s", error.message);
                }
            }
        } else if (input == 's') {
            OdServiceSort next = (OdServiceSort)(((unsigned)dashboard.sort + 1U) % 5U);
            od_dashboard_sort(&dashboard, next);
            (void)snprintf(status, sizeof(status), "Services sorted by column %u.",
                           (unsigned)next + 1U);
        } else if (input == 'S') {
            od_dashboard_sort(&dashboard, dashboard.sort);
            (void)snprintf(status, sizeof(status), "Service sort direction reversed.");
        } else if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    od_dashboard_move_focused(&dashboard, -3);
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    od_dashboard_move_focused(&dashboard, 3);
                } else if ((event.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) != 0U) {
                    const OdHitRegion *hit = od_hitmap_at(&hit_map, event.x, event.y);
                    if (hit != NULL) select_mouse_target(&dashboard, hit);
                }
            }
        } else if (input == 'r' || input == 'R') {
            (void)snprintf(status, sizeof(status),
                           "Return to the main menu to start a fresh scan.");
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    od_hitmap_free(&hit_map);
    od_dashboard_free(&dashboard);
    od_allocation_plan_free(&plan);
}

int od_tui_run(const OpendoorOptions *options) {
    (void)setlocale(LC_ALL, "");
    StartupScan scan = {0};
    od_scan_snapshot_init(&scan.snapshot, 1U);
    atomic_init(&scan.stage, OD_LOAD_PROJECT_FILES);
    atomic_init(&scan.done, false);
    atomic_init(&scan.warning_count, 0U);
    if (pthread_create(&scan.thread, NULL, startup_scan_main, &scan) != 0) {
        od_scan_snapshot_free(&scan.snapshot);
        return 5;
    }

    WINDOW *window = initscr();
    if (window == NULL) {
        (void)pthread_join(scan.thread, NULL);
        od_scan_snapshot_free(&scan.snapshot);
        return 5;
    }
    (void)noecho();
    (void)cbreak();
    (void)curs_set(0);
    (void)keypad(window, true);
    (void)mousemask(ALL_MOUSE_EVENTS, NULL);
    wtimeout(window, 80);
    const OdTheme *theme = od_theme_by_name("midnight");
    bool use_color = initialize_colors(theme, options->no_color);
    OdCanvas canvas = {0};
    OdError canvas_error;
    uint64_t started = monotonic_milliseconds();
    unsigned frame = 0U;
    bool animation_skipped = false;
    while (!animation_skipped) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(&canvas, width, height, &canvas_error)) {
            (void)endwin();
            (void)pthread_join(scan.thread, NULL);
            od_scan_snapshot_free(&scan.snapshot);
            return 5;
        }
        uint64_t elapsed = monotonic_milliseconds() - started;
        if (width < 60 || height < 18) {
            od_render_resize_required(&canvas);
        } else {
            od_render_loading(&canvas,
                              (OdLoadingStage)atomic_load(&scan.stage),
                              frame++,
                              elapsed > UINT32_MAX ? UINT32_MAX : (unsigned)elapsed,
                              options->reduced_motion,
                              options->force_ascii,
                              atomic_load(&scan.warning_count));
        }
        paint_canvas(window, &canvas, use_color);
        int input = wgetch(window);
        if (input == 27) animation_skipped = true;
        if (atomic_load(&scan.done) && elapsed >= 350U) break;
    }

    bool configured = profile_exists(options);
    OdProfile session_profile;
    od_profile_init(&session_profile);
    bool session_profile_ready = false;
    char profile_error[256] = {0};
    if (configured) {
        char default_path[4096];
        const char *path = resolved_profile_path(options, default_path, sizeof(default_path));
        OdError load_error;
        if (path != NULL && od_profile_load(path, &session_profile, &load_error) == OD_OK) {
            session_profile_ready = true;
        } else {
            configured = false;
            (void)snprintf(profile_error, sizeof(profile_error), "%s",
                           path == NULL ? "Profile path is too long." : load_error.message);
        }
    }
    bool scan_joined = false;
    size_t selected = 0U;
    bool quit = false;
    char status[256];
    menu_status(status, sizeof(status), configured, selected);
    if (profile_error[0] != '\0') {
        (void)snprintf(status, sizeof(status), "Profile not loaded: %.220s", profile_error);
    }
    while (!quit) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(&canvas, width, height, &canvas_error)) {
            quit = true;
            continue;
        }
        if (width < 60 || height < 18) {
            od_render_resize_required(&canvas);
        } else {
            if (!atomic_load(&scan.done)) {
                (void)snprintf(status, sizeof(status),
                               "Scan continues in the background • %zu warning(s)",
                               atomic_load(&scan.warning_count));
            }
            OdMenuView view = {project_label(options), configured, selected, status};
            od_render_main_menu(&canvas, &view, options->force_ascii);
        }
        paint_canvas(window, &canvas, use_color);
        int input = wgetch(window);
        size_t count = menu_item_count(configured);
        if (input == 'q' || input == 'Q' || input == 27) {
            quit = true;
        } else if (input == KEY_UP || input == 'k' || input == 'h') {
            selected = selected == 0U ? count - 1U : selected - 1U;
            menu_status(status, sizeof(status), configured, selected);
        } else if (input == KEY_DOWN || input == 'j' || input == 'l') {
            selected = (selected + 1U) % count;
            menu_status(status, sizeof(status), configured, selected);
        } else if (input == '\n' || input == '\r') {
            if (selected == count - 1U) {
                quit = true;
            } else if (!configured && selected == 0U) {
                const char *root = options->project_path == NULL ? "." : options->project_path;
                if (run_onboarding(window, &canvas, use_color, options->force_ascii,
                                   root, &session_profile)) {
                    configured = true;
                    session_profile_ready = true;
                    selected = 0U;
                    (void)snprintf(status, sizeof(status),
                                   "Candidate review complete • profile ready for conflict resolution.");
                } else {
                    (void)snprintf(status, sizeof(status),
                                   "Discovery review cancelled; no files were changed.");
                }
            } else if (configured && selected == 0U) {
                if (!session_profile_ready) {
                    (void)snprintf(status, sizeof(status),
                                   "No valid project profile is available.");
                } else if (!atomic_load(&scan.done)) {
                    (void)snprintf(status, sizeof(status),
                                   "The startup scan is still running; try again in a moment.");
                } else {
                    if (!scan_joined) {
                        (void)pthread_join(scan.thread, NULL);
                        scan_joined = true;
                    }
                    run_dashboard(window, &canvas, use_color, options->force_ascii,
                                  &session_profile, &scan.snapshot);
                    menu_status(status, sizeof(status), configured, selected);
                }
            } else {
                menu_status(status, sizeof(status), configured, selected);
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    od_canvas_free(&canvas);
    (void)endwin();
    if (!scan_joined) (void)pthread_join(scan.thread, NULL);
    od_scan_snapshot_free(&scan.snapshot);
    if (session_profile_ready) od_profile_free(&session_profile);
    return 0;
}
