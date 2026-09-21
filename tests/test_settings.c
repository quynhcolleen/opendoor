#include "opendoor/config.h"
#include "opendoor/help.h"
#include "opendoor/model.h"
#include "opendoor/screens.h"
#include "opendoor/settings_store.h"
#include "opendoor/theme.h"
#include "opendoor/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void test_settings_round_trip_and_atomic_save(void) {
    OdSettings settings;
    od_settings_defaults(&settings);
    (void)strcpy(settings.theme, "nord");
    settings.unicode_mode = OD_UNICODE_NEVER;
    settings.reduced_motion = true;
    settings.mouse = false;
    settings.auto_refresh = true;
    settings.refresh_seconds = 12U;
    OdError error;
    char *rendered = NULL;
    size_t length = 0U;
    CHECK(od_settings_render(&settings, &rendered, &length, &error) == OD_OK);
    OdSettings parsed;
    CHECK(od_settings_parse(rendered, length, &parsed, &error) == OD_OK);
    CHECK(strcmp(parsed.theme, "nord") == 0);
    CHECK(parsed.unicode_mode == OD_UNICODE_NEVER);
    CHECK(parsed.reduced_motion && !parsed.mouse && parsed.auto_refresh);
    CHECK(parsed.refresh_seconds == 12U);
    free(rendered);

    char template[] = "/tmp/opendoor-settings-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/config/opendoor/settings.toml", root);
    CHECK(od_settings_save(path, &settings, &error) == OD_OK);
    OdSettings loaded;
    CHECK(od_settings_load(path, &loaded, &error) == OD_OK);
    CHECK(strcmp(loaded.theme, "nord") == 0 && loaded.refresh_seconds == 12U);
    CHECK(od_settings_save(path, &settings, &error) == OD_OK);
    char temporary[1100];
    (void)snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    (void)unlink(temporary);
    (void)unlink(path);
    (void)snprintf(temporary, sizeof(temporary), "%s/config/opendoor", root);
    (void)rmdir(temporary);
    (void)snprintf(temporary, sizeof(temporary), "%s/config", root);
    (void)rmdir(temporary);
    (void)rmdir(root);
}

static void test_help_search_and_pagination(void) {
    OdHelp help;
    OdError error;
    CHECK(od_help_init(&help, 3U, &error) == OD_OK);
    CHECK(help.visible_count > 8U);
    od_help_move(&help, 4);
    CHECK(help.page_start == 3U);
    od_help_move_page(&help, 1);
    CHECK(help.selected >= 6U);
    CHECK(od_help_search(&help, "mouse", &error) == OD_OK);
    CHECK(help.visible_count > 0U);
    CHECK(strstr(od_help_description(&help, 0U), "mouse") != NULL ||
          strstr(od_help_key(&help, 0U), "mouse") != NULL);
    od_help_end(&help);
    od_help_home(&help);
    CHECK(help.selected == 0U && help.page_start == 0U);
    od_help_free(&help);
}

static void test_settings_and_help_are_one_viewport(void) {
    OdSettings settings;
    od_settings_defaults(&settings);
    OdSettingsView view = {&settings, 0U, "/tmp/settings.toml", "Unsaved changes"};
    OdCanvas canvas;
    OdError error;
    CHECK(od_canvas_init(&canvas, 70U, 22U, &error) == OD_OK);
    od_render_settings(&canvas, &view, true);
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL && newline_count(text) == 22U);
    CHECK(text != NULL && strstr(text, "Settings and appearance") != NULL);
    CHECK(text != NULL && strstr(text, "Mouse input") != NULL);
    CHECK(text != NULL && strstr(text, "[on]") != NULL);
    free(text);
    od_canvas_free(&canvas);

    OdHelp help;
    CHECK(od_help_init(&help, 5U, &error) == OD_OK);
    CHECK(od_canvas_init(&canvas, 70U, 22U, &error) == OD_OK);
    od_render_help(&canvas, &help, true, "Help ready");
    text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL && newline_count(text) == 22U);
    CHECK(text != NULL && strstr(text, "Keyboard and workflow help") != NULL);
    CHECK(text != NULL && strstr(text, "PgUp/PgDn Page") != NULL);
    CHECK(text != NULL && strstr(text, "Help ready") != NULL);
    free(text);
    od_canvas_free(&canvas);
    od_help_free(&help);
}

int main(void) {
    test_settings_round_trip_and_atomic_save();
    test_help_search_and_pagination();
    test_settings_and_help_are_one_viewport();
    CHECK(od_theme_count() == 4U);
    if (failures != 0) {
        fprintf(stderr, "%d settings checks failed\n", failures);
        return 1;
    }
    puts("settings checks passed");
    return 0;
}
