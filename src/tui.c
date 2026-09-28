#include "opendoor/tui.h"

#include "curses_compat.h"
#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/dashboard.h"
#include "opendoor/discovery.h"
#include "opendoor/persistence.h"
#include "opendoor/resolution.h"
#include "opendoor/scan.h"
#include "opendoor/screens.h"
#include "opendoor/ui.h"

#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;
static WINDOW *terminal_window = NULL;
static bool terminal_uses_color = false;

enum {
    OD_COLOR_BLACK = 0,
    OD_COLOR_RED = 1,
    OD_COLOR_GREEN = 2,
    OD_COLOR_YELLOW = 3,
    OD_COLOR_CYAN = 6,
    OD_COLOR_WHITE = 7
};

static void handle_signal(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static void set_status(char *status, size_t capacity, const char *format, ...) {
    if (status == NULL || capacity == 0U) return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(status, capacity, format, arguments);
    va_end(arguments);
}

static int role_attributes(OdStyleRole role, unsigned attributes) {
    int result = terminal_uses_color ? (int)COLOR_PAIR((int)role + 1) : 0;
    if ((attributes & 1U) != 0U) result |= A_BOLD;
    if ((attributes & 2U) != 0U) result |= A_REVERSE;
    if (!terminal_uses_color && role == OD_ROLE_SELECTED) result |= A_REVERSE;
    return result;
}

static bool initialize_colors(void) {
    static const short foreground[OD_ROLE_COUNT] = {
        OD_COLOR_WHITE,
        OD_COLOR_CYAN,
        OD_COLOR_GREEN,
        OD_COLOR_YELLOW,
        OD_COLOR_RED,
        OD_COLOR_WHITE,
        OD_COLOR_BLACK,
        OD_COLOR_CYAN,
        OD_COLOR_WHITE
    };
    static const short background[OD_ROLE_COUNT] = {
        -1, -1, -1, -1, -1, -1, OD_COLOR_CYAN, -1, -1
    };
    if (!has_colors() || start_color() == ERR) return false;
    (void)use_default_colors();
    for (size_t role = 0U; role < OD_ROLE_COUNT; ++role) {
        if (init_pair((short)(role + 1U), foreground[role],
                      background[role]) == ERR) return false;
    }
    return true;
}

static void present_canvas(const OdCanvas *canvas) {
    (void)werase(terminal_window);
    for (size_t y = 0U; y < canvas->height; ++y) {
        for (size_t x = 0U; x < canvas->width; ++x) {
            const OdCell *cell = &canvas->cells[y * canvas->width + x];
            if (cell->glyph[0] == '\0') continue;
            (void)wattrset(terminal_window, role_attributes(cell->role, cell->attributes));
            (void)wmove(terminal_window, (int)y, (int)x);
            (void)waddnstr(terminal_window, cell->glyph, (int)strlen(cell->glyph));
        }
    }
    (void)wattrset(terminal_window, 0);
    (void)wnoutrefresh(terminal_window);
    (void)doupdate();
}

static bool draw(void (*renderer)(OdCanvas *, void *), void *context) {
    int width = getmaxx(terminal_window);
    int height = getmaxy(terminal_window);
    if (width <= 0 || height <= 0) return false;
    OdCanvas canvas;
    OdError error;
    if (od_canvas_init(&canvas, (size_t)width, (size_t)height, &error) != OD_OK) {
        return false;
    }
    renderer(&canvas, context);
    present_canvas(&canvas);
    od_canvas_free(&canvas);
    return true;
}

typedef struct {
    size_t selected;
    const char *project;
    const char *status;
    bool ascii;
} MenuRenderContext;

static void render_menu(OdCanvas *canvas, void *opaque) {
    const MenuRenderContext *context = opaque;
    od_render_main_menu(canvas, context->selected, context->project,
                        context->status, context->ascii);
}

static OdMenuItem run_menu(const char *project, bool ascii, const char *status) {
    size_t selected = 0U;
    while (!interrupted) {
        MenuRenderContext context = {selected, project, status, ascii};
        (void)draw(render_menu, &context);
        int key = wgetch(terminal_window);
        if (key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    key = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    key = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U) {
                    OdMouseTarget target = od_menu_mouse_target(
                        (size_t)getmaxx(terminal_window),
                        (size_t)getmaxy(terminal_window), ascii,
                        event.x, event.y);
                    if (target.action == OD_MOUSE_MENU_ITEM) {
                        return od_menu_dispatch(target.item);
                    }
                }
            }
        }
        if (key == KEY_UP) {
            selected = selected == 0U ? (size_t)OD_MENU_COUNT - 1U : selected - 1U;
        } else if (key == KEY_DOWN) {
            selected = (selected + 1U) % (size_t)OD_MENU_COUNT;
        } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            return od_menu_dispatch(selected);
        } else if (key == 'q' || key == 27) {
            return OD_MENU_QUIT;
        }
    }
    return OD_MENU_QUIT;
}

static OdStatus config_path_for_project(const char *project,
                                        char *path,
                                        size_t capacity,
                                        OdError *error) {
    size_t length = strlen(project);
    int written = snprintf(path, capacity, "%s%s.ports.env", project,
                           length > 0U && project[length - 1U] == '/' ? "" : "/");
    if (written < 0 || (size_t)written >= capacity) {
        od_error_set(error, OD_ERROR_INVALID, "wanted-ports path is too long");
        return OD_ERROR_INVALID;
    }
    od_error_clear(error);
    return OD_OK;
}

static OdStatus load_wanted(const char *path,
                            OdAssignments *wanted,
                            bool *missing,
                            OdError *error) {
    od_assignments_init(wanted);
    *missing = false;
    if (access(path, F_OK) != 0) {
        if (errno == ENOENT) {
            *missing = true;
            od_error_clear(error);
            return OD_OK;
        }
        od_error_set(error, OD_ERROR_IO, "unable to inspect %s: %s",
                     path, strerror(errno));
        return OD_ERROR_IO;
    }
    return od_config_load(path, wanted, error);
}

typedef struct {
    OdAssignments wanted;
    uint16_t *occupied;
    size_t occupied_count;
    OdDashboard dashboard;
    char status[OD_ERROR_MESSAGE_CAP];
} DashboardState;

static void dashboard_state_free(DashboardState *state) {
    od_dashboard_free(&state->dashboard);
    free(state->occupied);
    od_assignments_free(&state->wanted);
    *state = (DashboardState){0};
}

static OdStatus refresh_dashboard(DashboardState *state,
                                  const char *config_path,
                                  uint64_t generation,
                                  OdError *error) {
    dashboard_state_free(state);
    bool missing = false;
    OdStatus config_status = load_wanted(config_path, &state->wanted, &missing, error);
    char config_error[OD_ERROR_MESSAGE_CAP] = {0};
    if (config_status != OD_OK) {
        set_status(config_error, sizeof(config_error), "%s", error->message);
        od_assignments_init(&state->wanted);
    }
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, generation);
    OdStatus status = od_scan_host(&snapshot, error);
    if (status == OD_OK) {
        status = od_dashboard_init(&state->dashboard, &state->wanted,
                                   &snapshot, error);
    }
    od_scan_snapshot_free(&snapshot);
    if (status != OD_OK) return status;
    if (config_status != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", config_error);
    } else if (missing) {
        set_status(state->status, sizeof(state->status),
                   "Wanted-ports file not found; showing occupied ports only");
    } else {
        set_status(state->status, sizeof(state->status), "Scan complete");
    }
    return OD_OK;
}

typedef struct {
    DashboardState *state;
    const char *config_path;
    bool ascii;
} DashboardRenderContext;

static void render_dashboard(OdCanvas *canvas, void *opaque) {
    DashboardRenderContext *context = opaque;
    size_t page_size = canvas->height > 16U ? (canvas->height - 16U) / 2U : 1U;
    if (page_size == 0U) page_size = 1U;
    od_dashboard_set_page_size(&context->state->dashboard, page_size);
    od_render_dashboard(canvas, &context->state->dashboard, context->config_path,
                        context->state->status, context->ascii);
}

static void run_dashboard(const char *config_path, bool ascii) {
    DashboardState state = {0};
    OdError error;
    uint64_t generation = 1U;
    if (refresh_dashboard(&state, config_path, generation, &error) != OD_OK) {
        set_status(state.status, sizeof(state.status), "%s", error.message);
    }
    while (!interrupted) {
        DashboardRenderContext context = {&state, config_path, ascii};
        (void)draw(render_dashboard, &context);
        int key = wgetch(terminal_window);
        if (key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    od_dashboard_scroll(&state.dashboard, -3);
                    continue;
                }
                if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    od_dashboard_scroll(&state.dashboard, 3);
                    continue;
                }
                if ((event.bstate & BUTTON1_CLICKED) != 0U) {
                    OdMouseTarget target = od_dashboard_mouse_target(
                        (size_t)getmaxx(terminal_window),
                        (size_t)getmaxy(terminal_window), event.x, event.y);
                    if (target.action == OD_MOUSE_REFRESH) key = 'r';
                    if (target.action == OD_MOUSE_BACK) key = 'q';
                }
            }
        }
        if (key == KEY_UP) {
            od_dashboard_scroll(&state.dashboard, -1);
        } else if (key == KEY_DOWN) {
            od_dashboard_scroll(&state.dashboard, 1);
        } else if (key == 'r') {
            ++generation;
            if (refresh_dashboard(&state, config_path, generation, &error) != OD_OK) {
                set_status(state.status, sizeof(state.status), "%s", error.message);
            }
        } else if (key == 'q' || key == 27) {
            break;
        }
    }
    dashboard_state_free(&state);
}

typedef struct {
    OdAssignments wanted;
    uint16_t *occupied;
    size_t occupied_count;
    OdAllocationPlan plan;
    OdResolution resolution;
    size_t scroll;
    bool apply_available;
    bool config_valid;
    bool prepared;
    char status[OD_ERROR_MESSAGE_CAP];
} ConflictState;

static void conflict_state_free(ConflictState *state) {
    od_resolution_free(&state->resolution);
    od_allocation_plan_free(&state->plan);
    free(state->occupied);
    od_assignments_free(&state->wanted);
    *state = (ConflictState){0};
}

static OdStatus prepare_conflicts(ConflictState *state,
                                  const char *config_path,
                                  OdError *error) {
    conflict_state_free(state);
    bool missing = false;
    OdStatus status = load_wanted(config_path, &state->wanted, &missing, error);
    if (status != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", error->message);
        return status;
    }
    state->config_valid = true;
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, 1U);
    status = od_scan_sockets(&snapshot, error);
    if (status == OD_OK) {
        status = od_discover_occupied_ports(&snapshot, &state->occupied,
                                            &state->occupied_count, error);
    }
    od_scan_snapshot_free(&snapshot);
    if (status == OD_OK) {
        status = od_allocate(&state->wanted, state->occupied,
                             state->occupied_count, &state->plan, error);
    }
    if (status == OD_OK) {
        status = od_resolution_init(&state->resolution, &state->plan, error);
    }
    if (status != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", error->message);
        return status;
    }
    state->prepared = true;
    state->apply_available = state->resolution.count > 0U;
    if (missing) {
        set_status(state->status, sizeof(state->status),
                   "Wanted-ports file not found; no configured ports to resolve");
    } else if (state->resolution.count == 0U) {
        set_status(state->status, sizeof(state->status), "No conflicts found");
    } else {
        set_status(state->status, sizeof(state->status),
                   "Press Enter once to apply the complete proposal");
    }
    return OD_OK;
}

typedef struct {
    ConflictState *state;
    bool ascii;
} ConflictRenderContext;

static void render_conflict_screen(OdCanvas *canvas, void *opaque) {
    ConflictRenderContext *context = opaque;
    od_render_conflicts(canvas, &context->state->resolution,
                        context->state->scroll,
                        context->state->apply_available,
                        context->state->prepared,
                        context->state->status,
                        context->ascii);
}

static void apply_proposal(ConflictState *state,
                           const char *config_path,
                           OdError *error) {
    if (!state->apply_available || !state->config_valid) return;
    for (size_t index = 0U; index < state->wanted.count; ++index) {
        state->wanted.items[index].port = state->plan.items[index].new_port;
    }
    if (od_config_write(config_path, &state->wanted, error) != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", error->message);
        return;
    }
    state->apply_available = false;
    set_status(state->status, sizeof(state->status), "Updated %zu port(s)",
               state->resolution.count);
}

static void run_conflicts(const char *config_path, bool ascii) {
    ConflictState state = {0};
    OdError error;
    (void)prepare_conflicts(&state, config_path, &error);
    while (!interrupted) {
        ConflictRenderContext context = {&state, ascii};
        (void)draw(render_conflict_screen, &context);
        int key = wgetch(terminal_window);
        int terminal_height = getmaxy(terminal_window);
        size_t page_size = terminal_height > 14 ?
            (size_t)(terminal_height - 14) / 2U : 1U;
        if (page_size == 0U) page_size = 1U;
        size_t maximum = state.resolution.count > page_size ?
            state.resolution.count - page_size : 0U;
        if (state.scroll > maximum) state.scroll = maximum;
        if (key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    state.scroll = state.scroll > 3U ? state.scroll - 3U : 0U;
                    continue;
                }
                if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    size_t remaining = maximum - state.scroll;
                    state.scroll += remaining > 3U ? 3U : remaining;
                    continue;
                }
                if ((event.bstate & BUTTON1_CLICKED) != 0U) {
                    OdMouseTarget target = od_conflicts_mouse_target(
                        (size_t)getmaxx(terminal_window),
                        (size_t)getmaxy(terminal_window),
                        state.apply_available, event.x, event.y);
                    if (target.action == OD_MOUSE_APPLY_ALL) key = '\n';
                    if (target.action == OD_MOUSE_BACK) key = 'q';
                }
            }
        }
        if (key == KEY_UP) {
            if (state.scroll > 0U) --state.scroll;
        } else if (key == KEY_DOWN) {
            if (state.scroll < maximum) ++state.scroll;
        } else if ((key == '\n' || key == '\r' || key == KEY_ENTER) &&
                   getmaxx(terminal_window) >= 60 &&
                   getmaxy(terminal_window) >= 18) {
            apply_proposal(&state, config_path, &error);
        } else if (key == 'q' || key == 27) {
            break;
        }
    }
    conflict_state_free(&state);
}

int od_tui_run(const OpendoorOptions *options) {
    const char *project = options->project_path == NULL ? "." : options->project_path;
    char config_path[4096];
    OdError error;
    if (config_path_for_project(project, config_path, sizeof(config_path), &error) != OD_OK) {
        fprintf(stderr, "opendoor: %s\n", error.message);
        return 3;
    }
    (void)setlocale(LC_ALL, "");
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    terminal_window = initscr();
    if (terminal_window == NULL) {
        fputs("opendoor: unable to initialize terminal\n", stderr);
        return 4;
    }
    (void)cbreak();
    (void)noecho();
    (void)keypad(terminal_window, true);
    (void)curs_set(0);
    (void)set_escdelay(25);
    terminal_uses_color = initialize_colors();
    (void)mousemask(ALL_MOUSE_EVENTS, NULL);
    wtimeout(terminal_window, -1);

    bool running = true;
    char menu_status[OD_ERROR_MESSAGE_CAP] = "Run before starting project services";
    while (running && !interrupted) {
        OdMenuItem item = run_menu(project, options->force_ascii, menu_status);
        switch (item) {
            case OD_MENU_DASHBOARD:
                run_dashboard(config_path, options->force_ascii);
                set_status(menu_status, sizeof(menu_status), "Returned from Dashboard");
                break;
            case OD_MENU_RESOLVE_CONFLICTS:
                run_conflicts(config_path, options->force_ascii);
                set_status(menu_status, sizeof(menu_status),
                           "Returned from Resolve conflicts");
                break;
            case OD_MENU_QUIT:
            case OD_MENU_COUNT:
                running = false;
                break;
        }
    }
    (void)endwin();
    terminal_window = NULL;
    terminal_uses_color = false;
    return 0;
}
