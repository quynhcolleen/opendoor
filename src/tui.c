#include "opendoor/tui.h"

#include "curses_compat.h"
#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/dashboard.h"
#include "opendoor/docker.h"
#include "opendoor/discovery.h"
#include "opendoor/help.h"
#include "opendoor/onboarding.h"
#include "opendoor/persistence.h"
#include "opendoor/resolution.h"
#include "opendoor/screens.h"
#include "opendoor/scan.h"
#include "opendoor/settings_store.h"
#include "opendoor/theme.h"
#include "opendoor/ui.h"

#include <locale.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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

typedef struct {
    int pipe_read;
    int pipe_write;
    struct sigaction previous_interrupt;
    struct sigaction previous_terminate;
    bool installed;
} SignalState;

static volatile sig_atomic_t termination_signal = 0;
static int signal_pipe_write = -1;

static void termination_handler(int signal_number) {
    termination_signal = signal_number;
    if (signal_pipe_write >= 0) {
        unsigned char value = (unsigned char)signal_number;
        (void)write(signal_pipe_write, &value, sizeof(value));
    }
}

static bool signal_state_install(SignalState *state) {
    *state = (SignalState){.pipe_read = -1, .pipe_write = -1};
    int descriptors[2];
    if (pipe(descriptors) != 0) return false;
    state->pipe_read = descriptors[0];
    state->pipe_write = descriptors[1];
    (void)fcntl(state->pipe_read, F_SETFL, O_NONBLOCK);
    (void)fcntl(state->pipe_write, F_SETFL, O_NONBLOCK);
    (void)fcntl(state->pipe_read, F_SETFD, FD_CLOEXEC);
    (void)fcntl(state->pipe_write, F_SETFD, FD_CLOEXEC);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = termination_handler;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &state->previous_interrupt) != 0) {
        (void)close(state->pipe_read);
        (void)close(state->pipe_write);
        *state = (SignalState){.pipe_read = -1, .pipe_write = -1};
        return false;
    }
    if (sigaction(SIGTERM, &action, &state->previous_terminate) != 0) {
        (void)sigaction(SIGINT, &state->previous_interrupt, NULL);
        (void)close(state->pipe_read);
        (void)close(state->pipe_write);
        *state = (SignalState){.pipe_read = -1, .pipe_write = -1};
        return false;
    }
    termination_signal = 0;
    signal_pipe_write = state->pipe_write;
    state->installed = true;
    return true;
}

static void signal_state_restore(SignalState *state) {
    if (!state->installed) return;
    signal_pipe_write = -1;
    (void)sigaction(SIGINT, &state->previous_interrupt, NULL);
    (void)sigaction(SIGTERM, &state->previous_terminate, NULL);
    (void)close(state->pipe_read);
    (void)close(state->pipe_write);
    state->installed = false;
}

static bool termination_requested(void) {
    return termination_signal != 0;
}

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
        if (termination_requested()) return false;
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

static bool parse_port_text(const char *text, uint16_t *port);

static char *duplicate_text(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static bool parse_protocols(const char *text, unsigned *protocols) {
    if (strcmp(text, "tcp") == 0) {
        *protocols = OD_PROTOCOL_TCP;
        return true;
    }
    if (strcmp(text, "udp") == 0) {
        *protocols = OD_PROTOCOL_UDP;
        return true;
    }
    if (strcmp(text, "tcp,udp") == 0 || strcmp(text, "udp,tcp") == 0) {
        *protocols = OD_PROTOCOL_TCP | OD_PROTOCOL_UDP;
        return true;
    }
    return false;
}

static bool update_profile_service(OdProfile *profile,
                                   size_t index,
                                   const char *name,
                                   const char *group,
                                   const char *variable,
                                   uint16_t port,
                                   unsigned protocols,
                                   OdError *error) {
    OdService *service = &profile->services[index];
    char *new_name = duplicate_text(name);
    char *new_group = duplicate_text(group);
    char *new_variable = duplicate_text(variable);
    if (new_name == NULL || new_group == NULL || new_variable == NULL) {
        free(new_name);
        free(new_group);
        free(new_variable);
        od_error_set(error, OD_ERROR_MEMORY, "unable to update managed service");
        return false;
    }
    char *old_name = service->name;
    char *old_group = service->group;
    char *old_variable = service->variable;
    uint16_t old_port = service->preferred_port;
    unsigned old_protocols = service->protocols;
    service->name = new_name;
    service->group = new_group;
    service->variable = new_variable;
    service->preferred_port = port;
    service->protocols = protocols;
    OdStatus status = od_profile_validate(profile, error);
    if (status != OD_OK) {
        service->name = old_name;
        service->group = old_group;
        service->variable = old_variable;
        service->preferred_port = old_port;
        service->protocols = old_protocols;
        free(new_name);
        free(new_group);
        free(new_variable);
        return false;
    }
    free(old_name);
    free(old_group);
    free(old_variable);
    return true;
}

static bool add_profile_service(OdProfile *profile,
                                const char *id,
                                const char *name,
                                const char *group,
                                const char *variable,
                                uint16_t port,
                                unsigned protocols,
                                OdError *error) {
    char *source_item = "manual:user";
    OdService service = {
        .id = (char *)id,
        .name = (char *)name,
        .group = (char *)group,
        .variable = (char *)variable,
        .preferred_port = port,
        .protocols = protocols,
        .sources = {.items = &source_item, .count = 1U},
        .managed = true
    };
    if (od_profile_add_service(profile, &service, error) != OD_OK) return false;
    if (od_profile_validate(profile, error) == OD_OK) return true;
    od_service_clear(&profile->services[profile->service_count - 1U]);
    --profile->service_count;
    return false;
}

static bool prompt_profile_service(WINDOW *window,
                                   OdCanvas *canvas,
                                   bool use_color,
                                   bool ascii,
                                   OdProfile *profile,
                                   size_t index,
                                   bool editing,
                                   char *status,
                                   size_t status_capacity) {
    char id[128] = {0};
    char name[128] = {0};
    char group[128] = "default";
    char variable[128] = {0};
    char port_text[16] = {0};
    char protocol_text[16] = "tcp";
    if (editing) {
        const OdService *service = &profile->services[index];
        (void)snprintf(id, sizeof(id), "%s", service->id);
        (void)snprintf(name, sizeof(name), "%s", service->name);
        (void)snprintf(group, sizeof(group), "%s", service->group);
        (void)snprintf(variable, sizeof(variable), "%s", service->variable);
        (void)snprintf(port_text, sizeof(port_text), "%u",
                       (unsigned)service->preferred_port);
        (void)snprintf(protocol_text, sizeof(protocol_text), "%s",
            service->protocols == (OD_PROTOCOL_TCP | OD_PROTOCOL_UDP) ? "tcp,udp" :
            (service->protocols == OD_PROTOCOL_UDP ? "udp" : "tcp"));
    }
    const char *title = editing ? "Edit managed service" : "Add managed service";
    if ((!editing && !prompt_text(window, canvas, use_color, ascii, title,
                                  "Stable ID", id, sizeof(id))) ||
        !prompt_text(window, canvas, use_color, ascii, title,
                     "Display name", name, sizeof(name)) ||
        !prompt_text(window, canvas, use_color, ascii, title,
                     "Group", group, sizeof(group)) ||
        !prompt_text(window, canvas, use_color, ascii, title,
                     "Environment variable", variable, sizeof(variable)) ||
        !prompt_text(window, canvas, use_color, ascii, title,
                     "Preferred port", port_text, sizeof(port_text)) ||
        !prompt_text(window, canvas, use_color, ascii, title,
                     "Protocols: tcp, udp, or tcp,udp", protocol_text,
                     sizeof(protocol_text))) {
        (void)snprintf(status, status_capacity, "Edit cancelled; the draft is unchanged.");
        return false;
    }
    uint16_t port = 0U;
    unsigned protocols = 0U;
    if (!parse_port_text(port_text, &port) ||
        port < profile->port_min || port > profile->port_max) {
        (void)snprintf(status, status_capacity, "Use a port from %u to %u.",
                       (unsigned)profile->port_min, (unsigned)profile->port_max);
        return false;
    }
    if (!parse_protocols(protocol_text, &protocols)) {
        (void)snprintf(status, status_capacity,
                       "Use tcp, udp, or tcp,udp for protocols.");
        return false;
    }
    OdError error;
    bool changed = editing ?
        update_profile_service(profile, index, name, group, variable,
                               port, protocols, &error) :
        add_profile_service(profile, id, name, group, variable,
                            port, protocols, &error);
    if (!changed) {
        (void)snprintf(status, status_capacity, "%s", error.message);
        return false;
    }
    (void)snprintf(status, status_capacity,
                   editing ? "Service updated in the draft; press s to save." :
                             "Service added to the draft; press s to save.");
    return true;
}

static void run_help(WINDOW *window,
                     OdCanvas *canvas,
                     bool use_color,
                     bool ascii) {
    OdError error;
    int height = getmaxy(window);
    OdHelp help;
    if (od_help_init(&help, height > 12 ? (size_t)(height - 12) : 1U, &error) != OD_OK) {
        return;
    }
    char status[256] = "Select a topic; every mouse action has a keyboard equivalent.";
    bool done = false;
    while (!done) {
        int width = getmaxx(window);
        height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) break;
        od_render_help(canvas, &help, ascii, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    input = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    input = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U &&
                           event.y >= 6 && event.y < height - 6) {
                    size_t target = help.page_start + (size_t)(event.y - 6);
                    if (target < help.visible_count) {
                        help.selected = target;
                        od_help_move(&help, 0);
                    }
                }
            }
        }
        if (termination_requested()) {
            done = true;
            continue;
        }
        if (input == 27 || input == 'q' || input == 'Q' || input == '?') {
            done = true;
        } else if (input == KEY_UP || input == 'k') {
            od_help_move(&help, -1);
        } else if (input == KEY_DOWN || input == 'j') {
            od_help_move(&help, 1);
        } else if (input == KEY_PPAGE) {
            od_help_move_page(&help, -1);
        } else if (input == KEY_NPAGE) {
            od_help_move_page(&help, 1);
        } else if (input == KEY_HOME) {
            od_help_home(&help);
        } else if (input == KEY_END) {
            od_help_end(&help);
        } else if (input == '/') {
            char query[96];
            (void)snprintf(query, sizeof(query), "%s", help.query);
            if (prompt_text(window, canvas, use_color, ascii,
                            "Search help", "Key, action, or workflow",
                            query, sizeof(query))) {
                if (od_help_search(&help, query, &error) == OD_OK) {
                    (void)snprintf(status, sizeof(status),
                        query[0] == '\0' ? "Showing all help topics." :
                                           "Showing help topics matching %.180s.",
                        query);
                } else {
                    (void)snprintf(status, sizeof(status), "%s", error.message);
                }
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    od_help_free(&help);
}

static size_t theme_index(const char *name) {
    for (size_t index = 0U; index < od_theme_count(); ++index) {
        const OdTheme *theme = od_theme_at(index);
        if (theme != NULL && strcmp(theme->name, name) == 0) return index;
    }
    return 0U;
}

static void change_setting(OdSettings *settings, size_t selected, int direction) {
    int step = direction < 0 ? -1 : 1;
    if (selected == 0U) {
        size_t count = od_theme_count();
        size_t index = theme_index(settings->theme);
        index = direction < 0 ? (index == 0U ? count - 1U : index - 1U) :
                                (index + 1U) % count;
        const OdTheme *theme = od_theme_at(index);
        if (theme != NULL) (void)snprintf(settings->theme, sizeof(settings->theme), "%s", theme->name);
    } else if (selected == 1U) {
        int mode = (int)settings->unicode_mode + step;
        if (mode < (int)OD_UNICODE_AUTO) mode = (int)OD_UNICODE_NEVER;
        if (mode > (int)OD_UNICODE_NEVER) mode = (int)OD_UNICODE_AUTO;
        settings->unicode_mode = (OdUnicodeMode)mode;
    } else if (selected == 2U) {
        settings->reduced_motion = !settings->reduced_motion;
    } else if (selected == 3U) {
        settings->mouse = !settings->mouse;
    } else if (selected == 4U) {
        settings->auto_refresh = !settings->auto_refresh;
    } else if (selected == 5U) {
        if (direction < 0 && settings->refresh_seconds > 1U) {
            --settings->refresh_seconds;
        } else if (direction > 0 && settings->refresh_seconds < 3600U) {
            ++settings->refresh_seconds;
        }
    }
}

static bool run_settings(WINDOW *window,
                         OdCanvas *canvas,
                         bool use_color,
                         bool ascii,
                         const char *settings_path,
                         OdSettings *settings) {
    OdSettings draft = *settings;
    size_t selected = 0U;
    char status[256] = "Changes apply after you save.";
    OdError error;
    while (true) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) return false;
        OdSettingsView view = {&draft, selected, settings_path, status};
        od_render_settings(canvas, &view, ascii);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == KEY_MOUSE && settings->mouse) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    input = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    input = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U &&
                           event.y >= 6 && event.y < 12) {
                    selected = (size_t)(event.y - 6);
                    change_setting(&draft, selected, 1);
                    (void)snprintf(status, sizeof(status),
                                   "Changed locally; press s to save.");
                    input = ERR;
                }
            }
        }
        if (termination_requested()) return false;
        if (input == 27 || input == 'q' || input == 'Q') return false;
        if (input == KEY_UP || input == 'k') {
            selected = selected == 0U ? 5U : selected - 1U;
        } else if (input == KEY_DOWN || input == 'j') {
            selected = (selected + 1U) % 6U;
        } else if (input == KEY_LEFT || input == 'h') {
            change_setting(&draft, selected, -1);
            (void)snprintf(status, sizeof(status), "Changed locally; press s to save.");
        } else if (input == KEY_RIGHT || input == 'l' || input == ' ') {
            change_setting(&draft, selected, 1);
            (void)snprintf(status, sizeof(status), "Changed locally; press s to save.");
        } else if (input == 's' || input == 'S' || input == '\n' || input == '\r') {
            if (settings_path == NULL) {
                (void)snprintf(status, sizeof(status),
                               "Unable to save: settings path is unavailable.");
            } else if (od_settings_save(settings_path, &draft, &error) != OD_OK) {
                (void)snprintf(status, sizeof(status), "%s", error.message);
            } else {
                *settings = draft;
                return true;
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
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
        if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    input = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    input = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U &&
                           event.y >= 6) {
                    size_t target = od_onboarding_page_start(&onboarding) +
                                    (size_t)(event.y - 6);
                    if (target < onboarding.candidates.count &&
                        target < od_onboarding_page_start(&onboarding) +
                                 onboarding.page_size) {
                        onboarding.selected = target;
                        if (event.x >= 3 && event.x <= 6) {
                            input = ' ';
                        } else {
                            input = ERR;
                        }
                    }
                }
            }
        }
        if (termination_requested()) {
            finished = true;
            continue;
        }
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

static bool assignment_path(const char *project_root,
                            const OdProfile *profile,
                            char *path,
                            size_t capacity) {
    if (profile->assignment_file == NULL || profile->assignment_file[0] == '/' ||
        strstr(profile->assignment_file, "..") != NULL) return false;
    int count = snprintf(path, capacity, "%s%s%s", project_root,
                         project_root[0] != '\0' &&
                         project_root[strlen(project_root) - 1U] == '/' ? "" : "/",
                         profile->assignment_file);
    return count >= 0 && (size_t)count < capacity;
}

static OdStatus load_saved_assignments(const char *project_root,
                                       const OdProfile *profile,
                                       OdAssignments *assignments,
                                       OdError *error) {
    od_assignments_init(assignments);
    char path[4096];
    if (!assignment_path(project_root, profile, path, sizeof(path))) {
        od_error_set(error, OD_ERROR_INVALID, "assignment path is unsafe or too long");
        return OD_ERROR_INVALID;
    }
    struct stat information;
    if (lstat(path, &information) != 0 && errno == ENOENT) {
        od_error_clear(error);
        return OD_OK;
    }
    return od_assignments_load_file(path, assignments, error);
}

static OdStatus create_allocation_plan(const OdProfile *profile,
                                       const OdScanSnapshot *snapshot,
                                       const OdAssignments *saved,
                                       OdAllocationPlan *plan,
                                       OdError *error) {
    size_t occupied_count = snapshot->endpoint_count + snapshot->docker_mapping_count;
    OdOccupiedPort *occupied = NULL;
    if (occupied_count > 0U) {
        occupied = calloc(occupied_count, sizeof(*occupied));
        if (occupied == NULL) {
            od_error_set(error, OD_ERROR_MEMORY, "unable to prepare occupied ports");
            return OD_ERROR_MEMORY;
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
                                              saved, plan, error);
    free(occupied);
    return allocation_status;
}

static bool create_dashboard(const OdProfile *profile,
                             const OdScanSnapshot *snapshot,
                             const OdAssignments *saved,
                             OdDashboard *dashboard,
                             OdAllocationPlan *plan,
                             OdError *error) {
    OdStatus allocation_status = create_allocation_plan(profile, snapshot, saved,
                                                        plan, error);
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
    if (hit->action == OD_HIT_FOCUS_WIDGET) {
        dashboard->expanded = false;
    } else if (hit->action == OD_HIT_SCROLL_UP) {
        od_dashboard_move_focused(dashboard, -1);
    } else if (hit->action == OD_HIT_SCROLL_DOWN) {
        od_dashboard_move_focused(dashboard, 1);
    } else if (hit->action == OD_HIT_TOGGLE_EXPAND) {
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

static OdStatus scan_now(uint64_t generation,
                         OdScanSnapshot *snapshot,
                         OdError *error);

static bool apply_dashboard_snapshot(const char *project_root,
                                     const OdProfile *profile,
                                     OdScanSnapshot *snapshot,
                                     OdScanSnapshot *fresh,
                                     OdAssignments *saved,
                                     OdAllocationPlan *plan,
                                     OdDashboard *dashboard,
                                     char *status,
                                     size_t status_capacity) {
    OdError error;
    OdAssignments new_saved;
    OdStatus saved_status = load_saved_assignments(project_root, profile, &new_saved, &error);
    if (saved_status != OD_OK && saved_status != OD_ERROR_FOREIGN) {
        (void)snprintf(status, status_capacity, "%s", error.message);
        return false;
    }
    OdAllocationPlan new_plan = {0};
    OdDashboard new_dashboard;
    if (!create_dashboard(profile, fresh,
                          saved_status == OD_OK ? &new_saved : NULL,
                          &new_dashboard, &new_plan, &error)) {
        od_assignments_free(&new_saved);
        (void)snprintf(status, status_capacity, "%s", error.message);
        return false;
    }

    char selected_id[64];
    char search[128];
    (void)snprintf(selected_id, sizeof(selected_id), "%s", dashboard->selected_id);
    (void)snprintf(search, sizeof(search), "%s", dashboard->search);
    OdServiceSort sort = dashboard->sort;
    bool ascending = dashboard->sort_ascending;
    OdDashboardWidget focused = dashboard->focused;
    bool expanded = dashboard->expanded;
    (void)snprintf(new_dashboard.selected_id, sizeof(new_dashboard.selected_id), "%s",
                   selected_id);
    if (od_dashboard_search(&new_dashboard, search, &error) != OD_OK) {
        od_dashboard_free(&new_dashboard);
        od_allocation_plan_free(&new_plan);
        od_assignments_free(&new_saved);
        (void)snprintf(status, status_capacity, "%s", error.message);
        return false;
    }
    if (sort != OD_SERVICE_SORT_NAME) od_dashboard_sort(&new_dashboard, sort);
    if (!ascending) od_dashboard_sort(&new_dashboard, sort);
    new_dashboard.focused = focused;
    new_dashboard.expanded = expanded;

    od_dashboard_free(dashboard);
    od_allocation_plan_free(plan);
    od_assignments_free(saved);
    od_scan_snapshot_free(snapshot);
    *snapshot = *fresh;
    *fresh = (OdScanSnapshot){0};
    new_dashboard.snapshot = snapshot;
    *dashboard = new_dashboard;
    *plan = new_plan;
    *saved = new_saved;
    (void)snprintf(status, status_capacity, "Scan #%llu complete • %zu warning(s)",
                   (unsigned long long)snapshot->generation, snapshot->warning_count);
    return true;
}

typedef struct {
    pthread_t thread;
    atomic_bool done;
    bool active;
    OdScanSnapshot snapshot;
    OdError error;
    OdStatus status;
} DashboardRefresh;

static void *dashboard_refresh_main(void *argument) {
    DashboardRefresh *refresh = argument;
    refresh->status = scan_now(refresh->snapshot.generation,
                               &refresh->snapshot, &refresh->error);
    atomic_store(&refresh->done, true);
    return NULL;
}

static bool dashboard_refresh_start(DashboardRefresh *refresh,
                                    uint64_t generation,
                                    OdError *error) {
    if (refresh->active) return false;
    *refresh = (DashboardRefresh){0};
    od_scan_snapshot_init(&refresh->snapshot, generation);
    atomic_init(&refresh->done, false);
    if (pthread_create(&refresh->thread, NULL, dashboard_refresh_main, refresh) != 0) {
        od_scan_snapshot_free(&refresh->snapshot);
        od_error_set(error, OD_ERROR_IO, "unable to start background refresh");
        return false;
    }
    refresh->active = true;
    return true;
}

static void run_dashboard_detail(WINDOW *window,
                                 OdCanvas *canvas,
                                 bool use_color,
                                 bool ascii,
                                 const OdDashboard *dashboard) {
    size_t page = 0U;
    size_t page_count = 1U;
    bool done = false;
    while (!done) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        OdError error;
        if (!canvas_resize(canvas, width, height, &error)) return;
        page_count = od_render_dashboard_detail(canvas, dashboard, page, ascii);
        if (page >= page_count) page = page_count - 1U;
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) input = KEY_PPAGE;
                if ((event.bstate & BUTTON5_PRESSED) != 0U) input = KEY_NPAGE;
            }
        }
        if (termination_requested() || input == 27 || input == 'q' ||
            input == 'Q' || input == 'd' || input == 'D') {
            done = true;
        } else if ((input == KEY_PPAGE || input == KEY_UP || input == 'k') && page > 0U) {
            --page;
        } else if ((input == KEY_NPAGE || input == KEY_DOWN || input == 'j') &&
                   page + 1U < page_count) {
            ++page;
        } else if (input == KEY_HOME) {
            page = 0U;
        } else if (input == KEY_END) {
            page = page_count - 1U;
        }
    }
}

static void run_dashboard(WINDOW *window,
                          OdCanvas *canvas,
                          bool use_color,
                          bool ascii,
                          const char *project_root,
                          const OdProfile *profile,
                          const OdSettings *settings,
                          OdScanSnapshot *snapshot) {
    OdDashboard dashboard;
    OdAllocationPlan plan = {0};
    OdError error;
    OdAssignments saved;
    OdStatus saved_status = load_saved_assignments(project_root, profile, &saved, &error);
    if (saved_status != OD_OK && saved_status != OD_ERROR_FOREIGN) return;
    if (!create_dashboard(profile, snapshot,
                          saved_status == OD_OK ? &saved : NULL,
                          &dashboard, &plan, &error)) {
        od_assignments_free(&saved);
        return;
    }
    if (profile->service_count == 0U) dashboard.focused = OD_WIDGET_LISTENERS;
    OdHitMap hit_map;
    od_hitmap_init(&hit_map);
    char status[256];
    (void)snprintf(status, sizeof(status), "Live scan ready • %zu warning(s)",
                   snapshot->warning_count);
    uint64_t last_refresh = monotonic_milliseconds();
    DashboardRefresh refresh = {0};
    bool done = false;
    while (!done) {
        if (refresh.active && atomic_load(&refresh.done)) {
            (void)pthread_join(refresh.thread, NULL);
            refresh.active = false;
            if (refresh.status == OD_OK) {
                if (!apply_dashboard_snapshot(project_root, profile, snapshot,
                                              &refresh.snapshot, &saved, &plan,
                                              &dashboard, status, sizeof(status))) {
                    od_scan_snapshot_free(&refresh.snapshot);
                }
            } else {
                (void)snprintf(status, sizeof(status), "%s", refresh.error.message);
            }
            last_refresh = monotonic_milliseconds();
        }
        uint64_t now = monotonic_milliseconds();
        if (settings->auto_refresh && !refresh.active &&
            now - last_refresh >= (uint64_t)settings->refresh_seconds * UINT64_C(1000)) {
            if (dashboard_refresh_start(&refresh, snapshot->generation + 1U, &error)) {
                (void)snprintf(status, sizeof(status),
                               "Refresh in progress • navigation remains available.");
            } else {
                (void)snprintf(status, sizeof(status), "%s", error.message);
                last_refresh = now;
            }
        }
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) break;
        od_render_dashboard(canvas, &dashboard, ascii, &hit_map, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (termination_requested()) {
            done = true;
            continue;
        }
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
        } else if (input == 'd' || input == 'D') {
            run_dashboard_detail(window, canvas, use_color, ascii, &dashboard);
            (void)snprintf(status, sizeof(status),
                           "Details closed • selection and focus preserved.");
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
        } else if (input == '?') {
            run_help(window, canvas, use_color, ascii);
            (void)snprintf(status, sizeof(status),
                           "Help closed • dashboard focus preserved.");
        } else if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    od_dashboard_move_focused(&dashboard, -3);
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    od_dashboard_move_focused(&dashboard, 3);
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U) {
                    const OdHitRegion *hit = od_hitmap_at(&hit_map, event.x, event.y);
                    if (hit != NULL) select_mouse_target(&dashboard, hit);
                }
            }
        } else if (input == 'r' || input == 'R') {
            if (refresh.active) {
                (void)snprintf(status, sizeof(status),
                               "Refresh already in progress • navigation remains available.");
            } else if (dashboard_refresh_start(&refresh, snapshot->generation + 1U, &error)) {
                (void)snprintf(status, sizeof(status),
                               "Refresh in progress • navigation remains available.");
            } else {
                (void)snprintf(status, sizeof(status), "%s", error.message);
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    if (refresh.active) {
        (void)pthread_join(refresh.thread, NULL);
        od_scan_snapshot_free(&refresh.snapshot);
    }
    od_hitmap_free(&hit_map);
    od_dashboard_free(&dashboard);
    od_allocation_plan_free(&plan);
    od_assignments_free(&saved);
}

static void run_listener_explorer(WINDOW *window,
                                  OdCanvas *canvas,
                                  bool use_color,
                                  bool ascii,
                                  const char *project_root,
                                  const OdSettings *settings,
                                  OdScanSnapshot *snapshot) {
    OdProfile profile;
    od_profile_init(&profile);
    char name[128];
    project_display_name(project_root, name, sizeof(name));
    size_t name_length = strlen(name) + 1U;
    profile.project_name = malloc(name_length);
    profile.assignment_file = malloc(sizeof(".ports.env"));
    if (profile.project_name == NULL || profile.assignment_file == NULL) {
        od_profile_free(&profile);
        return;
    }
    memcpy(profile.project_name, name, name_length);
    memcpy(profile.assignment_file, ".ports.env", sizeof(".ports.env"));
    profile.port_min = 1024U;
    profile.port_max = 65535U;
    run_dashboard(window, canvas, use_color, ascii, project_root,
                  &profile, settings, snapshot);
    od_profile_free(&profile);
}

static OdStatus scan_now(uint64_t generation,
                         OdScanSnapshot *snapshot,
                         OdError *error) {
    od_scan_snapshot_init(snapshot, generation);
    OdStatus status = od_scan_host(snapshot, error);
    if (status == OD_OK) {
        status = od_docker_scan("docker", 2500U, 4U * 1024U * 1024U,
                                snapshot, error);
    }
    if (status != OD_OK) od_scan_snapshot_free(snapshot);
    return status;
}

static bool parse_port_text(const char *text, uint16_t *port) {
    errno = 0;
    char *end = NULL;
    unsigned long numeric = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || numeric == 0UL || numeric > 65535UL) {
        return false;
    }
    *port = (uint16_t)numeric;
    return true;
}

static OdStatus review_conflicts(WINDOW *window,
                                 OdCanvas *canvas,
                                 bool use_color,
                                 bool ascii,
                                 OdResolution *resolution,
                                 OdError *error) {
    char status[256] = "Choose explicitly; nothing is saved during review.";
    while (!od_resolution_done(resolution)) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, error)) return error->code;
        od_render_conflict_resolution(canvas, resolution, ascii, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (termination_requested()) return OD_ERROR_CANCELLED;
        if (input == 27 || input == 'q' || input == 'Q') return OD_ERROR_CANCELLED;
        if (input == '\n' || input == '\r' || input == 'a' || input == 'A') {
            od_resolution_accept(resolution);
            (void)snprintf(status, sizeof(status), "Recommendation accepted.");
        } else if (input == 's' || input == 'S') {
            od_resolution_skip(resolution);
            (void)snprintf(status, sizeof(status),
                           "Service skipped; it will not be written to the assignment file.");
        } else if (input == 'e' || input == 'E') {
            const OdResolutionItem *item = od_resolution_current(resolution);
            if (item == NULL) continue;
            const OdAllocation *allocation =
                &resolution->plan->items[item->allocation_index];
            char port_text[16];
            (void)snprintf(port_text, sizeof(port_text), "%u",
                           (unsigned)allocation->new_port);
            if (prompt_text(window, canvas, use_color, ascii,
                            "Choose a replacement port", "Port number",
                            port_text, sizeof(port_text))) {
                uint16_t port;
                if (!parse_port_text(port_text, &port)) {
                    (void)snprintf(status, sizeof(status),
                                   "Port must be a number from 1 to 65535.");
                } else if (od_resolution_edit(resolution, port, error) != OD_OK) {
                    (void)snprintf(status, sizeof(status), "%s", error->message);
                } else {
                    (void)snprintf(status, sizeof(status),
                                   "Replacement changed to port %u; Enter accepts it.",
                                   (unsigned)port);
                }
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    od_error_clear(error);
    return OD_OK;
}

static OdStatus review_changes_and_save(WINDOW *window,
                                        OdCanvas *canvas,
                                        bool use_color,
                                        bool ascii,
                                        const char *project_root,
                                        const char *profile_path,
                                        const OdProfile *profile,
                                        bool import_foreign,
                                        OdAllocationPlan *plan,
                                        OdScanSnapshot *snapshot,
                                        OdError *error) {
    size_t selected = 0U;
    char status[256] = "Review the complete diff before saving.";
    while (true) {
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, error)) return error->code;
        od_render_change_review(canvas, profile, plan, selected, ascii, status);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        if (termination_requested()) return OD_ERROR_CANCELLED;
        size_t page_size = height > 12 ? (size_t)(height - 12) : 1U;
        if (input == 27 || input == 'q' || input == 'Q') return OD_ERROR_CANCELLED;
        if ((input == KEY_UP || input == 'k') && selected > 0U) {
            --selected;
        } else if ((input == KEY_DOWN || input == 'j') && selected + 1U < plan->count) {
            ++selected;
        } else if (input == KEY_PPAGE) {
            selected = selected > page_size ? selected - page_size : 0U;
        } else if (input == KEY_NPAGE && plan->count > 0U) {
            size_t maximum = plan->count - 1U;
            selected = selected > maximum - (selected > maximum ? 0U : selected) ? maximum :
                       selected + page_size;
            if (selected > maximum) selected = maximum;
        } else if (input == KEY_HOME) {
            selected = 0U;
        } else if (input == KEY_END && plan->count > 0U) {
            selected = plan->count - 1U;
        } else if (input == '\n' || input == '\r' || input == 's' || input == 'S') {
            (void)snprintf(status, sizeof(status),
                           "Rescanning and probing ports before the atomic save...");
            od_render_change_review(canvas, profile, plan, selected, ascii, status);
            paint_canvas(window, canvas, use_color);
            OdScanSnapshot fresh;
            OdStatus verify_status = scan_now(snapshot->generation + 1U, &fresh, error);
            bool fresh_ready = verify_status == OD_OK;
            if (verify_status == OD_OK) {
                verify_status = od_plan_validate_snapshot(profile, plan, &fresh, error);
            }
            if (verify_status == OD_OK) {
                verify_status = od_plan_probe_bindings(profile, plan, error);
            }
            if (verify_status != OD_OK) {
                if (fresh_ready) {
                    od_scan_snapshot_free(snapshot);
                    *snapshot = fresh;
                }
                return verify_status;
            }
            verify_status = import_foreign ?
                od_project_save_importing_foreign(project_root, profile_path,
                                                   profile, plan, error) :
                od_project_save(project_root, profile_path, profile, plan, error);
            if (verify_status != OD_OK) {
                od_scan_snapshot_free(&fresh);
                (void)snprintf(status, sizeof(status), "%s", error->message);
                continue;
            }
            od_scan_snapshot_free(snapshot);
            *snapshot = fresh;
            od_error_clear(error);
            return OD_OK;
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
}

static bool set_assignment_file(OdProfile *profile,
                                const char *value,
                                OdError *error) {
    if (value[0] == '\0' || value[0] == '/' || strstr(value, "..") != NULL) {
        od_error_set(error, OD_ERROR_INVALID,
                     "alternate output must be a safe project-relative path");
        return false;
    }
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store alternate output path");
        return false;
    }
    memcpy(copy, value, length);
    free(profile->assignment_file);
    profile->assignment_file = copy;
    return true;
}

static OdStatus choose_foreign_file_policy(WINDOW *window,
                                           OdCanvas *canvas,
                                           bool use_color,
                                           bool ascii,
                                           const char *project_root,
                                           OdProfile *profile,
                                           OdAssignments *saved,
                                           bool *import_foreign,
                                           OdError *error) {
    char choice[24] = "cancel";
    if (!prompt_text(window, canvas, use_color, ascii,
                     "Foreign assignment file detected",
                     "Type import, alternate, or cancel",
                     choice, sizeof(choice))) return OD_ERROR_CANCELLED;
    if (strcmp(choice, "cancel") == 0) return OD_ERROR_CANCELLED;
    char path[4096];
    if (!assignment_path(project_root, profile, path, sizeof(path))) {
        od_error_set(error, OD_ERROR_INVALID, "assignment path is unsafe or too long");
        return OD_ERROR_INVALID;
    }
    if (strcmp(choice, "import") == 0) {
        OdStatus status = od_assignments_import_file(path, saved, error);
        if (status == OD_OK) *import_foreign = true;
        return status;
    }
    if (strcmp(choice, "alternate") == 0) {
        char alternate[256] = ".ports.opendoor.env";
        if (!prompt_text(window, canvas, use_color, ascii,
                         "Choose alternate assignment output",
                         "Project-relative file path",
                         alternate, sizeof(alternate))) return OD_ERROR_CANCELLED;
        if (!set_assignment_file(profile, alternate, error)) return error->code;
        return load_saved_assignments(project_root, profile, saved, error);
    }
    od_error_set(error, OD_ERROR_INVALID,
                 "choose import, alternate, or cancel for the foreign file");
    return OD_ERROR_INVALID;
}

static OdStatus run_resolution(WINDOW *window,
                               OdCanvas *canvas,
                               bool use_color,
                               bool ascii,
                               const char *project_root,
                               const char *profile_path,
                               OdProfile *profile,
                               OdScanSnapshot *snapshot,
                               OdError *error) {
    OdAssignments saved;
    OdStatus status = load_saved_assignments(project_root, profile, &saved, error);
    bool import_foreign = false;
    if (status == OD_ERROR_FOREIGN) {
        status = choose_foreign_file_policy(window, canvas, use_color, ascii,
                                            project_root, profile, &saved,
                                            &import_foreign, error);
    }
    if (status != OD_OK) return status;
    OdAllocationPlan plan = {0};
    status = create_allocation_plan(profile, snapshot, &saved, &plan, error);
    od_assignments_free(&saved);
    if (status != OD_OK) return status;
    OdResolution resolution;
    status = od_resolution_init(&resolution, profile, snapshot, &plan, error);
    if (status == OD_OK) {
        status = review_conflicts(window, canvas, use_color, ascii, &resolution, error);
    }
    od_resolution_free(&resolution);
    if (status == OD_OK) {
        status = review_changes_and_save(window, canvas, use_color, ascii,
                                         project_root, profile_path, profile,
                                         import_foreign,
                                         &plan, snapshot, error);
    }
    od_allocation_plan_free(&plan);
    return status;
}

static OdStatus clone_profile(const OdProfile *source,
                              OdProfile *copy,
                              OdError *error) {
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = od_profile_render(source, &text, &length, error);
    if (status == OD_OK) status = od_profile_parse(text, length, copy, error);
    free(text);
    return status;
}

static bool run_profile_editor(WINDOW *window,
                               OdCanvas *canvas,
                               bool use_color,
                               bool ascii,
                               const char *project_root,
                               const char *profile_path,
                               OdProfile *profile,
                               OdScanSnapshot *snapshot,
                               char *result,
                               size_t result_capacity) {
    OdError error;
    OdProfile draft;
    if (clone_profile(profile, &draft, &error) != OD_OK) {
        (void)snprintf(result, result_capacity, "%s", error.message);
        return false;
    }
    size_t selected = 0U;
    char status[256] = "Edit a service or add one; nothing is saved until you press s.";
    bool done = false;
    bool saved = false;
    while (!done) {
        if (draft.service_count > 0U && selected >= draft.service_count) {
            selected = draft.service_count - 1U;
        }
        int width = getmaxx(window);
        int height = getmaxy(window);
        if (!canvas_resize(canvas, width, height, &error)) break;
        char assignment[4096];
        const char *assignment_display = assignment_path(project_root, &draft,
                                                          assignment,
                                                          sizeof(assignment)) ?
                                             assignment : "unavailable";
        OdProfileView view = {
            .profile = &draft,
            .selected_service = selected,
            .profile_path = profile_path,
            .assignment_path = assignment_display,
            .status = status
        };
        od_render_profile_editor(canvas, &view, ascii);
        paint_canvas(window, canvas, use_color);
        int input = wgetch(window);
        size_t rows = height > 10 ? (size_t)(height - 10) : 1U;
        if (input == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    input = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    input = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U &&
                           event.y >= 6 && event.y < 6 + (int)rows) {
                    size_t page_start = (selected / rows) * rows;
                    size_t target = page_start + (size_t)(event.y - 6);
                    if (target < draft.service_count) selected = target;
                    input = ERR;
                }
            }
        }
        if (termination_requested()) {
            done = true;
        } else if (input == 27 || input == 'q' || input == 'Q') {
            (void)snprintf(result, result_capacity,
                           "Profile editor closed; draft changes were discarded.");
            done = true;
        } else if ((input == KEY_UP || input == 'k') && selected > 0U) {
            --selected;
        } else if ((input == KEY_DOWN || input == 'j') &&
                   selected + 1U < draft.service_count) {
            ++selected;
        } else if (input == KEY_PPAGE) {
            selected = selected > rows ? selected - rows : 0U;
        } else if (input == KEY_NPAGE && draft.service_count > 0U) {
            size_t maximum = draft.service_count - 1U;
            selected = selected + rows > maximum ? maximum : selected + rows;
        } else if (input == KEY_HOME) {
            selected = 0U;
        } else if (input == KEY_END && draft.service_count > 0U) {
            selected = draft.service_count - 1U;
        } else if (input == 'e' || input == 'E' || input == '\n' || input == '\r') {
            if (draft.service_count == 0U) {
                (void)snprintf(status, sizeof(status),
                               "No service is selected; press a to add one.");
            } else {
                (void)prompt_profile_service(window, canvas, use_color, ascii,
                                             &draft, selected, true,
                                             status, sizeof(status));
            }
        } else if (input == 'a' || input == 'A') {
            if (prompt_profile_service(window, canvas, use_color, ascii,
                                       &draft, selected, false,
                                       status, sizeof(status))) {
                selected = draft.service_count - 1U;
            }
        } else if (input == 'x' || input == 'X') {
            char confirmation[16] = {0};
            if (!prompt_text(window, canvas, use_color, ascii,
                             "Reset local assignments",
                             "Type RESET to back up and remove only the assignment file",
                             confirmation, sizeof(confirmation))) {
                (void)snprintf(status, sizeof(status),
                               "Reset cancelled; no assignment file was changed.");
            } else if (strcmp(confirmation, "RESET") != 0) {
                (void)snprintf(status, sizeof(status),
                               "Reset cancelled; type RESET exactly to confirm.");
            } else if (od_project_reset_assignments(project_root, profile, &error) != OD_OK) {
                (void)snprintf(status, sizeof(status), "%s", error.message);
            } else {
                (void)snprintf(status, sizeof(status),
                               "Local assignments reset; the project profile was kept.");
            }
        } else if (input == 's' || input == 'S') {
            OdStatus save_status = od_profile_validate(&draft, &error);
            if (save_status == OD_OK) {
                save_status = run_resolution(window, canvas, use_color, ascii,
                                             project_root, profile_path, &draft,
                                             snapshot, &error);
            }
            if (save_status == OD_OK) {
                od_profile_free(profile);
                *profile = draft;
                draft = (OdProfile){0};
                (void)snprintf(result, result_capacity,
                               "Project profile and assignments saved atomically.");
                saved = true;
                done = true;
            } else if (save_status == OD_ERROR_CANCELLED) {
                (void)snprintf(status, sizeof(status),
                               "Save cancelled; the profile draft remains open.");
            } else {
                (void)snprintf(status, sizeof(status), "%s", error.message);
            }
        }
    }
    od_profile_free(&draft);
    return saved;
}

int od_tui_run(const OpendoorOptions *options) {
    (void)setlocale(LC_ALL, "");
    OdSettings settings;
    od_settings_defaults(&settings);
    char settings_path[4096];
    OdError settings_error;
    const char *active_settings_path =
        od_settings_resolve_path(settings_path, sizeof(settings_path), &settings_error) == OD_OK ?
            settings_path : NULL;
    char settings_notice[256] = {0};
    if (active_settings_path != NULL) {
        struct stat settings_information;
        if (lstat(active_settings_path, &settings_information) == 0) {
            if (od_settings_load(active_settings_path, &settings, &settings_error) != OD_OK) {
                od_settings_defaults(&settings);
                (void)snprintf(settings_notice, sizeof(settings_notice),
                               "Settings ignored: %.220s", settings_error.message);
            }
        } else if (errno != ENOENT) {
            (void)snprintf(settings_notice, sizeof(settings_notice),
                           "Settings unavailable: %.210s", strerror(errno));
        }
    } else {
        (void)snprintf(settings_notice, sizeof(settings_notice), "%s",
                       settings_error.message);
    }
    const OdTheme *theme = od_theme_by_name(settings.theme);
    if (theme == NULL) {
        theme = od_theme_by_name("midnight");
        (void)snprintf(settings.theme, sizeof(settings.theme), "midnight");
        (void)snprintf(settings_notice, sizeof(settings_notice),
                       "Unknown theme replaced with midnight; save settings to keep it.");
    }
    bool ascii = options->force_ascii || settings.unicode_mode == OD_UNICODE_NEVER ||
                 (settings.unicode_mode == OD_UNICODE_AUTO && MB_CUR_MAX <= 1U);
    bool reduced_motion = options->reduced_motion || settings.reduced_motion;
    SignalState signal_state;
    if (!signal_state_install(&signal_state)) return 5;
    StartupScan scan = {0};
    od_scan_snapshot_init(&scan.snapshot, 1U);
    atomic_init(&scan.stage, OD_LOAD_PROJECT_FILES);
    atomic_init(&scan.done, false);
    atomic_init(&scan.warning_count, 0U);
    if (pthread_create(&scan.thread, NULL, startup_scan_main, &scan) != 0) {
        od_scan_snapshot_free(&scan.snapshot);
        signal_state_restore(&signal_state);
        return 5;
    }

    WINDOW *window = initscr();
    if (window == NULL) {
        (void)pthread_join(scan.thread, NULL);
        od_scan_snapshot_free(&scan.snapshot);
        signal_state_restore(&signal_state);
        return 5;
    }
    (void)noecho();
    (void)cbreak();
    (void)curs_set(0);
    (void)keypad(window, true);
    (void)mousemask(settings.mouse ? ALL_MOUSE_EVENTS : 0U, NULL);
    wtimeout(window, 80);
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
            signal_state_restore(&signal_state);
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
                              reduced_motion,
                              ascii,
                              atomic_load(&scan.warning_count));
        }
        paint_canvas(window, &canvas, use_color);
        int input = wgetch(window);
        if (termination_requested()) {
            animation_skipped = true;
            break;
        }
        if (input == 27) animation_skipped = true;
        if (atomic_load(&scan.done) && elapsed >= 350U) break;
    }

    bool configured = profile_exists(options);
    const char *project_root = options->project_path == NULL ? "." : options->project_path;
    char default_profile_path[4096];
    const char *active_profile_path = resolved_profile_path(
        options, default_profile_path, sizeof(default_profile_path));
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
    bool quit = termination_requested();
    char status[256];
    menu_status(status, sizeof(status), configured, selected);
    if (profile_error[0] != '\0') {
        (void)snprintf(status, sizeof(status), "Profile not loaded: %.220s", profile_error);
    } else if (settings_notice[0] != '\0') {
        (void)snprintf(status, sizeof(status), "%s", settings_notice);
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
            od_render_main_menu(&canvas, &view, ascii);
        }
        paint_canvas(window, &canvas, use_color);
        int input = wgetch(window);
        if (input == KEY_MOUSE && settings.mouse) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if ((event.bstate & BUTTON4_PRESSED) != 0U) {
                    input = KEY_UP;
                } else if ((event.bstate & BUTTON5_PRESSED) != 0U) {
                    input = KEY_DOWN;
                } else if ((event.bstate & BUTTON1_CLICKED) != 0U) {
                    int menu_width = 50;
                    if (menu_width > width - 4) menu_width = width - 4;
                    int menu_x = (width - menu_width) / 2;
                    int first_row = 11;
                    if (event.x >= menu_x && event.x < menu_x + menu_width &&
                        event.y >= first_row &&
                        event.y < first_row + (int)menu_item_count(configured)) {
                        selected = (size_t)(event.y - first_row);
                        input = '\n';
                    }
                }
            }
        }
        size_t count = menu_item_count(configured);
        if (termination_requested()) {
            quit = true;
        } else if (input == '?') {
            run_help(window, &canvas, use_color, ascii);
            menu_status(status, sizeof(status), configured, selected);
        } else if (input == 'q' || input == 'Q' || input == 27) {
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
                if (run_onboarding(window, &canvas, use_color, ascii,
                                   project_root, &session_profile)) {
                    configured = true;
                    session_profile_ready = true;
                    selected = 0U;
                    if (!scan_joined) {
                        (void)pthread_join(scan.thread, NULL);
                        scan_joined = true;
                    }
                    OdError workflow_error;
                    OdStatus workflow_status = active_profile_path == NULL ? OD_ERROR_INVALID :
                        run_resolution(window, &canvas, use_color, ascii,
                                       project_root, active_profile_path, &session_profile,
                                       &scan.snapshot, &workflow_error);
                    if (workflow_status == OD_OK) {
                        (void)snprintf(status, sizeof(status),
                                       "Profile and assignments saved atomically.");
                    } else if (workflow_status == OD_ERROR_CANCELLED) {
                        (void)snprintf(status, sizeof(status),
                                       "Save cancelled; no assignment changes were written.");
                    } else {
                        (void)snprintf(status, sizeof(status), "%s",
                            active_profile_path == NULL ? "Profile path is too long." :
                                                          workflow_error.message);
                    }
                } else {
                    (void)snprintf(status, sizeof(status),
                                   "Discovery review cancelled; no files were changed.");
                }
            } else if (!configured && selected == 1U) {
                if (!atomic_load(&scan.done)) {
                    (void)snprintf(status, sizeof(status),
                                   "The startup scan is still running; try again in a moment.");
                } else {
                    if (!scan_joined) {
                        (void)pthread_join(scan.thread, NULL);
                        scan_joined = true;
                    }
                    run_listener_explorer(window, &canvas, use_color, ascii,
                                          project_root, &settings, &scan.snapshot);
                    menu_status(status, sizeof(status), configured, selected);
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
                    run_dashboard(window, &canvas, use_color, ascii,
                                  project_root, &session_profile, &settings,
                                  &scan.snapshot);
                    menu_status(status, sizeof(status), configured, selected);
                }
            } else if (configured && selected == 1U) {
                if (!session_profile_ready || active_profile_path == NULL) {
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
                    OdError workflow_error;
                    OdStatus workflow_status = run_resolution(
                        window, &canvas, use_color, ascii,
                        project_root, active_profile_path, &session_profile,
                        &scan.snapshot, &workflow_error);
                    if (workflow_status == OD_OK) {
                        (void)snprintf(status, sizeof(status),
                                       "Assignments verified and saved atomically.");
                    } else if (workflow_status == OD_ERROR_CANCELLED) {
                        (void)snprintf(status, sizeof(status),
                                       "Conflict review cancelled; no files were changed.");
                    } else {
                        (void)snprintf(status, sizeof(status), "%s", workflow_error.message);
                    }
                }
            } else if (configured && selected == 2U) {
                if (!atomic_load(&scan.done)) {
                    (void)snprintf(status, sizeof(status),
                                   "The startup scan is still running; try again in a moment.");
                } else {
                    if (!scan_joined) {
                        (void)pthread_join(scan.thread, NULL);
                        scan_joined = true;
                    }
                    OdScanSnapshot fresh;
                    OdError refresh_error;
                    OdStatus refresh_status = scan_now(scan.snapshot.generation + 1U,
                                                       &fresh, &refresh_error);
                    if (refresh_status == OD_OK) {
                        od_scan_snapshot_free(&scan.snapshot);
                        scan.snapshot = fresh;
                        (void)snprintf(status, sizeof(status),
                                       "Scan #%llu complete • %zu warning(s)",
                                       (unsigned long long)scan.snapshot.generation,
                                       scan.snapshot.warning_count);
                    } else {
                        (void)snprintf(status, sizeof(status), "%s", refresh_error.message);
                    }
                }
            } else if (configured && selected == 3U) {
                if (!session_profile_ready || active_profile_path == NULL) {
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
                    char editor_result[256] =
                        "Profile editor closed; no files were changed.";
                    (void)run_profile_editor(window, &canvas, use_color, ascii,
                                             project_root, active_profile_path,
                                             &session_profile, &scan.snapshot,
                                             editor_result, sizeof(editor_result));
                    (void)snprintf(status, sizeof(status), "%s", editor_result);
                }
            } else if ((!configured && selected == 2U) ||
                       (configured && selected == 4U)) {
                if (run_settings(window, &canvas, use_color, ascii,
                                 active_settings_path, &settings)) {
                    theme = od_theme_by_name(settings.theme);
                    if (theme == NULL) theme = od_theme_by_name("midnight");
                    use_color = initialize_colors(theme, options->no_color);
                    ascii = options->force_ascii ||
                            settings.unicode_mode == OD_UNICODE_NEVER ||
                            (settings.unicode_mode == OD_UNICODE_AUTO && MB_CUR_MAX <= 1U);
                    reduced_motion = options->reduced_motion || settings.reduced_motion;
                    (void)reduced_motion;
                    (void)mousemask(settings.mouse ? ALL_MOUSE_EVENTS : 0U, NULL);
                    (void)snprintf(status, sizeof(status),
                                   "Settings saved and applied.");
                } else {
                    (void)snprintf(status, sizeof(status),
                                   "Settings closed without saving changes.");
                }
            } else if ((!configured && selected == 3U) ||
                       (configured && selected == 5U)) {
                run_help(window, &canvas, use_color, ascii);
                menu_status(status, sizeof(status), configured, selected);
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
    signal_state_restore(&signal_state);
    return 0;
}
