#include "opendoor/app.h"
#include "opendoor/screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void test_menu_dispatch_is_fixed_to_three_items(void) {
    CHECK(OD_MENU_COUNT == 3);
    CHECK(od_menu_dispatch(0U) == OD_MENU_DASHBOARD);
    CHECK(od_menu_dispatch(1U) == OD_MENU_RESOLVE_CONFLICTS);
    CHECK(od_menu_dispatch(2U) == OD_MENU_QUIT);
    CHECK(od_menu_dispatch(99U) == OD_MENU_QUIT);

    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_main_menu(&canvas, 1U, ".", "Ready", true);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Dashboard") != NULL);
    CHECK(text != NULL && strstr(text, "Resolve conflicts") != NULL);
    CHECK(text != NULL && strstr(text, "Quit") != NULL);
    CHECK(text != NULL && strstr(text, "Discover this project") == NULL);
    CHECK(text != NULL && strstr(text, "Settings") == NULL);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 100U, 32U, &error) == OD_OK);
    od_render_main_menu(&canvas, 0U, ".", "Ready", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "▄▄▄▄") != NULL);
    CHECK(text != NULL && strstr(text, "┌") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_removed_update_and_help_are_unknown_options(void) {
    char *update_arguments[] = {"opendoor", "--update", NULL};
    OpendoorOptions options;
    CHECK(opendoor_parse_args(2, update_arguments, &options) == 2);

    char *help_arguments[] = {"opendoor", "--help", NULL};
    CHECK(opendoor_parse_args(2, help_arguments, &options) == 2);

    char *normal_arguments[] = {
        "opendoor", "--project", ".", "--ascii", NULL
    };
    CHECK(opendoor_parse_args(4, normal_arguments, &options) == 0);
    CHECK(options.project_path != NULL && strcmp(options.project_path, ".") == 0);
    CHECK(options.force_ascii);
}

static void test_mouse_targets_match_visible_controls(void) {
    OdMouseTarget target = od_menu_mouse_target(80U, 24U, false, 20, 12);
    CHECK(target.action == OD_MOUSE_MENU_ITEM);
    CHECK(target.item == 1U);

    target = od_menu_mouse_target(80U, 24U, false, 2, 2);
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
    CHECK(text != NULL && strstr(text, "PROCESS") == NULL);
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
    test_removed_update_and_help_are_unknown_options();
    test_mouse_targets_match_visible_controls();
    test_dashboard_is_read_only();
    test_conflicts_have_one_whole_plan_action();
    if (failures != 0) {
        fprintf(stderr, "%d menu/screen checks failed\n", failures);
        return 1;
    }
    puts("menu/screen checks passed");
    return 0;
}
