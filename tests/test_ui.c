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

static void test_update_is_a_cli_only_action(void) {
    char *update_arguments[] = {"opendoor", "--update", NULL};
    OpendoorOptions options;
    CHECK(opendoor_parse_args(2, update_arguments, &options) == 0);
    CHECK(options.update_requested);

    char *mixed_arguments[] = {"opendoor", "--update", "--ascii", NULL};
    CHECK(opendoor_parse_args(3, mixed_arguments, &options) == 2);
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
            .running = true,
            .conflict = true,
            .process = "api-server",
            .directory = "/work/services/api"
        },
        {
            .port = 4000U,
            .running = false,
            .conflict = false,
            .process = "-",
            .directory = "-"
        },
        {
            .port = 5000U,
            .running = true,
            .conflict = false,
            .process = "🚪🚪🚪🚪🚪🚪",
            .directory = "/work/services/unicode"
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
    od_render_dashboard(&canvas, &dashboard, ".ports.env", "Scan complete", false);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "PORT") != NULL);
    CHECK(text != NULL && strstr(text, "STATUS") != NULL);
    CHECK(text != NULL && strstr(text, "CONFLICT") != NULL);
    CHECK(text != NULL && strstr(text, "PROCESS") != NULL);
    CHECK(text != NULL && strstr(text, "DIRECTORY") != NULL);
    CHECK(text != NULL && strstr(text, "api-server") != NULL);
    CHECK(text != NULL && strstr(text, "/work/services/api") != NULL);
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
    CHECK(text != NULL && strstr(text, "free") != NULL);
    CHECK(text != NULL && strstr(text, "Up/Down Scroll") != NULL);
    CHECK(text != NULL && strstr(text, "r Refresh") != NULL);
    CHECK(text != NULL && strstr(text, "Enter") == NULL);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 60U, 18U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, ".ports.env", "Scan complete", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Showing 1–1 of 3 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "q/Esc Back") != NULL);
    free(text);

    dashboard.scroll = 2U;
    od_render_dashboard(&canvas, &dashboard, ".ports.env", "Scan complete", false);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Showing 3–3 of 3 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "5000") != NULL);
    free(text);
    od_canvas_free(&canvas);
    dashboard.scroll = 0U;

    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_dashboard(&canvas, &dashboard, ".ports.env", "Scan complete", true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "| PORT") != NULL);
    CHECK(text != NULL && line_has_at_least(text, '+', 6U));
    free(text);
    od_canvas_free(&canvas);
}

static void test_conflicts_have_one_whole_plan_action(void) {
    OdResolutionItem item = {
        .variable = "API_PORT",
        .old_port = 3000U,
        .new_port = 3001U
    };
    OdResolution resolution = {.items = &item, .count = 1U};
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    od_render_conflicts(&canvas, &resolution, 0U, true, true,
                        "Ready to apply", false);
    char *text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "OLD PORT") != NULL);
    CHECK(text != NULL && strstr(text, "NEW PORT") != NULL);
    CHECK(text != NULL && strstr(text, "3000") != NULL);
    CHECK(text != NULL && strstr(text, "3001") != NULL);
    CHECK(text != NULL && strstr(text, "┬") != NULL);
    CHECK(text != NULL && strstr(text, "├") != NULL);
    CHECK(text != NULL && strstr(text, "┼") != NULL);
    CHECK(text != NULL && strstr(text, "┤") != NULL);
    CHECK(text != NULL && strstr(text, "┴") != NULL);
    CHECK(text != NULL && strstr(text, "Enter Apply all") != NULL);
    CHECK(text != NULL && strstr(text, "q/Esc Cancel") != NULL);
    CHECK(text != NULL && strstr(text, "┌") != NULL);
    CHECK(text != NULL && strstr(text, "Edit") == NULL);
    CHECK(text != NULL && strstr(text, "Override") == NULL);
    free(text);

    od_render_conflicts(&canvas, &resolution, 0U, false, true,
                        "Updated 1 port(s)", true);
    text = rendered_text(&canvas);
    CHECK(text != NULL && strstr(text, "Updated 1 port(s)") != NULL);
    CHECK(text != NULL && strstr(text, "Enter Apply all") == NULL);
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
    test_update_is_a_cli_only_action();
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
