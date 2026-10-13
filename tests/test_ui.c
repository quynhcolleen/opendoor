#include "opendoor/app.h"
#include "opendoor/history.h"
#include "opendoor/screens.h"
#include "opendoor/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

int __real_doupdate(void);

typedef struct {
    char events[16];
    size_t count;
    bool intercept_flush;
} FlushTrace;

static FlushTrace *active_flush_trace = NULL;

static void trace_event(FlushTrace *trace, char event) {
    if (trace == NULL || trace->count + 1U >= sizeof(trace->events)) return;
    trace->events[trace->count++] = event;
    trace->events[trace->count] = '\0';
}

int __wrap_doupdate(void) {
    if (active_flush_trace != NULL && active_flush_trace->intercept_flush) {
        trace_event(active_flush_trace, 'F');
        return 0;
    }
    return __real_doupdate();
}

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static char *rendered_text(OdCanvas *canvas) {
    OdError error;
    char *text = od_canvas_to_text(canvas, &error);
    CHECK(text != NULL);
    return text;
}

static bool line_has_at_least(const char *text, char glyph, size_t count) {
    size_t seen = 0U;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            seen = 0U;
        } else if (*cursor == glyph && ++seen >= count) {
            return true;
        }
    }
    return false;
}

static size_t substring_count(const char *text, const char *needle) {
    size_t count = 0U;
    size_t length = strlen(needle);
    for (const char *match = strstr(text, needle);
         match != NULL;
         match = strstr(match + length, needle)) {
        ++count;
    }
    return count;
}

static const OdCell *canvas_cell(const OdCanvas *canvas, size_t x, size_t y) {
    CHECK(canvas != NULL);
    CHECK(x < canvas->width);
    CHECK(y < canvas->height);
    return &canvas->cells[y * canvas->width + x];
}

static void trace_loading_render(void *opaque) {
    trace_event(opaque, 'R');
}

static void trace_blocking_scan(void *opaque) {
    trace_event(opaque, 'B');
}

/* Catches a queued loading draw that is never flushed before work blocks. */
static void test_interstitial_is_flushed_before_blocking_work(void) {
    FlushTrace trace = {.intercept_flush = true};
    active_flush_trace = &trace;
    od_tui_run_flushed_interstitial(trace_loading_render,
                                    trace_blocking_scan,
                                    &trace);
    active_flush_trace = NULL;

    const char *render = strchr(trace.events, 'R');
    const char *flush = strchr(trace.events, 'F');
    const char *block = strchr(trace.events, 'B');
    CHECK(render != NULL);
    CHECK(flush != NULL);
    CHECK(block != NULL);
    CHECK(render != NULL && flush != NULL && render < flush);
    CHECK(flush != NULL && block != NULL && flush < block);
}

static void test_dashboard_loading_screen_explains_the_blocking_work(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_dashboard_loading(&canvas, true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "OPENDOOR / Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Scanning project ports...") != NULL);
    CHECK(text != NULL &&
          strstr(text, "Discovering declarations and checking live endpoints") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_resolve_open_loading_screen_explains_the_blocking_work(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_conflicts_loading(&canvas, true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "OPENDOOR / Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "Scanning for port conflicts...") != NULL);
    CHECK(text != NULL &&
          strstr(text, "Discovering declarations and checking live endpoints") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_resolve_apply_loading_screen_explains_the_blocking_work(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_conflicts_apply_loading(&canvas, true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "OPENDOOR / Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "Revalidating before apply...") != NULL);
    CHECK(text != NULL &&
          strstr(text, "Rescanning listeners before writing project files") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_history_loading_screen_explains_the_blocking_work(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_history_loading(&canvas, true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "OPENDOOR / History") != NULL);
    CHECK(text != NULL && strstr(text, "Loading change history...") != NULL);
    CHECK(text != NULL &&
          strstr(text, "Reading the log and checking recorded declarations") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_menu_dispatch_is_fixed_to_four_items(void) {
    CHECK(OD_MENU_COUNT == 4);
    CHECK(od_menu_dispatch(0U) == OD_MENU_DASHBOARD);
    CHECK(od_menu_dispatch(1U) == OD_MENU_RESOLVE_CONFLICTS);
    CHECK(od_menu_dispatch(2U) == OD_MENU_HISTORY);
    CHECK(od_menu_dispatch(3U) == OD_MENU_QUIT);
    CHECK(od_menu_dispatch(99U) == OD_MENU_QUIT);

    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_main_menu(&canvas, 1U, "/tmp/example/opendoor", "Ready", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "History") != NULL);
    CHECK(text != NULL && strstr(text, "Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Ports in this project") != NULL);
    CHECK(text != NULL && strstr(text, "Find and fix port clashes") != NULL);
    CHECK(text != NULL && strstr(text, "View and revert past changes") != NULL);
    CHECK(text != NULL && strstr(text, "Pre-start port check - example/opendoor") != NULL);
    CHECK(text != NULL && strstr(text, "/tmp/example/opendoor") == NULL);
    CHECK(text != NULL &&
          strstr(text,
                 "+-----------------------|OpenDoor|-----------------------+") != NULL);
    CHECK(text != NULL && strstr(text, "Up/Down Navigate   Enter Select   q Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Discover this project") == NULL);
    CHECK(text != NULL && strstr(text, "Settings") == NULL);
    CHECK(strcmp(canvas_cell(&canvas, 11U, 8U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 68U, 15U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 35U, 8U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 44U, 8U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 9U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 14U)->glyph, " ") == 0);
    for (size_t x = 12U; x <= 67U; ++x) {
        CHECK(canvas_cell(&canvas, x, 11U)->role == OD_ROLE_SELECTED);
    }
    CHECK(canvas_cell(&canvas, 11U, 11U)->role != OD_ROLE_SELECTED);
    CHECK(canvas_cell(&canvas, 68U, 11U)->role != OD_ROLE_SELECTED);
    CHECK(strcmp(canvas_cell(&canvas, 12U, 10U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 13U, 10U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 13U, 11U)->glyph, ">") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 13U, 12U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 10U)->glyph, "D") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 11U)->glyph, "R") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 12U)->glyph, "H") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 13U)->glyph, "Q") == 0);
    CHECK(canvas_cell(&canvas, 15U, 10U)->role == OD_ROLE_DEFAULT);
    CHECK(canvas_cell(&canvas, 15U, 10U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 36U, 10U)->role == OD_ROLE_MUTED);
    CHECK(canvas_cell(&canvas, 36U, 10U)->attributes == 0U);
    CHECK(strcmp(canvas_cell(&canvas, 36U, 10U)->glyph, "P") == 0);
    CHECK(canvas_cell(&canvas, 15U, 11U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 36U, 11U)->role == OD_ROLE_SELECTED);
    CHECK(canvas_cell(&canvas, 36U, 11U)->attributes == 0U);
    CHECK(strcmp(canvas_cell(&canvas, 32U, 11U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 33U, 11U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 34U, 11U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 35U, 11U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 36U, 12U)->glyph, "V") == 0);
    CHECK(canvas_cell(&canvas, 36U, 12U)->role == OD_ROLE_MUTED);
    CHECK(strcmp(canvas_cell(&canvas, 36U, 13U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 67U, 10U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 20U, 16U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 20U, 17U)->role == OD_ROLE_PRIMARY);
    CHECK(canvas_cell(&canvas, 20U, 17U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 28U, 17U)->role == OD_ROLE_MUTED);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 18U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 37U, 19U)->role == OD_ROLE_MUTED);

    od_render_main_menu(&canvas, 2U, "/tmp/example/opendoor", "Ready", true);
    for (size_t x = 12U; x <= 67U; ++x) {
        CHECK(canvas_cell(&canvas, x, 12U)->role == OD_ROLE_SELECTED);
        CHECK(canvas_cell(&canvas, x, 11U)->role != OD_ROLE_SELECTED);
    }
    CHECK(canvas_cell(&canvas, 15U, 12U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 36U, 12U)->role == OD_ROLE_SELECTED);
    CHECK(canvas_cell(&canvas, 36U, 12U)->attributes == 0U);
    CHECK(strcmp(canvas_cell(&canvas, 13U, 12U)->glyph, ">") == 0);
    od_render_main_menu(&canvas, 3U, "/tmp/example/opendoor", "Ready", true);
    for (size_t x = 12U; x <= 67U; ++x) {
        CHECK(canvas_cell(&canvas, x, 13U)->role == OD_ROLE_SELECTED);
        CHECK(canvas_cell(&canvas, x, 12U)->role != OD_ROLE_SELECTED);
    }
    CHECK(strcmp(canvas_cell(&canvas, 13U, 13U)->glyph, ">") == 0);
    free(text);
    od_canvas_free(&canvas);

    char original_directory[OD_PATH_CAP];
    CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    const char *original_home = getenv("HOME");
    char *saved_home = original_home == NULL ? NULL : strdup(original_home);
    CHECK(original_home == NULL || saved_home != NULL);
    char home_template[] = "/tmp/opendoor-menu-home-XXXXXX";
    char *fake_home = mkdtemp(home_template);
    CHECK(fake_home != NULL);
    char fake_project[OD_PATH_CAP];
    (void)snprintf(fake_project, sizeof(fake_project), "%s/DSVN", fake_home);
    CHECK(mkdir(fake_project, 0700) == 0);
    CHECK(setenv("HOME", fake_home, 1) == 0);
    CHECK(chdir(fake_project) == 0);

    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    od_render_main_menu(&canvas, 0U, ".", "Ready", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "▄▄▄▄") != NULL);
    CHECK(text != NULL && strstr(text, "╭") != NULL);
    CHECK(text != NULL && strstr(text, "╮") != NULL);
    CHECK(text != NULL && strstr(text, "╰") != NULL);
    CHECK(text != NULL && strstr(text, "╯") != NULL);
    CHECK(text != NULL && strstr(text, "|OpenDoor|") != NULL);
    CHECK(text != NULL && strstr(text, "▶") != NULL);
    CHECK(text != NULL && strstr(text, "↑↓ Navigate   Enter Select   q Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Pre-start port check · ~/DSVN") != NULL);
    CHECK(text != NULL && strstr(text, "Pre-start port check · .") == NULL);
    CHECK(text != NULL && strstr(text, fake_project) == NULL);
    CHECK(strcmp(canvas_cell(&canvas, 21U, 15U)->glyph, "╭") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 78U, 22U)->glyph, "╯") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 16U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 21U)->glyph, " ") == 0);
    for (size_t x = 22U; x <= 77U; ++x) {
        CHECK(canvas_cell(&canvas, x, 17U)->role == OD_ROLE_SELECTED);
    }
    CHECK(strcmp(canvas_cell(&canvas, 22U, 17U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 17U)->glyph, "▶") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 18U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 19U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 19U)->glyph, "H") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 20U)->glyph, "Q") == 0);
    CHECK(canvas_cell(&canvas, 25U, 17U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 46U, 17U)->role == OD_ROLE_SELECTED);
    CHECK(canvas_cell(&canvas, 46U, 17U)->attributes == 0U);
    CHECK(strcmp(canvas_cell(&canvas, 45U, 17U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 28U, 23U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 32U, 24U)->role == OD_ROLE_PRIMARY);
    CHECK(canvas_cell(&canvas, 35U, 24U)->role == OD_ROLE_MUTED);
    CHECK(strcmp(canvas_cell(&canvas, 35U, 25U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 47U, 26U)->role == OD_ROLE_MUTED);
    free(text);
    od_canvas_free(&canvas);
    CHECK(chdir(original_directory) == 0);
    if (saved_home == NULL) {
        CHECK(unsetenv("HOME") == 0);
    } else {
        CHECK(setenv("HOME", saved_home, 1) == 0);
    }
    free(saved_home);
    CHECK(rmdir(fake_project) == 0);
    CHECK(rmdir(fake_home) == 0);
}

static void test_menu_degrades_at_minimum_size(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 60U, 18U, &error) == OD_OK);
    od_render_main_menu(&canvas, 3U,
                        "/tmp/a-project-with-a-very-long-directory-name-for-menu-testing",
                        "Run before starting project services", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "History") != NULL);
    CHECK(text != NULL && strstr(text, "Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Ports in this project") == NULL);
    CHECK(text != NULL && strstr(text, "Find and fix port clashes") == NULL);
    CHECK(text != NULL && strstr(text, "View and revert past changes") == NULL);
    CHECK(text != NULL && strstr(text, "Run before starting project services") == NULL);
    CHECK(text != NULL && strstr(text, "...") != NULL);
    CHECK(strcmp(canvas_cell(&canvas, 2U, 6U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 57U, 13U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 6U, 10U)->glyph, "H") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 4U, 11U)->glyph, ">") == 0);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 59U, 17U, &error) == OD_OK);
    od_render_main_menu(&canvas, 0U, ".", "Ready", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Terminal too small") != NULL);
    CHECK(text != NULL && strstr(text, "Dashboard") == NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_update_is_cli_only_and_must_be_used_alone(void) {
    char *update_arguments[] = {"opendoor", "--update", NULL};
    OpendoorOptions options;
    CHECK(opendoor_parse_args(2, update_arguments, &options) == 0);
    CHECK(options.update_requested);

    char *mixed_arguments[] = {
        "opendoor", "--update", "--project", ".", NULL
    };
    CHECK(opendoor_parse_args(4, mixed_arguments, &options) == 2);

    char *help_arguments[] = {"opendoor", "--help", NULL};
    CHECK(opendoor_parse_args(2, help_arguments, &options) == 2);

    char *normal_arguments[] = {
        "opendoor", "--project", ".", "--ascii", NULL
    };
    CHECK(opendoor_parse_args(4, normal_arguments, &options) == 0);
    CHECK(options.project_path != NULL && strcmp(options.project_path, ".") == 0);
    CHECK(options.force_ascii);
    CHECK(!options.update_requested);
}

static void test_mouse_targets_match_visible_controls(void) {
    OdMouseTarget target = od_menu_mouse_target(100U, 32U, false, 40, 17);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 0U);
    target = od_menu_mouse_target(100U, 32U, false, 40, 18);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 1U);
    target = od_menu_mouse_target(100U, 32U, false, 40, 19);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 2U);
    target = od_menu_mouse_target(100U, 32U, false, 40, 20);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 3U);
    target = od_menu_mouse_target(100U, 32U, false, 40, 16);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(100U, 32U, false, 40, 21);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(100U, 32U, false, 21, 17);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(100U, 32U, false, 2, 2);
    CHECK(target.action == OD_MOUSE_NONE);

    target = od_menu_mouse_target(60U, 18U, true, 20, 8);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 0U);
    target = od_menu_mouse_target(60U, 18U, true, 20, 9);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 1U);
    target = od_menu_mouse_target(60U, 18U, true, 20, 10);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 2U);
    target = od_menu_mouse_target(60U, 18U, true, 20, 11);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 3U);
    target = od_menu_mouse_target(60U, 18U, true, 20, 7);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(60U, 18U, true, 20, 12);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(59U, 18U, true, 20, 10);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(60U, 17U, true, 20, 10);
    CHECK(target.action == OD_MOUSE_NONE);

    target = od_dashboard_mouse_target(80U, 24U, 18, 23);
    CHECK(target.action == OD_MOUSE_REFRESH);
    target = od_dashboard_mouse_target(80U, 24U, 32, 23);
    CHECK(target.action == OD_MOUSE_BACK);
    target = od_dashboard_mouse_target(80U, 24U, 10, 10);
    CHECK(target.action == OD_MOUSE_NONE);


}

static void test_dashboard_is_read_only(void) {
    OdPortRow rows[] = {
        {
            .port = 3000U,
            .status = OD_PORT_RUNNING,
            .declared = true,
            .relative_folder = "./services/api",
            .process = "api-server",
            .source = "env (PORT) — ./services/api/.env"
        },
        {
            .port = 4000U,
            .status = OD_PORT_NOT_RUNNING,
            .declared = true,
            .relative_folder = "./services/web",
            .process = "-",
            .source = "compose — ./services/web/compose.yaml"
        },
        {
            .port = 5000U,
            .status = OD_PORT_IN_USE_OTHER,
            .declared = true,
            .relative_folder = "./services/外部",
            .process = "other-server",
            .source = "env (PORT) — ./services/外部/.env"
        }
    };
    OdDashboard dashboard = {
        .rows = rows,
        .count = 3U,
        .page_size = 10U
    };
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 180U, 30U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, "Scan complete", false);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "PORT") != NULL);
    CHECK(text != NULL && strstr(text, "RELATIVE FOLDER") != NULL);
    CHECK(text != NULL && strstr(text, "STATUS") != NULL);
    CHECK(text != NULL && strstr(text, "SOURCE") != NULL);
    CHECK(text != NULL && strstr(text, "CONFLICT") == NULL);
    CHECK(text != NULL && strstr(text, "PROCESS") != NULL);
    CHECK(text != NULL && strstr(text, "DIRECTORY") == NULL);
    CHECK(text != NULL && strstr(text, "/work/") == NULL);
    CHECK(text != NULL && strstr(text, "./services/api") != NULL);
    CHECK(text != NULL && strstr(text, "env (PORT)") != NULL);
    CHECK(text != NULL && strstr(text, "./services/api/.env") != NULL);
    CHECK(text != NULL && strstr(text, "?") == NULL);
    CHECK(text != NULL && strstr(text, "┌") != NULL);
    CHECK(text != NULL && strstr(text, "┬") != NULL);
    CHECK(text != NULL && strstr(text, "├") != NULL);
    CHECK(text != NULL && strstr(text, "┼") != NULL);
    CHECK(text != NULL && strstr(text, "┤") != NULL);
    CHECK(text != NULL && strstr(text, "┴") != NULL);
    CHECK(text != NULL && substring_count(text, "├") >= 3U);
    CHECK(text != NULL && substring_count(text, "┼") >= 3U);
    CHECK(text != NULL && strstr(text, "│ PORT") != NULL);
    CHECK(text != NULL && strstr(text, "running") != NULL);
    CHECK(text != NULL && strstr(text, "not running") != NULL);
    CHECK(text != NULL && strstr(text, "in use (other)") != NULL);
    CHECK(text != NULL && strstr(text, "Up/Down Scroll") != NULL);
    CHECK(text != NULL && strstr(text, "r Refresh") != NULL);
    CHECK(text != NULL && strstr(text, "Enter") == NULL);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 60U, 18U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, "Scan complete", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Showing 1–1 of 3 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "q/Esc Back") != NULL);
    free(text);

    dashboard.scroll = 2U;
    od_render_dashboard(&canvas, &dashboard, "Scan complete", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Showing 3–3 of 3 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "5000") != NULL);
    free(text);
    od_canvas_free(&canvas);
    dashboard.scroll = 0U;

    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, "Scan complete", true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "| PORT") != NULL);
    CHECK(text != NULL && line_has_at_least(text, '+', 5U));
    free(text);
    od_canvas_free(&canvas);
}

/* A conflict is a STATUS condition, not a property of its path/process/source. */
static void test_dashboard_conflict_color_is_limited_to_status(void) {
    OdPortRow row = {
        .port = 5500U,
        .status = OD_PORT_IN_USE_OTHER,
        .declared = true,
        .relative_folder = "./",
        .process = "code",
        .source = "env (PORT) — ./.env"
    };
    OdDashboard dashboard = {
        .rows = &row,
        .count = 1U,
        .page_size = 10U
    };
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 100U, 24U, &error) == OD_OK);

    od_render_dashboard(&canvas, &dashboard, "Scan complete", true);

    /* At 100 columns, the five cell texts begin at x=5,13,39,56,81. */
    CHECK(canvas_cell(&canvas, 5U, 10U)->role == OD_ROLE_DEFAULT);
    CHECK(canvas_cell(&canvas, 13U, 10U)->role == OD_ROLE_DEFAULT);
    CHECK(canvas_cell(&canvas, 56U, 10U)->role == OD_ROLE_DEFAULT);
    CHECK(canvas_cell(&canvas, 81U, 10U)->role == OD_ROLE_DEFAULT);
    for (size_t offset = 0U; offset < strlen("in use (other)"); ++offset) {
        const OdCell *cell = canvas_cell(&canvas, 39U + offset, 10U);
        CHECK(cell->role == OD_ROLE_DANGER);
        CHECK(cell->attributes == 1U);
    }

    od_canvas_free(&canvas);
}

static void test_dashboard_renders_declaration_names_and_live_processes(void) {
    char project_root[OD_PATH_CAP];
    CHECK(getcwd(project_root, sizeof(project_root)) != NULL);

    OdPortDeclaration declaration = {
        .port = 37955U,
        .relative_path = "./services/backend/config/.env.production",
        .relative_folder = "./services/backend",
        .environment_key = "API_PORT"
    };
    OdProjectDiscovery discovery = {
        .items = &declaration,
        .count = 1U
    };
    OdEndpoint endpoints[2] = {
        {.local_port = 37955U, .pid = 101},
        {.local_port = 37956U, .pid = 102}
    };
    (void)snprintf(endpoints[0].directory, sizeof(endpoints[0].directory),
                   "%s", project_root);
    (void)snprintf(endpoints[0].process, sizeof(endpoints[0].process),
                   "project-worker-12345");
    (void)snprintf(endpoints[1].directory, sizeof(endpoints[1].directory),
                   "%s", project_root);
    (void)snprintf(endpoints[1].executable, sizeof(endpoints[1].executable),
                   "/usr/bin/node");
    OdScanSnapshot snapshot = {
        .endpoints = endpoints,
        .endpoint_count = 2U
    };
    OdDashboard dashboard;
    OdError error;
    CHECK(od_dashboard_init(&dashboard, project_root, &discovery,
                            &snapshot, &error) == OD_OK);

    OdCanvas canvas;
    CHECK(od_canvas_init(&canvas, 100U, 28U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, "Scan complete", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "PROCESS") != NULL);
    CHECK(text != NULL && strstr(text, "env (API_PORT)") != NULL);
    CHECK(text != NULL && strstr(text, "project-worker-12345") != NULL);
    CHECK(text != NULL && strstr(text, "node") != NULL);
    CHECK(text != NULL &&
          strstr(text, "./services/backend/config/.env.production") == NULL);
    free(text);
    od_canvas_free(&canvas);
    od_dashboard_free(&dashboard);
}

static void test_empty_dashboard_explains_supported_project_sources(void) {
    OdDashboard dashboard = {0};
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 100U, 28U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, "Scan complete", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "No ports found in this project") != NULL);
    CHECK(text != NULL && strstr(text, "Compose") != NULL);
    CHECK(text != NULL && strstr(text, ".env") != NULL);
    CHECK(text != NULL && strstr(text, "package.json") != NULL);
    CHECK(text != NULL && strstr(text, "Makefile") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_scan_warning_views_and_apply_severity(void) {
    OdError error;
    OdScanReport discovery = {0}, sockets = {0}, report = {0};
    CHECK(od_scan_report_add(&discovery, OD_WARNING_DISCOVERY,
                             "./package.json", "invalid JSON", &error) == OD_OK);
    CHECK(od_scan_report_add(&sockets, OD_WARNING_OWNER_ATTRIBUTION,
                             "Process ownership", "owners unreadable", &error) == OD_OK);
    CHECK(od_scan_report_combine(&report, &discovery, &sockets, &error) == OD_OK);
    od_scan_report_free(&discovery);
    od_scan_report_free(&sockets);
    OdCanvas canvas;
    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    OdResolution empty = {0};
    od_render_conflicts_report(&canvas, &empty, 0U, 0U, true, NULL, true, &report);
    char *text = rendered_text(&canvas);
    CHECK(strstr(text, "Scan incomplete (2 warnings)") != NULL);
    CHECK(strstr(text, "No conflicts found") == NULL);
    CHECK(strstr(text, "./package.json: invalid JSON") != NULL);
    CHECK(strstr(text, "[info]") != NULL);
    CHECK(strstr(text, "Process ownership: owners unreadable") != NULL);
    CHECK(canvas_cell(&canvas, 2U, 2U)->role == OD_ROLE_WARNING);
    free(text);
    OdResolutionItem item = {.automatic = true, .old_port = 5500U, .new_port = 5501U,
        .relative_path = "./.env", .variable = "PORT", .source_kind = OD_SOURCE_ENV};
    OdResolution resolution = {.items = &item, .count = 1U, .automatic_count = 1U};
    od_render_conflicts_report(&canvas, &resolution, 2U, 0U, true, NULL, true, &report);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "[Apply]") != NULL);
    CHECK(strstr(text, "Scan was incomplete (2 warnings); undiscovered declarations or unknown owners are possible.") != NULL);
    free(text);
    CHECK(od_scan_report_add(&report, OD_WARNING_SOCKET_PARTIAL,
                             "Socket table (IPv4 TCP)", "partial read", &error) == OD_OK);
    od_render_conflicts_report(&canvas, &resolution, 3U, 0U, true, NULL, true, &report);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "[Apply]") == NULL && strstr(text, "Blocked") != NULL);
    CHECK(strstr(text, "[blocks apply]") != NULL);
    CHECK(strstr(text, OD_SCAN_APPLY_BLOCKED) != NULL);
    free(text);
    od_render_dashboard_report(&canvas, NULL, NULL, true, &report, false, 0U);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "w Warnings") != NULL);
    CHECK(strstr(text, "Scan incomplete (3 warnings)") != NULL);
    free(text);
    bool mouse_warning = false;
    for (int x = 0; x < 100; ++x) {
        if (od_dashboard_report_mouse_target(100U, 32U, &report, false, x, 31).action == OD_MOUSE_WARNINGS) mouse_warning = true;
    }
    CHECK(mouse_warning);
    od_render_dashboard_report(&canvas, NULL, NULL, true, &report, true, 0U);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "./package.json: invalid JSON") != NULL);
    CHECK(strstr(text, "[blocks apply]") != NULL);
    free(text);
    od_render_dashboard_report(&canvas, NULL, "no proc socket tables were readable",
                                true, &report, false, 0U);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "no proc socket tables were readable") != NULL);
    CHECK(strstr(text, "Scan incomplete (3 warnings)") != NULL);
    free(text);
    for (size_t i = 0U; i < 202U; ++i) {
        CHECK(od_scan_report_add(&report, OD_WARNING_DISCOVERY, "./bad.json", "invalid JSON", &error) == OD_OK);
    }
    od_render_dashboard_report(&canvas, NULL, NULL, true, &report, true, 199U);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "and 5 more") != NULL);
    CHECK(strstr(text, "Scan incomplete (205 warnings)") != NULL);
    free(text);
    od_canvas_free(&canvas);
    od_scan_report_free(&report);
}

static void test_resolve_summary_is_not_duplicated(void) {
    OdError error;
    OdScanReport report = {0};
    CHECK(od_scan_report_add(&report, OD_WARNING_OWNER_ATTRIBUTION,
        "Process ownership", "some owners could not be read (permissions)", &error) == OD_OK);
    OdResolutionItem item = {.automatic = true, .old_port = 5500U, .new_port = 5501U,
        .relative_path = "./.env", .variable = "PORT", .source_kind = OD_SOURCE_ENV};
    OdResolution resolution = {.items = &item, .count = 1U, .automatic_count = 1U};
    OdCanvas canvas;
    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    od_render_conflicts_report(&canvas, &resolution, 1U, 0U, true,
        "Scan incomplete (1 warning)", false, &report);
    char *text = rendered_text(&canvas);
    CHECK(substring_count(text, "Scan incomplete (1 warning)") == 1U);
    CHECK(strstr(text, "PROPOSED") != NULL && strstr(text, "[Apply]") != NULL);
    CHECK(canvas_cell(&canvas, 2U, 2U)->role == OD_ROLE_WARNING);
    free(text);
    /* De-duplication must not hide operation outcomes or real errors. */
    const char *statuses[] = {"Updated 1 port(s)", "Source file changed; refresh conflicts"};
    for (size_t i = 0U; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        od_render_conflicts_report(&canvas, &resolution, 1U, 0U, true,
            statuses[i], false, &report);
        text = rendered_text(&canvas);
        CHECK(substring_count(text, "Scan incomplete (1 warning)") == 1U);
        CHECK(strstr(text, statuses[i]) != NULL);
        free(text);
    }
    od_canvas_free(&canvas);
    od_scan_report_free(&report);
}

static void test_resolve_empty_state_has_no_table(void) {
    OdError error;
    OdScanReport report = {0};
    CHECK(od_scan_report_add(&report, OD_WARNING_OWNER_ATTRIBUTION,
        "Process ownership", "some owners could not be read (permissions)", &error) == OD_OK);
    OdResolution empty = {0};
    const size_t widths[] = {60U, 78U, 79U, 90U, 91U, 160U};
    for (size_t w = 0U; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (size_t mode = 0U; mode < 2U; ++mode) {
            OdCanvas canvas;
            CHECK(od_canvas_init(&canvas, widths[w], 18U, &error) == OD_OK);
            for (size_t state = 0U; state < 3U; ++state) {
                const OdScanReport *warnings = state == 0U ? &report : NULL;
                od_render_conflicts_report(&canvas, &empty, 0U, 0U, state != 2U,
                    state == 0U ? "Scan incomplete (1 warning)" : NULL, mode == 0U, warnings);
                char *text = rendered_text(&canvas);
                CHECK(strstr(text, "PROPOSED") == NULL && strstr(text, "SOURCE") == NULL);
                CHECK(strstr(text, "[Apply]") == NULL);
                const char *message = state == 0U ? "No conflicts detected in scanned data." :
                    state == 1U ? "No conflicts found" : "Unable to prepare proposal";
                CHECK(strstr(text, message) != NULL);
                if (state == 0U) {
                    CHECK(substring_count(text, "Scan incomplete (1 warning)") == 1U);
                    CHECK(strstr(text, "No conflicts found") == NULL);
                }
                /* Check canvas cells too: an empty state must have no leftover
                 * ASCII or Unicode table rules, including above/below it. */
                bool has_table_border = false, has_apply_target = false;
                for (size_t y = 4U; y < canvas.height - 5U; ++y) {
                    for (size_t x = 0U; x < canvas.width; ++x) {
                        const OdCell *cell = canvas_cell(&canvas, x, y);
                        if (cell->role == OD_ROLE_FOCUSED_BORDER) has_table_border = true;
                        if (od_conflicts_report_mouse_target(canvas.width, canvas.height,
                            &empty, 0U, 0U, warnings, (int)x, (int)y).action == OD_MOUSE_CONFLICT_APPLY)
                            has_apply_target = true;
                    }
                }
                CHECK(!has_table_border && !has_apply_target);
                free(text);
            }
            od_canvas_free(&canvas);
        }
    }
    od_scan_report_free(&report);
}

static void test_warning_document_scroll_and_hit_targets(void) {
    OdError error;
    OdScanReport report = {0};
    for (size_t i = 0U; i < OD_SCAN_WARNING_CAP + 5U; ++i)
        CHECK(od_scan_report_add(&report, OD_WARNING_DISCOVERY,
                                 "./package.json", "invalid JSON", &error) == OD_OK);
    OdResolutionItem items[6] = {0};
    for (size_t i = 0U; i < 6U; ++i) {
        items[i] = (OdResolutionItem){.allocation_index = i, .automatic = true,
            .old_port = 5500U, .new_port = 5501U, .relative_path = "./.env",
            .variable = "PORT", .source_kind = OD_SOURCE_ENV};
    }
    OdResolution resolution = {.items = items, .count = 6U, .automatic_count = 6U};
    const size_t widths[] = {60U, 78U, 79U, 90U, 91U, 160U};
    for (size_t w = 0U; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        OdCanvas canvas;
        CHECK(od_canvas_init(&canvas, widths[w], 18U, &error) == OD_OK);
        size_t selected = od_scan_warning_rows(&report);
        size_t scroll = od_conflicts_report_scroll(&resolution, &report, selected, 0U, 18U);
        od_render_conflicts_report(&canvas, &resolution, selected, scroll, true, NULL, true, &report);
        char *text = rendered_text(&canvas);
        CHECK(strstr(text, "and 5 more") != NULL);
        CHECK(strstr(text, "[Apply]") != NULL);
        CHECK(strstr(text, "Scan incomplete (205 warnings)") != NULL);
        free(text);
        bool actionable = false;
        for (int y = 4; y < 14; ++y) {
            OdMouseTarget target = od_conflicts_report_mouse_target(widths[w], 18U,
                &resolution, selected, scroll, &report, (int)widths[w] - 10, y);
            if (target.action != OD_MOUSE_CONFLICT_APPLY) continue;
            actionable = true;
            CHECK(strcmp(canvas_cell(&canvas, widths[w] - 10U, (size_t)y)->glyph, "[") == 0);
        }
        CHECK(actionable);
        /* Also exercise the next group just below the viewport. Blank space
         * before the page count must never act as an invisible Apply button. */
        selected = OD_SCAN_WARNING_CAP - 3U;
        scroll = selected;
        scroll = od_conflicts_report_scroll(&resolution, &report, selected, scroll, 18U);
        od_render_conflicts_report(&canvas, &resolution, selected, scroll, true, NULL, true, &report);
        CHECK(od_conflicts_report_mouse_target(widths[w], 18U, &resolution, selected,
            scroll, &report, (int)widths[w] - 10, 13).action == OD_MOUSE_NONE);
        OdResolution empty = {0};
        selected = od_scan_warning_rows(&report) - 1U;
        scroll = od_conflicts_report_scroll(&empty, &report, selected, 0U, 18U);
        od_render_conflicts_report(&canvas, &empty, selected, scroll, true, NULL, true, &report);
        text = rendered_text(&canvas);
        CHECK(strstr(text, "No conflicts detected in scanned data.") != NULL);
        CHECK(strstr(text, "and 5 more") != NULL);
        free(text);
        od_canvas_free(&canvas);
    }
    od_scan_report_free(&report);
}

static void test_conflict_table_has_row_actions(void) {
    OdResolutionItem items[] = {
        {.allocation_index = 0U, .old_port = 5500U, .new_port = 5501U,
         .automatic = true, .source_kind = OD_SOURCE_ENV,
         .variable = "PORT", .relative_path = "./.env",
         .line_before = "- PORT=5500", .line_after = "+ PORT=5501"},
        {.allocation_index = 0U, .old_port = 5500U, .new_port = 5501U,
         .source_kind = OD_SOURCE_COMPOSE, .relative_path = "./compose.yaml"},
        {.allocation_index = 1U, .old_port = 6000U, .new_port = 6001U,
         .source_kind = OD_SOURCE_PACKAGE_JSON, .relative_path = "./package.json",
         .manual_reason = "package scripts are manual-only"}
    };
    OdResolution resolution = {.items = items, .count = 3U,
                               .automatic_count = 1U, .manual_count = 2U};
    const size_t widths[] = {60U, 78U, 79U, 90U, 91U, 160U};
    for (size_t w = 0U; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        OdCanvas canvas;
        OdError error;
        CHECK(od_canvas_init(&canvas, widths[w], 18U, &error) == OD_OK);
        od_render_conflicts(&canvas, &resolution, 0U, 0U, true, "Ready", true);
        char *text = rendered_text(&canvas);
        CHECK(text != NULL && strstr(text, "PROPOSED") != NULL);
        CHECK(text != NULL && strstr(text, "SOURCE") != NULL);
        CHECK(text != NULL && strstr(text, "ACTION") != NULL);
        CHECK(text != NULL && strstr(text, "[Apply]") != NULL);
        CHECK(text != NULL && strstr(text, "Manual") != NULL);
        CHECK(text != NULL && strstr(text, "Showing 1-2 of 2 conflict(s)") != NULL);
        CHECK(text != NULL && strstr(text, "env (PORT)") != NULL);
        CHECK(text != NULL && strstr(text, "Automatic changes") == NULL);
        CHECK(text != NULL && strstr(text, "Apply all") == NULL);
        CHECK(text != NULL && (strstr(text, "RELATIVE FOLDER") != NULL) == (widths[w] >= 91U));
        CHECK(text != NULL && (strstr(text, "PROCESS") != NULL) == (widths[w] >= 79U));
        OdCanvas header = {canvas.width, 1U, canvas.cells + 5U * canvas.width};
        char *heading = rendered_text(&header);
        if (heading != NULL) {
            const char *port = strstr(heading, "PORT");
            const char *proposed = strstr(heading, "PROPOSED");
            const char *source = strstr(heading, "SOURCE");
            const char *action = strstr(heading, "ACTION");
            CHECK(port != NULL && proposed != NULL && source != NULL && action != NULL &&
                  port < proposed && proposed < source && source < action);
            free(heading);
        }
        if (widths[w] == 160U) {
            CHECK(strstr(text, "env (PORT) — ./.env, ./compose.yaml") != NULL);
        }
        /* Every interior cell on the selected row, including separators and
         * the Apply label, must share the selected background. */
        for (size_t x = 2U; x < widths[w] - 2U; ++x) {
            CHECK(canvas.cells[7U * canvas.width + x].role == OD_ROLE_SELECTED);
        }

        OdMouseTarget target = od_conflicts_mouse_target(widths[w], 18U, &resolution,
                                                          0U, 0U, (int)widths[w] - 10, 7);
        CHECK(target.action == OD_MOUSE_CONFLICT_APPLY && target.item == 0U);
        target = od_conflicts_mouse_target(widths[w], 18U, &resolution,
                                           0U, 0U, (int)widths[w] - 3, 7);
        CHECK(target.action == OD_MOUSE_CONFLICT_ROW);
        target = od_conflicts_mouse_target(widths[w], 18U, &resolution, 0U, 0U, 5, 9);
        CHECK(target.action == OD_MOUSE_CONFLICT_ROW && target.item == 1U);
        target = od_conflicts_mouse_target(widths[w], 18U, &resolution,
                                           0U, 0U, (int)widths[w] - 10, 9);
        CHECK(target.action == OD_MOUSE_CONFLICT_ROW && target.item == 1U);
        target = od_conflicts_mouse_target(widths[w], 18U, &resolution, 0U, 0U, 5, 8);
        CHECK(target.action == OD_MOUSE_NONE);
        CHECK(canvas.cells[7U * canvas.width + widths[w] - 10U].attributes == 1U);
        free(text);
        od_render_conflicts(&canvas, &resolution, 1U, 0U, true, "Ready", true);
        text = rendered_text(&canvas);
        CHECK(text != NULL && strstr(text, "package scripts are manual-only") != NULL);
        CHECK(text != NULL && strstr(text, "Enter Apply") == NULL);
        free(text);
        OdResolution empty = {0};
        od_render_conflicts(&canvas, &empty, 0U, 0U, true, NULL, true);
        text = rendered_text(&canvas);
        CHECK(text != NULL && strstr(text, "No conflicts found") != NULL);
        free(text);
        od_render_conflicts(&canvas, &empty, 0U, 0U, false, "Scan failed", true);
        text = rendered_text(&canvas);
        CHECK(text != NULL && strstr(text, "Unable to prepare proposal") != NULL);
        CHECK(text != NULL && strstr(text, "No conflicts found") == NULL);
        free(text);
        od_canvas_free(&canvas);
    }
}

static char *rendered_line(OdCanvas *canvas, size_t y) {
    OdCanvas line = {canvas->width, 1U, canvas->cells + y * canvas->width};
    return rendered_text(&line);
}

static void history_fixture(OdHistoryRecord records[12], OdHistory *history) {
    static char *paths[] = {
        "./row-00/.env", "./row-01/.env", "./row-02/.env", "./row-03/.env",
        "./row-04/.env", "./row-05/.env", "./row-06/.env", "./row-07/.env",
        "./row-08/.env", "./row-09/.env", "./row-10/.env", "./row-11/.env"
    };
    for (size_t index = 0U; index < 12U; ++index) {
        records[index] = (OdHistoryRecord){
            .id = 120U - index,
            .timestamp = "2026-10-06T09:30:18.715Z",
            .relative_path = paths[index],
            .source_kind = index == 2U ? OD_SOURCE_COMPOSE : OD_SOURCE_ENV,
            .environment_key = "PORT",
            .old_port = (uint16_t)(2000U + index),
            .new_port = (uint16_t)(3000U + index),
            .availability = index % 2U == 0U ? OD_HISTORY_READY : OD_HISTORY_UNAVAILABLE,
            .reason = index % 2U == 0U ? NULL :
                "The declaration moved; its exact byte span no longer matches. "
                "Restore the recorded location before trying again."
        };
        if (index == 2U) records[index].environment_key = NULL;
    }
    *history = (OdHistory){.items = records, .count = 12U};
}

/* Catches raw timestamps, mismatched Dashboard vocabulary, wrapped reasons,
 * unequal row heights, and selected-row seams or nested action backgrounds. */
static void test_history_rows_are_uniform_and_reasons_stay_in_status(void) {
    OdHistoryRecord records[12];
    OdHistory history;
    history_fixture(records, &history);
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 240U, 32U, &error) == OD_OK);
    od_render_history(&canvas, &history, 1U, 0U, "History loaded", NULL, true);
    char *text = rendered_text(&canvas);
    CHECK(strstr(text, "OPENDOOR / History") != NULL);
    CHECK(strstr(text, "WHEN") != NULL);
    CHECK(strstr(text, "RELATIVE FOLDER") != NULL);
    CHECK(strstr(text, "SOURCE") != NULL);
    CHECK(strstr(text, "CHANGE") != NULL);
    CHECK(strstr(text, "ACTION / STATUS") != NULL);
    CHECK(strstr(text, "FILE / KEY") == NULL);
    CHECK(strstr(text, "2026-10-06 09:30:18") != NULL);
    CHECK(strstr(text, "2026-10-06T09:30:18.715Z") == NULL);
    CHECK(strstr(text, "Showing 1-10 of 12 change(s)") != NULL);
    CHECK(substring_count(text, records[1].reason) == 1U);
    free(text);
    char *header = rendered_line(&canvas, 5U);
    const char *when_header = strstr(header, "WHEN");
    const char *folder_header = strstr(header, "RELATIVE FOLDER");
    const char *source_header = strstr(header, "SOURCE");
    const char *change_header = strstr(header, "CHANGE");
    const char *action_header = strstr(header, "ACTION / STATUS");
    CHECK(when_header != NULL && folder_header != NULL && source_header != NULL &&
          change_header != NULL && action_header != NULL);
    if (when_header != NULL && folder_header != NULL && source_header != NULL &&
        change_header != NULL && action_header != NULL) {
        CHECK(when_header < folder_header && folder_header < source_header &&
              source_header < change_header && change_header < action_header);
    }
    free(header);
    for (size_t index = 0U; index < 10U; ++index) {
        size_t y = 7U + index * 2U;
        char *line = rendered_line(&canvas, y);
        char expected[64];
        (void)snprintf(expected, sizeof(expected), "%u -> %u",
                       (unsigned)records[index].old_port,
                       (unsigned)records[index].new_port);
        char folder[32];
        (void)snprintf(folder, sizeof(folder), "./row-%02zu", index);
        CHECK(strstr(line, folder) != NULL);
        CHECK(strstr(line, records[index].relative_path) == NULL);
        CHECK(strstr(line, index == 2U ? "compose" : "env (PORT)") != NULL);
        CHECK(strstr(line, "2026-10-06 09:30:18") != NULL);
        CHECK(strstr(line, "2026-10-06T09:30:18.715Z") == NULL);
        CHECK(strstr(line, expected) != NULL);
        const char *action = index % 2U == 0U ? "[Revert]" : "Unavailable";
        CHECK(strstr(line, action) != NULL);
        CHECK(strstr(line, "declaration") == NULL);
        CHECK(strstr(line, "Restore") == NULL);
        CHECK(strcmp(canvas_cell(&canvas, 222U, y)->glyph,
                     index % 2U == 0U ? "[" : "U") == 0);
        CHECK(strcmp(canvas_cell(&canvas, 222U + strlen(action), y)->glyph, " ") == 0);
        for (size_t glyph = 0U; glyph < strlen(action); ++glyph) {
            const OdCell *cell = canvas_cell(&canvas, 222U + glyph, y);
            OdStyleRole expected_role = index == 1U ? OD_ROLE_SELECTED :
                (index % 2U == 0U ? OD_ROLE_PRIMARY : OD_ROLE_DANGER);
            CHECK(cell->role == expected_role);
            CHECK(cell->attributes == 1U);
        }
        CHECK(strcmp(canvas_cell(&canvas, 1U, y + 1U)->glyph, "+") == 0);
        char *separator = rendered_line(&canvas, y + 1U);
        CHECK(strstr(separator, "Unavailable") == NULL);
        CHECK(strstr(separator, "declaration") == NULL);
        free(separator);
        free(line);
    }
    for (size_t y = 0U; y < 30U; ++y) {
        char *line = rendered_line(&canvas, y);
        CHECK(strstr(line, "The declaration moved") == NULL);
        CHECK(strstr(line, "Restore the recorded location") == NULL);
        free(line);
    }
    char *detail = rendered_line(&canvas, 30U);
    CHECK(strstr(detail, "./row-01/.env | PORT | 2001 -> 3001") != NULL);
    CHECK(strstr(detail, records[1].reason) != NULL);
    CHECK(canvas_cell(&canvas, 3U, 9U)->role == OD_ROLE_SELECTED);
    for (size_t x = 2U; x <= 237U; ++x) {
        CHECK(canvas_cell(&canvas, x, 9U)->role == OD_ROLE_SELECTED);
    }
    free(detail);

    od_render_history(&canvas, &history, 0U, 0U, NULL, NULL, false);
    for (size_t glyph = 0U; glyph < 8U; ++glyph) {
        CHECK(canvas_cell(&canvas, 222U + glyph, 7U)->role == OD_ROLE_SELECTED);
        CHECK(canvas_cell(&canvas, 222U + glyph, 7U)->attributes == 1U);
    }
    for (size_t x = 2U; x <= 237U; ++x) {
        CHECK(canvas_cell(&canvas, x, 7U)->role == OD_ROLE_SELECTED);
    }
    text = rendered_text(&canvas);
    CHECK(strstr(text, "┼") != NULL);
    CHECK(strstr(text, "The declaration moved") == NULL);
    free(text);
    od_canvas_free(&canvas);
}

/* Catches visual-line pagination, off-by-one row indexes, and text escaping
 * its cell or the terminal at the existing minimum size. */
static void test_history_pagination_clipping_and_empty_state(void) {
    OdHistoryRecord records[12];
    OdHistory history;
    history_fixture(records, &history);
    CHECK(od_history_page_size(18U) == 3U);
    CHECK(od_history_page_size(32U) == 10U);
    CHECK(od_history_visible_scroll(12U, 3U, 0U, 18U) == 1U);
    CHECK(od_history_visible_scroll(12U, 0U, 9U, 18U) == 0U);
    CHECK(od_history_visible_scroll(12U, 11U, 0U, 18U) == 9U);
    CHECK(od_history_visible_scroll(12U, SIZE_MAX, SIZE_MAX, 18U) == 9U);
    CHECK(od_history_visible_scroll(0U, SIZE_MAX, SIZE_MAX, 18U) == 0U);
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 60U, 18U, &error) == OD_OK);
    od_render_history(&canvas, &history, 11U, SIZE_MAX, NULL, NULL, true);
    char *text = rendered_text(&canvas);
    CHECK(strstr(text, "Showing 10-12 of 12 change(s)") != NULL);
    CHECK(strstr(text, "2009 -> 3009") != NULL);
    CHECK(strstr(text, "2010 -> 3010") != NULL);
    CHECK(strstr(text, "2011 -> 3011") != NULL);
    CHECK(strstr(text, "2008 -> 3008") == NULL);
    CHECK(strstr(text, "STATUS") != NULL);
    CHECK(strstr(text, "RELATIVE FOLDER") == NULL);
    CHECK(strstr(text, "FILE / KEY") == NULL);
    CHECK(strcmp(canvas_cell(&canvas, 58U, 7U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 47U, 7U)->glyph, "U") == 0);
    free(text);
    records[10].relative_path = "./a-very-long-directory/another-directory/file.env";
    records[10].environment_key = "A_VERY_LONG_ENVIRONMENT_KEY";
    od_render_history(&canvas, &history, 10U, 9U, NULL, NULL, true);
    CHECK(strcmp(canvas_cell(&canvas, 20U, 9U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 32U, 9U)->glyph, "2") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 47U, 9U)->glyph, "[") == 0);
    char *warnings[] = {"Skipped malformed history line"};
    OdHistory empty = {.warnings = warnings, .warning_count = 1U};
    od_render_history(&canvas, &empty, 0U, 0U, NULL, NULL, true);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "No history yet") != NULL);
    CHECK(strstr(text, "Skipped malformed history line") != NULL);
    CHECK(strstr(text, "Showing 0-0 of 0 change(s)") != NULL);
    CHECK(strstr(text, "Enter Revert") == NULL);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 79U, 18U, &error) == OD_OK);
    od_render_history(&canvas, &history, 0U, 0U, NULL, NULL, true);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "RELATIVE FOLDER") != NULL);
    CHECK(strstr(text, "ACTION / STATUS") != NULL);
    CHECK(strcmp(canvas_cell(&canvas, 63U, 7U)->glyph, "[") == 0);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 91U, 18U, &error) == OD_OK);
    od_render_history(&canvas, &history, 0U, 0U, NULL, NULL, true);
    CHECK(strcmp(canvas_cell(&canvas, 73U, 7U)->glyph, "[") == 0);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 59U, 17U, &error) == OD_OK);
    od_render_history(&canvas, &history, 0U, 0U, NULL, NULL, true);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "Terminal too small") != NULL);
    CHECK(strstr(text, "[Revert]") == NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void check_history_target(size_t width, size_t height, const OdHistory *history,
                                 size_t selected, size_t scroll, bool confirming,
                                 int x, int y, OdMouseAction action, size_t item) {
    OdMouseTarget target = od_history_mouse_target(width, height, history,
                                                   selected, scroll, confirming, x, y);
    CHECK(target.action == action);
    if (action == OD_MOUSE_HISTORY_ROW || action == OD_MOUSE_HISTORY_REVERT) {
        CHECK(target.item == item);
    }
}

/* Catches broad action hit boxes, unavailable writes, separator/empty-row
 * selection, and underlying list controls leaking into confirmation. */
static void test_history_targets_match_only_visible_actions(void) {
    OdHistoryRecord records[12];
    OdHistory history;
    history_fixture(records, &history);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 20, 7,
                          OD_MOUSE_HISTORY_ROW, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 81, 7,
                          OD_MOUSE_HISTORY_ROW, 0U);
    for (int x = 82; x < 90; ++x) {
        check_history_target(100U, 32U, &history, 0U, 0U, false, x, 7,
                              OD_MOUSE_HISTORY_REVERT, 0U);
    }
    check_history_target(100U, 32U, &history, 0U, 0U, false, 90, 7,
                          OD_MOUSE_HISTORY_ROW, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 82, 9,
                          OD_MOUSE_HISTORY_ROW, 1U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 82, 8,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 1, 7,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 98, 7,
                          OD_MOUSE_NONE, 0U);
    check_history_target(60U, 18U, &history, 10U, 9U, false, 47, 9,
                          OD_MOUSE_HISTORY_REVERT, 10U);
    check_history_target(60U, 18U, &history, 10U, 9U, false, 42, 13,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 18, 31,
                          OD_MOUSE_HISTORY_REVERT, 0U);
    check_history_target(100U, 32U, &history, 1U, 0U, false, 18, 31,
                          OD_MOUSE_REFRESH, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 33, 31,
                          OD_MOUSE_REFRESH, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, false, 45, 31,
                          OD_MOUSE_BACK, 0U);
    check_history_target(100U, 32U, NULL, 0U, 0U, false, 20, 7,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 82, 7,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 32, 31,
                          OD_MOUSE_BACK, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 1, 31,
                          OD_MOUSE_HISTORY_CONFIRM, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 16, 31,
                          OD_MOUSE_HISTORY_CONFIRM, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 17, 31,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, -1, 31,
                          OD_MOUSE_NONE, 0U);
    check_history_target(100U, 32U, &history, 0U, 0U, true, 100, 31,
                          OD_MOUSE_NONE, 0U);
    check_history_target(59U, 18U, &history, 0U, 0U, true, 1, 17,
                          OD_MOUSE_NONE, 0U);
    check_history_target(60U, 17U, &history, 0U, 0U, false, 42, 7,
                          OD_MOUSE_NONE, 0U);
}

static void test_history_confirmation_names_exact_reverse_operation(void) {
    OdHistoryRecord records[12];
    OdHistory history;
    history_fixture(records, &history);
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    od_render_history_confirmation(&canvas, &records[0], true);
    char *text = rendered_text(&canvas);
    CHECK(strstr(text, "OPENDOOR / History") != NULL);
    CHECK(strstr(text, "Confirm revert #120") != NULL);
    CHECK(strstr(text, "./row-00/.env") != NULL);
    CHECK(strstr(text, "Key: PORT") != NULL);
    CHECK(strstr(text, "3000 -> 2000") != NULL);
    CHECK(strstr(text, "2000 -> 3000") == NULL);
    CHECK(strstr(text, "Enter does nothing") != NULL);
    CHECK(strstr(text, "y Confirm revert") != NULL);
    CHECK(strstr(text, "n/q/Esc Cancel") != NULL);
    CHECK(strstr(text, "WHEN") == NULL);
    free(text);
    od_canvas_free(&canvas);
    CHECK(od_canvas_init(&canvas, 59U, 17U, &error) == OD_OK);
    od_render_history_confirmation(&canvas, &records[0], true);
    text = rendered_text(&canvas);
    CHECK(strstr(text, "Terminal too small") != NULL);
    CHECK(strstr(text, "Confirm revert") == NULL);
    free(text);
    od_canvas_free(&canvas);
}

/* Refusal feedback must not rely on an exact match with loaded reasons: a
 * refresh error, changed classification, or removed record must stay below. */
static void test_history_refusal_and_refresh_errors_never_reach_top_line(void) {
    OdHistoryRecord records[12];
    OdHistory history;
    history_fixture(records, &history);
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 360U, 32U, &error) == OD_OK);
    const char *refusal = "Current port is 9999; expected 3001";
    const char *failure = "Current port is 9999; expected 3001; refresh failed: history log is unreadable";
    records[1].reason = "Source file is missing";
    const char *summary = "Revert failed for history #119; refresh failed";
    od_render_history(&canvas, &history, 1U, 0U, summary, failure, true);
    char *top = rendered_line(&canvas, 2U);
    CHECK(strstr(top, summary) != NULL);
    CHECK(strstr(top, refusal) == NULL);
    CHECK(strstr(top, "history log is unreadable") == NULL);
    free(top);
    char *bottom = rendered_line(&canvas, 30U);
    CHECK(strstr(bottom, "Source file is missing") != NULL);
    CHECK(strstr(bottom, failure) != NULL);
    free(bottom);

    records[1].availability = OD_HISTORY_READY;
    records[1].reason = NULL;
    od_render_history(&canvas, &history, 1U, 0U, summary, failure, true);
    top = rendered_line(&canvas, 2U);
    CHECK(strstr(top, refusal) == NULL);
    free(top);
    bottom = rendered_line(&canvas, 30U);
    CHECK(strstr(bottom, failure) != NULL);
    free(bottom);

    OdHistory empty = {0};
    od_render_history(&canvas, &empty, 0U, 0U, summary, failure, true);
    top = rendered_line(&canvas, 2U);
    CHECK(strstr(top, refusal) == NULL);
    free(top);
    bottom = rendered_line(&canvas, 30U);
    CHECK(strstr(bottom, failure) != NULL);
    free(bottom);
    od_canvas_free(&canvas);
}

int main(void) {
    test_interstitial_is_flushed_before_blocking_work();
    test_dashboard_loading_screen_explains_the_blocking_work();
    test_resolve_open_loading_screen_explains_the_blocking_work();
    test_resolve_apply_loading_screen_explains_the_blocking_work();
    test_history_loading_screen_explains_the_blocking_work();
    test_menu_dispatch_is_fixed_to_four_items();
    test_menu_degrades_at_minimum_size();
    test_update_is_cli_only_and_must_be_used_alone();
    test_mouse_targets_match_visible_controls();
    test_dashboard_is_read_only();
    test_dashboard_conflict_color_is_limited_to_status();
    test_dashboard_renders_declaration_names_and_live_processes();
    test_empty_dashboard_explains_supported_project_sources();
    test_scan_warning_views_and_apply_severity();
    test_resolve_summary_is_not_duplicated();
    test_resolve_empty_state_has_no_table();
    test_warning_document_scroll_and_hit_targets();
    test_conflict_table_has_row_actions();
    test_history_rows_are_uniform_and_reasons_stay_in_status();
    test_history_pagination_clipping_and_empty_state();
    test_history_targets_match_only_visible_actions();
    test_history_confirmation_names_exact_reverse_operation();
    test_history_refusal_and_refresh_errors_never_reach_top_line();
    if (failures != 0) {
        fprintf(stderr, "%d menu/screen checks failed\n", failures);
        return 1;
    }
    puts("menu/screen checks passed");
    return 0;
}
