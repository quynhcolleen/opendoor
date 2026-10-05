#include "opendoor/tui.h"

#include "curses_compat.h"
#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/dashboard.h"
#include "opendoor/discovery.h"
#include "opendoor/persistence.h"
#include "opendoor/history.h"
#include "opendoor/resolution.h"
#include "opendoor/scan.h"
#include "opendoor/screens.h"
#include "opendoor/ui.h"

#include <errno.h>
#include <inttypes.h>
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
static bool terminal_muted_uses_dim = true;

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
    if (role == OD_ROLE_MUTED && terminal_muted_uses_dim) result |= A_DIM;
    if (!terminal_uses_color && role == OD_ROLE_SELECTED) result |= A_REVERSE;
    return result;
}

static bool initialize_colors(void) {
    short foreground[OD_ROLE_COUNT] = {
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
    terminal_muted_uses_dim = true;
    if (!has_colors() || start_color() == ERR) return false;
    (void)use_default_colors();
    if (COLORS >= 256) {
        foreground[OD_ROLE_MUTED] = 245;
        terminal_muted_uses_dim = false;
    } else if (COLORS >= 9) {
        foreground[OD_ROLE_MUTED] = 8;
        terminal_muted_uses_dim = false;
    }
    for (size_t role = 0U; role < OD_ROLE_COUNT; ++role) {
        if (init_pair((short)(role + 1U), foreground[role],
                      background[role]) == ERR) {
            terminal_muted_uses_dim = true;
            return false;
        }
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

typedef struct {
    OdProjectDiscovery discovery;
    OdDashboard dashboard;
    char status[OD_ERROR_MESSAGE_CAP];
} DashboardState;

static void dashboard_state_free(DashboardState *state) {
    od_dashboard_free(&state->dashboard);
    od_project_discovery_free(&state->discovery);
    *state = (DashboardState){0};
}

static OdStatus refresh_dashboard(DashboardState *state,
                                  const char *project_root,
                                  uint64_t generation,
                                  OdError *error) {
    dashboard_state_free(state);
    OdStatus status = od_discover_project_ports(project_root, &state->discovery,
                                                error);
    if (status != OD_OK) return status;
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, generation);
    status = od_scan_host(&snapshot, error);
    if (status == OD_OK) {
        status = od_dashboard_init(&state->dashboard, project_root,
                                   &state->discovery, &snapshot, error);
    }
    od_scan_snapshot_free(&snapshot);
    if (status != OD_OK) return status;
    if (state->discovery.warning_count > 0U) {
        set_status(state->status, sizeof(state->status),
                   "Scan complete with %zu warning(s)",
                   state->discovery.warning_count);
    } else {
        set_status(state->status, sizeof(state->status), "Scan complete");
    }
    return OD_OK;
}

typedef struct {
    DashboardState *state;
    bool ascii;
} DashboardRenderContext;

static void render_dashboard(OdCanvas *canvas, void *opaque) {
    DashboardRenderContext *context = opaque;
    size_t page_size = canvas->height > 16U ? (canvas->height - 16U) / 2U : 1U;
    if (page_size == 0U) page_size = 1U;
    od_dashboard_set_page_size(&context->state->dashboard, page_size);
    od_render_dashboard(canvas, &context->state->dashboard,
                        context->state->status, context->ascii);
}

static void run_dashboard(const char *project_root, bool ascii) {
    DashboardState state = {0};
    OdError error;
    uint64_t generation = 1U;
    if (refresh_dashboard(&state, project_root, generation, &error) != OD_OK) {
        set_status(state.status, sizeof(state.status), "%s", error.message);
    }
    while (!interrupted) {
        DashboardRenderContext context = {&state, ascii};
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
            if (refresh_dashboard(&state, project_root, generation, &error) != OD_OK) {
                set_status(state.status, sizeof(state.status), "%s", error.message);
            }
        } else if (key == 'q' || key == 27) {
            break;
        }
    }
    dashboard_state_free(&state);
}

typedef struct {
    OdProjectDiscovery discovery;
    OdResolution resolution;
    size_t scroll;
    bool apply_available;
    bool prepared;
    char status[OD_ERROR_MESSAGE_CAP];
} ConflictState;

static void conflict_state_free(ConflictState *state) {
    od_resolution_free(&state->resolution);
    od_project_discovery_free(&state->discovery);
    *state = (ConflictState){0};
}

static OdStatus prepare_conflicts(ConflictState *state,
                                  const char *project_root,
                                  OdError *error) {
    conflict_state_free(state);
    OdStatus status = od_discover_project_ports(project_root, &state->discovery,
                                                error);
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, 1U);
    if (status == OD_OK) status = od_scan_host(&snapshot, error);
    if (status == OD_OK) status = od_resolution_build(
        project_root, &state->discovery, &snapshot, &state->resolution, error);
    od_scan_snapshot_free(&snapshot);
    if (status != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", error->message);
        return status;
    }
    state->prepared = true;
    state->apply_available = state->resolution.automatic_count > 0U;
    if (state->resolution.count == 0U) {
        set_status(state->status, sizeof(state->status), "No conflicts found");
    } else if (!state->apply_available) {
        set_status(state->status, sizeof(state->status),
                   "%zu manual suggestion(s); nothing will be written",
                   state->resolution.manual_count);
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
                           const char *project_root,
                           OdError *error) {
    if (!state->apply_available) return;
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, 2U);
    OdStatus status = od_scan_host(&snapshot, error);
    if (status == OD_OK) {
        status = od_resolution_validate_snapshot(project_root, &state->resolution,
                                                 &snapshot, error);
    }
    od_scan_snapshot_free(&snapshot);
    size_t updated = 0U;
    if (status == OD_OK) {
        status = od_history_apply_resolution(&state->resolution, OD_HISTORY_APPLY,
                                              NULL, &updated, error);
    }
    if (status != OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", error->message);
        return;
    }
    state->apply_available = false;
    if (state->resolution.manual_count > 0U) {
        set_status(state->status, sizeof(state->status),
                   "Updated %zu port(s); %zu manual suggestion(s)",
                   updated, state->resolution.manual_count);
    } else {
        set_status(state->status, sizeof(state->status), "Updated %zu port(s)",
                   updated);
    }
}

static void run_conflicts(const char *project_root, bool ascii) {
    ConflictState state = {0};
    OdError error;
    (void)prepare_conflicts(&state, project_root, &error);
    while (!interrupted) {
        ConflictRenderContext context = {&state, ascii};
        (void)draw(render_conflict_screen, &context);
        int key = wgetch(terminal_window);
        int terminal_height = getmaxy(terminal_window);
        size_t page_size = terminal_height > 12 ?
            (size_t)(terminal_height - 12) / 2U : 1U;
        if (page_size == 0U) page_size = 1U;
        size_t visual_count = od_resolution_visual_line_count(&state.resolution);
        size_t maximum = visual_count > page_size ?
            visual_count - page_size : 0U;
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
            apply_proposal(&state, project_root, &error);
        } else if (key == 'q' || key == 27) {
            break;
        }
    }
    conflict_state_free(&state);
}

typedef struct {
    OdHistory history;
    size_t selected;
    size_t scroll;
    bool confirming;
    uint64_t pending_id;
    char status[OD_ERROR_MESSAGE_CAP * 2U];
} HistoryState;

static size_t history_index_for_id(const OdHistory *history, uint64_t id) {
    for (size_t index = 0U; index < history->count; ++index) {
        if (history->items[index].id == id) return index;
    }
    return history->count;
}

static OdStatus reload_history(HistoryState *state, const char *project_root,
                                OdError *error) {
    uint64_t selected_id = state->selected < state->history.count ?
        state->history.items[state->selected].id : 0U;
    OdHistory loaded = {0};
    OdStatus result = od_history_load(project_root, &loaded, error);
    if (result == OD_OK) result = od_history_refresh(project_root, &loaded, error);
    if (result != OD_OK) {
        /* A partially classified refresh must not leave any write target. */
        for (size_t index = 0U; index < loaded.count; ++index) {
            loaded.items[index].availability = OD_HISTORY_UNCHECKED;
        }
    }
    od_history_free(&state->history);
    state->history = loaded;
    state->selected = history_index_for_id(&loaded, selected_id);
    if (state->selected == loaded.count) state->selected = 0U;
    if (result == OD_OK) {
        set_status(state->status, sizeof(state->status),
                   "Loaded %zu change(s), newest first", loaded.count);
    } else {
        set_status(state->status, sizeof(state->status), "%s", error->message);
    }
    return result;
}

static void begin_history_confirmation(HistoryState *state, const char *project_root) {
    if (state->selected >= state->history.count ||
        state->history.items[state->selected].availability != OD_HISTORY_READY) return;
    uint64_t id = state->history.items[state->selected].id;
    OdError error;
    if (reload_history(state, project_root, &error) != OD_OK) return;
    size_t index = history_index_for_id(&state->history, id);
    if (index == state->history.count) {
        set_status(state->status, sizeof(state->status), "Selected history entry no longer exists");
        return;
    }
    state->selected = index;
    if (state->history.items[index].availability != OD_HISTORY_READY) {
        set_status(state->status, sizeof(state->status), "Selected change is unavailable");
        return;
    }
    state->pending_id = id;
    state->confirming = true;
}

static void confirm_history_revert(HistoryState *state, const char *project_root) {
    uint64_t id = state->pending_id;
    OdError error;
    size_t updated = 0U;
    OdStatus result = od_history_revert(project_root, id, &updated, &error);
    char outcome[OD_ERROR_MESSAGE_CAP];
    if (result == OD_OK) {
        set_status(outcome, sizeof(outcome), "Reverted history #%" PRIu64 "; updated %zu port(s)",
                   id, updated);
    } else {
        set_status(outcome, sizeof(outcome), "%s", error.message);
    }
    state->confirming = false;
    state->pending_id = 0U;
    if (reload_history(state, project_root, &error) == OD_OK) {
        set_status(state->status, sizeof(state->status), "%s", outcome);
    } else {
        set_status(state->status, sizeof(state->status), "%s; refresh failed: %s",
                   outcome, error.message);
    }
}

typedef struct {
    HistoryState *state;
    bool ascii;
} HistoryRenderContext;

static void render_history_screen(OdCanvas *canvas, void *opaque) {
    HistoryRenderContext *context = opaque;
    HistoryState *state = context->state;
    if (state->confirming) {
        size_t index = history_index_for_id(&state->history, state->pending_id);
        od_render_history_confirmation(canvas, index < state->history.count ?
                                         &state->history.items[index] : NULL, context->ascii);
    } else {
        state->scroll = od_history_visible_scroll(state->history.count, state->selected,
                                                   state->scroll, canvas->height);
        od_render_history(canvas, &state->history, state->selected, state->scroll,
                            state->status, context->ascii);
    }
}

static void run_history(const char *project_root, bool ascii) {
    HistoryState state = {0};
    OdError error;
    (void)reload_history(&state, project_root, &error);
    while (!interrupted) {
        HistoryRenderContext context = {&state, ascii};
        (void)draw(render_history_screen, &context);
        int key = wgetch(terminal_window);
        int width = getmaxx(terminal_window);
        int height = getmaxy(terminal_window);
        bool usable = width >= 60 && height >= 18;
        if (key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if (!state.confirming && (event.bstate & BUTTON4_PRESSED) != 0U) {
                    state.selected = od_page_target(state.selected, state.history.count, 3U, -1);
                    continue;
                }
                if (!state.confirming && (event.bstate & BUTTON5_PRESSED) != 0U) {
                    state.selected = od_page_target(state.selected, state.history.count, 3U, 1);
                    continue;
                }
                if ((event.bstate & BUTTON1_CLICKED) != 0U && usable) {
                    OdMouseTarget target = od_history_mouse_target(
                        (size_t)width, (size_t)height, &state.history,
                        state.selected, state.scroll, state.confirming, event.x, event.y);
                    if (target.action == OD_MOUSE_HISTORY_ROW) {
                        state.selected = target.item;
                    } else if (target.action == OD_MOUSE_HISTORY_REVERT) {
                        state.selected = target.item;
                        begin_history_confirmation(&state, project_root);
                        continue;
                    } else if (target.action == OD_MOUSE_HISTORY_CONFIRM) {
                        key = 'y';
                    } else if (target.action == OD_MOUSE_REFRESH) {
                        key = 'r';
                    } else if (target.action == OD_MOUSE_BACK) {
                        key = 'q';
                    }
                }
            }
        }
        /* A pending ID stays frozen until explicit confirmation or cancel.
         * Navigation, Enter, wheel, row clicks and refresh are inert here. */
        if (state.confirming) {
            if (key == 'n' || key == 'q' || key == 27) {
                state.confirming = false;
                state.pending_id = 0U;
                set_status(state.status, sizeof(state.status), "Revert cancelled");
            } else if (key == 'y' && usable) {
                confirm_history_revert(&state, project_root);
            }
            continue;
        }
        if (key == KEY_UP) {
            state.selected = od_page_target(state.selected, state.history.count, 1U, -1);
        } else if (key == KEY_DOWN) {
            state.selected = od_page_target(state.selected, state.history.count, 1U, 1);
        } else if (key == 'r') {
            (void)reload_history(&state, project_root, &error);
        } else if ((key == '\n' || key == '\r' || key == KEY_ENTER) && usable) {
            begin_history_confirmation(&state, project_root);
        } else if (key == 'q' || key == 27) {
            break;
        }
    }
    od_history_free(&state.history);
}

int od_tui_run(const OpendoorOptions *options) {
    const char *project = options->project_path == NULL ? "." : options->project_path;
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
    bool menu_ascii = options->force_ascii || !terminal_uses_color;
    char menu_status[OD_ERROR_MESSAGE_CAP] = "Run before starting project services";
    while (running && !interrupted) {
        OdMenuItem item = run_menu(project, menu_ascii, menu_status);
        switch (item) {
            case OD_MENU_DASHBOARD:
                run_dashboard(project, options->force_ascii);
                (void)flushinp();
                set_status(menu_status, sizeof(menu_status), "Returned from Dashboard");
                break;
            case OD_MENU_RESOLVE_CONFLICTS:
                run_conflicts(project, options->force_ascii);
                set_status(menu_status, sizeof(menu_status),
                           "Returned from Resolve conflicts");
                break;
            case OD_MENU_HISTORY:
                run_history(project, options->force_ascii);
                (void)flushinp();
                set_status(menu_status, sizeof(menu_status), "Returned from History");
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
