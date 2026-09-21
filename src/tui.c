#include "opendoor/tui.h"

#include "curses_compat.h"
#include "opendoor/docker.h"
#include "opendoor/screens.h"
#include "opendoor/scan.h"
#include "opendoor/theme.h"
#include "opendoor/ui.h"

#include <locale.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

static bool profile_exists(const OpendoorOptions *options) {
    const char *path = options->profile_path;
    char default_path[4096];
    if (path == NULL) {
        const char *root = options->project_path == NULL ? "." : options->project_path;
        int count = snprintf(default_path, sizeof(default_path), "%s/.opendoor/project.toml", root);
        if (count < 0 || (size_t)count >= sizeof(default_path)) return false;
        path = default_path;
    }
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
    size_t selected = 0U;
    bool quit = false;
    char status[256];
    menu_status(status, sizeof(status), configured, selected);
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
            } else {
                menu_status(status, sizeof(status), configured, selected);
            }
        } else if (input == KEY_RESIZE) {
            continue;
        }
    }
    od_canvas_free(&canvas);
    (void)endwin();
    (void)pthread_join(scan.thread, NULL);
    od_scan_snapshot_free(&scan.snapshot);
    return 0;
}
