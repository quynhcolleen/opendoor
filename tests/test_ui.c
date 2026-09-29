#include "opendoor/app.h"
#include "opendoor/screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

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

static void test_menu_dispatch_is_fixed_to_three_items(void) {
    CHECK(OD_MENU_COUNT == 3);
    CHECK(od_menu_dispatch(0U) == OD_MENU_DASHBOARD);
    CHECK(od_menu_dispatch(1U) == OD_MENU_RESOLVE_CONFLICTS);
    CHECK(od_menu_dispatch(2U) == OD_MENU_QUIT);
    CHECK(od_menu_dispatch(99U) == OD_MENU_QUIT);

    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_main_menu(&canvas, 1U, "/tmp/example/opendoor", "Ready", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Ports in this project") != NULL);
    CHECK(text != NULL && strstr(text, "Find and fix port clashes") != NULL);
    CHECK(text != NULL && strstr(text, "Pre-start port check - example/opendoor") != NULL);
    CHECK(text != NULL && strstr(text, "/tmp/example/opendoor") == NULL);
    CHECK(text != NULL &&
          strstr(text,
                 "+-----------------------|OpenDoor|-----------------------+") != NULL);
    CHECK(text != NULL && strstr(text, "Up/Down Navigate   Enter Select   q Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Discover this project") == NULL);
    CHECK(text != NULL && strstr(text, "Settings") == NULL);
    CHECK(strcmp(canvas_cell(&canvas, 11U, 8U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 68U, 14U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 35U, 8U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 44U, 8U)->glyph, "|") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 9U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 15U, 13U)->glyph, " ") == 0);
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
    CHECK(strcmp(canvas_cell(&canvas, 15U, 12U)->glyph, "Q") == 0);
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
    CHECK(strcmp(canvas_cell(&canvas, 36U, 12U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 67U, 10U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 20U, 15U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 20U, 16U)->role == OD_ROLE_PRIMARY);
    CHECK(canvas_cell(&canvas, 20U, 16U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 28U, 16U)->role == OD_ROLE_MUTED);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 17U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 37U, 18U)->role == OD_ROLE_MUTED);

    od_render_main_menu(&canvas, 2U, "/tmp/example/opendoor", "Ready", true);
    for (size_t x = 12U; x <= 67U; ++x) {
        CHECK(canvas_cell(&canvas, x, 12U)->role == OD_ROLE_SELECTED);
        CHECK(canvas_cell(&canvas, x, 11U)->role != OD_ROLE_SELECTED);
    }
    CHECK(canvas_cell(&canvas, 15U, 12U)->attributes == 1U);
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
    CHECK(strcmp(canvas_cell(&canvas, 78U, 21U)->glyph, "╯") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 16U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 25U, 20U)->glyph, " ") == 0);
    for (size_t x = 22U; x <= 77U; ++x) {
        CHECK(canvas_cell(&canvas, x, 17U)->role == OD_ROLE_SELECTED);
    }
    CHECK(strcmp(canvas_cell(&canvas, 22U, 17U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 17U)->glyph, "▶") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 18U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 23U, 19U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 25U, 17U)->attributes == 1U);
    CHECK(canvas_cell(&canvas, 46U, 17U)->role == OD_ROLE_SELECTED);
    CHECK(canvas_cell(&canvas, 46U, 17U)->attributes == 0U);
    CHECK(strcmp(canvas_cell(&canvas, 45U, 17U)->glyph, " ") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 28U, 22U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 32U, 23U)->role == OD_ROLE_PRIMARY);
    CHECK(canvas_cell(&canvas, 35U, 23U)->role == OD_ROLE_MUTED);
    CHECK(strcmp(canvas_cell(&canvas, 35U, 24U)->glyph, " ") == 0);
    CHECK(canvas_cell(&canvas, 47U, 25U)->role == OD_ROLE_MUTED);
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
    od_render_main_menu(&canvas, 2U,
                        "/tmp/a-project-with-a-very-long-directory-name-for-menu-testing",
                        "Run before starting project services", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Ports in this project") == NULL);
    CHECK(text != NULL && strstr(text, "Find and fix port clashes") == NULL);
    CHECK(text != NULL && strstr(text, "Run before starting project services") == NULL);
    CHECK(text != NULL && strstr(text, "...") != NULL);
    CHECK(strcmp(canvas_cell(&canvas, 2U, 6U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 57U, 12U)->glyph, "+") == 0);
    CHECK(strcmp(canvas_cell(&canvas, 4U, 10U)->glyph, ">") == 0);
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
    target = od_menu_mouse_target(100U, 32U, false, 40, 16);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(100U, 32U, false, 40, 20);
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
    target = od_menu_mouse_target(60U, 18U, true, 20, 7);
    CHECK(target.action == OD_MOUSE_NONE);
    target = od_menu_mouse_target(60U, 18U, true, 20, 11);
    CHECK(target.action == OD_MOUSE_NONE);

    target = od_dashboard_mouse_target(80U, 24U, 18, 23);
    CHECK(target.action == OD_MOUSE_REFRESH);
    target = od_dashboard_mouse_target(80U, 24U, 32, 23);
    CHECK(target.action == OD_MOUSE_BACK);
    target = od_dashboard_mouse_target(80U, 24U, 10, 10);
    CHECK(target.action == OD_MOUSE_NONE);

    target = od_conflicts_mouse_target(80U, 24U, true, 18, 23);
    CHECK(target.action == OD_MOUSE_APPLY_ALL);
    target = od_conflicts_mouse_target(80U, 24U, true, 37, 23);
    CHECK(target.action == OD_MOUSE_BACK);
    target = od_conflicts_mouse_target(80U, 24U, false, 18, 23);
    CHECK(target.action == OD_MOUSE_BACK);
}

static void test_dashboard_is_read_only(void) {
    OdPortRow rows[] = {
        {
            .port = 3000U,
            .status = OD_PORT_RUNNING,
            .declared = true,
            .relative_folder = "./services/api",
            .source = "./services/api/.env"
        },
        {
            .port = 4000U,
            .status = OD_PORT_NOT_RUNNING,
            .declared = true,
            .relative_folder = "./services/web",
            .source = "./services/web/compose.yaml"
        },
        {
            .port = 5000U,
            .status = OD_PORT_IN_USE_OTHER,
            .declared = true,
            .relative_folder = "./services/外部",
            .source = "./services/外部/.env"
        }
    };
    OdDashboard dashboard = {
        .rows = rows,
        .count = 3U,
        .page_size = 10U
    };
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 120U, 30U, &error) == OD_OK);
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

static void test_dashboard_renders_compact_process_names(void) {
    char project_root[OD_PATH_CAP];
    CHECK(getcwd(project_root, sizeof(project_root)) != NULL);

    OdPortDeclaration declaration = {
        .port = 37955U,
        .relative_path = "./services/backend/config/.env.production",
        .relative_folder = "./services/backend"
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

static void test_conflicts_have_one_whole_plan_action(void) {
    OdResolutionItem items[] = {
        {
            .declaration_index = 0U,
            .old_port = 3000U,
            .new_port = 3001U,
            .automatic = true,
            .source_kind = OD_SOURCE_ENV,
            .write_kind = OD_WRITE_ENV_LITERAL,
            .line = 2U,
            .relative_path = "./.env",
            .line_before = "- API_PORT=3000",
            .line_after = "+ API_PORT=3001",
            .manual_reason = ""
        },
        {
            .declaration_index = 1U,
            .old_port = 8080U,
            .new_port = 8081U,
            .automatic = true,
            .source_kind = OD_SOURCE_COMPOSE,
            .write_kind = OD_WRITE_COMPOSE_LITERAL,
            .line = 5U,
            .relative_path = "./compose.yaml",
            .line_before = "-       - \"8080:80\"",
            .line_after = "+       - \"8081:80\"",
            .manual_reason = ""
        },
        {
            .declaration_index = 2U,
            .old_port = 5173U,
            .new_port = 5174U,
            .automatic = false,
            .source_kind = OD_SOURCE_PACKAGE_JSON,
            .write_kind = OD_WRITE_MANUAL_ONLY,
            .line = 3U,
            .relative_path = "./package.json",
            .line_before = "-     \"dev\": \"vite --port 5173\"",
            .line_after = "+     \"dev\": \"vite --port 5174\"",
            .manual_reason = "package.json scripts are manual-only"
        }
    };
    OdResolution resolution = {
        .items = items,
        .count = 3U,
        .automatic_count = 2U,
        .manual_count = 1U
    };
    CHECK(od_resolution_visual_line_count(&resolution) >= 10U);
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    od_render_conflicts(&canvas, &resolution, 0U, true, true,
                        "Ready to apply", false);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Automatic changes") != NULL);
    CHECK(text != NULL && strstr(text, "./.env") != NULL);
    CHECK(text != NULL && strstr(text, "- API_PORT=3000") != NULL);
    CHECK(text != NULL && strstr(text, "+ API_PORT=3001") != NULL);
    CHECK(text != NULL && strstr(text, "./compose.yaml") != NULL);
    CHECK(text != NULL && strstr(text, "Manual suggestions") != NULL);
    CHECK(text != NULL && strstr(text, "not applied automatically") != NULL);
    CHECK(text != NULL && strstr(text, "change line 3 in ./package.json to 5174") != NULL);
    CHECK(text != NULL && strstr(text, "package.json scripts are manual-only") != NULL);
    CHECK(text != NULL && strstr(text, "┌") != NULL);
    CHECK(text != NULL && strstr(text, "└") != NULL);
    CHECK(text != NULL && strstr(text, "Enter Apply all") != NULL);
    CHECK(text != NULL && strstr(text, "q/Esc Cancel") != NULL);
    CHECK(text != NULL && strstr(text, "┌") != NULL);
    CHECK(text != NULL && strstr(text, "Edit") == NULL);
    CHECK(text != NULL && strstr(text, "Override") == NULL);
    free(text);

    od_render_conflicts(&canvas, &resolution, 0U, false, true,
                        "Updated 2 port(s); 1 manual suggestion(s)", true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Updated 2 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "Enter Apply all") == NULL);
    free(text);

    OdResolution manual_only = {
        .items = &items[2],
        .count = 1U,
        .manual_count = 1U
    };
    od_render_conflicts(&canvas, &manual_only, 0U, true, true,
                        "Manual suggestion only", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Manual suggestions") != NULL);
    CHECK(text != NULL && strstr(text, "Enter Apply all") == NULL);
    free(text);

    od_canvas_free(&canvas);
    CHECK(od_canvas_init(&canvas, 60U, 18U, &error) == OD_OK);
    size_t visual_lines = od_resolution_visual_line_count(&resolution);
    size_t last_scroll = visual_lines > 3U ? visual_lines - 3U : 0U;
    od_render_conflicts(&canvas, &resolution, last_scroll, true, true,
                        "Ready", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "package.json scripts are manual-only") != NULL);
    CHECK(text != NULL && strstr(text, "Showing") != NULL);
    free(text);

    resolution = (OdResolution){0};
    od_render_conflicts(&canvas, &resolution, 0U, false, true, NULL, true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "No conflicts found") != NULL);
    CHECK(text != NULL && strstr(text, "OLD PORT") == NULL);
    free(text);

    od_render_conflicts(&canvas, &resolution, 0U, false, false,
                        "Unable to scan sockets", true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Unable to prepare proposal") != NULL);
    CHECK(text != NULL && strstr(text, "No conflicts found") == NULL);
    free(text);
    od_canvas_free(&canvas);
}

int main(void) {
    test_menu_dispatch_is_fixed_to_three_items();
    test_menu_degrades_at_minimum_size();
    test_update_is_cli_only_and_must_be_used_alone();
    test_mouse_targets_match_visible_controls();
    test_dashboard_is_read_only();
    test_dashboard_renders_compact_process_names();
    test_empty_dashboard_explains_supported_project_sources();
    test_conflicts_have_one_whole_plan_action();
    if (failures != 0) {
        fprintf(stderr, "%d menu/screen checks failed\n", failures);
        return 1;
    }
    puts("menu/screen checks passed");
    return 0;
}
