#include "opendoor/theme.h"

#include <string.h>

enum {
    OD_COLOR_BLACK = 0,
    OD_COLOR_RED = 1,
    OD_COLOR_GREEN = 2,
    OD_COLOR_YELLOW = 3,
    OD_COLOR_BLUE = 4,
    OD_COLOR_MAGENTA = 5,
    OD_COLOR_CYAN = 6,
    OD_COLOR_WHITE = 7,
    OD_THEME_BOLD = 1,
    OD_THEME_REVERSE = 2
};

static const OdTheme themes[] = {
    {
        "midnight",
        {OD_COLOR_WHITE, OD_COLOR_CYAN, OD_COLOR_GREEN, OD_COLOR_YELLOW,
         OD_COLOR_RED, OD_COLOR_WHITE, OD_COLOR_BLACK, OD_COLOR_CYAN, OD_COLOR_WHITE},
        {-1, -1, -1, -1, -1, -1, OD_COLOR_CYAN, -1, -1},
        {0U, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD,
         0U, OD_THEME_REVERSE, OD_THEME_BOLD, 0U}
    },
    {
        "gruvbox",
        {OD_COLOR_WHITE, OD_COLOR_YELLOW, OD_COLOR_GREEN, OD_COLOR_YELLOW,
         OD_COLOR_RED, OD_COLOR_WHITE, OD_COLOR_BLACK, OD_COLOR_YELLOW, OD_COLOR_WHITE},
        {-1, -1, -1, -1, -1, -1, OD_COLOR_YELLOW, -1, -1},
        {0U, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD,
         0U, OD_THEME_REVERSE, OD_THEME_BOLD, 0U}
    },
    {
        "nord",
        {OD_COLOR_WHITE, OD_COLOR_CYAN, OD_COLOR_GREEN, OD_COLOR_YELLOW,
         OD_COLOR_RED, OD_COLOR_BLUE, OD_COLOR_BLACK, OD_COLOR_CYAN, OD_COLOR_BLUE},
        {-1, -1, -1, -1, -1, -1, OD_COLOR_WHITE, -1, -1},
        {0U, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD,
         0U, OD_THEME_REVERSE, OD_THEME_BOLD, 0U}
    },
    {
        "monochrome",
        {OD_COLOR_WHITE, OD_COLOR_WHITE, OD_COLOR_WHITE, OD_COLOR_WHITE,
         OD_COLOR_WHITE, OD_COLOR_WHITE, OD_COLOR_BLACK, OD_COLOR_WHITE, OD_COLOR_WHITE},
        {-1, -1, -1, -1, -1, -1, OD_COLOR_WHITE, -1, -1},
        {0U, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD, OD_THEME_BOLD,
         0U, OD_THEME_REVERSE, OD_THEME_BOLD, 0U}
    }
};

const OdTheme *od_theme_by_name(const char *name) {
    if (name == NULL) return NULL;
    for (size_t index = 0U; index < sizeof(themes) / sizeof(themes[0]); ++index) {
        if (strcmp(themes[index].name, name) == 0) return &themes[index];
    }
    return NULL;
}

const OdTheme *od_theme_at(size_t index) {
    return index < sizeof(themes) / sizeof(themes[0]) ? &themes[index] : NULL;
}

size_t od_theme_count(void) {
    return sizeof(themes) / sizeof(themes[0]);
}

