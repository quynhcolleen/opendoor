#include "opendoor/screens.h"
#include "opendoor/theme.h"
#include "opendoor/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static size_t newline_count(const char *text) {
    size_t count = 0U;
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '\n') ++count;
    }
    return count;
}

static void test_canvas_is_exactly_one_viewport(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 10U, 4U, &error) == OD_OK);
    od_canvas_clear(&canvas, OD_ROLE_DEFAULT);
    od_canvas_write(&canvas, 0, 0, "1234567890EXTRA", 99U, OD_ROLE_DEFAULT, 0U);
    od_canvas_write(&canvas, 0, 4, "must not render", 99U, OD_ROLE_DEFAULT, 0U);
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(newline_count(text) == 4U);
        CHECK(strncmp(text, "1234567890\n", 11U) == 0);
        CHECK(strstr(text, "EXTRA") == NULL);
        CHECK(strstr(text, "must not render") == NULL);
        free(text);
    }
    od_canvas_free(&canvas);
}

static void test_main_menu_snapshot(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 80U, 24U, &error) == OD_OK);
    OdMenuView view = {"demo", false, 0U, "Ready"};
    od_render_main_menu(&canvas, &view, true);
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(newline_count(text) == 24U);
        CHECK(strstr(text, "OPEN DOOR") != NULL);
        CHECK(strstr(text, "> Discover this project") != NULL);
        CHECK(strstr(text, "  Open listener explorer") != NULL);
        CHECK(strstr(text, "Ready") != NULL);
        CHECK(strstr(text, "Up/Down Navigate") != NULL);
        free(text);
    }
    od_canvas_free(&canvas);
}

static void test_loading_and_resize_snapshots(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 72U, 20U, &error) == OD_OK);
    od_render_loading(&canvas, OD_LOAD_PROCESS_OWNERS, 2U, 480U, true, true, 1U);
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(strstr(text, "[done] Project files") != NULL);
    CHECK(strstr(text, "[....] Process ownership") != NULL);
    CHECK(strstr(text, "Warnings: 1") != NULL);
    CHECK(strstr(text, "480 ms") != NULL);
    free(text);
    od_canvas_free(&canvas);

    CHECK(od_canvas_init(&canvas, 40U, 10U, &error) == OD_OK);
    od_render_resize_required(&canvas);
    text = od_canvas_to_text(&canvas, &error);
    CHECK(newline_count(text) == 10U);
    CHECK(strstr(text, "Terminal too small") != NULL);
    CHECK(strstr(text, "60 x 18") != NULL);
    free(text);
    od_canvas_free(&canvas);
}

static void test_unicode_and_ascii_boxes(void) {
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 8U, 4U, &error) == OD_OK);
    od_canvas_clear(&canvas, OD_ROLE_DEFAULT);
    od_canvas_box(&canvas, 0, 0, 8, 4, false, OD_ROLE_FOCUSED_BORDER);
    char *unicode = od_canvas_to_text(&canvas, &error);
    CHECK(strstr(unicode, "┌──────┐") != NULL);
    free(unicode);
    od_canvas_clear(&canvas, OD_ROLE_DEFAULT);
    od_canvas_box(&canvas, 0, 0, 8, 4, true, OD_ROLE_FOCUSED_BORDER);
    char *ascii = od_canvas_to_text(&canvas, &error);
    CHECK(strstr(ascii, "+------+") != NULL);
    free(ascii);
    od_canvas_free(&canvas);
}

static void test_midnight_theme(void) {
    const OdTheme *theme = od_theme_by_name("midnight");
    CHECK(theme != NULL);
    if (theme == NULL) return;
    CHECK(strcmp(theme->name, "midnight") == 0);
    CHECK(theme->foreground[OD_ROLE_PRIMARY] == 6);
    CHECK(theme->foreground[OD_ROLE_SUCCESS] == 2);
    CHECK(theme->foreground[OD_ROLE_WARNING] == 3);
    CHECK(theme->foreground[OD_ROLE_DANGER] == 1);
}

int main(void) {
    test_canvas_is_exactly_one_viewport();
    test_main_menu_snapshot();
    test_loading_and_resize_snapshots();
    test_unicode_and_ascii_boxes();
    test_midnight_theme();
    if (failures != 0) {
        fprintf(stderr, "%d UI checks failed\n", failures);
        return 1;
    }
    puts("UI checks passed");
    return 0;
}
