#include "opendoor/screens.h"
#include "opendoor/display.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const banner[] = {
    "   ▄▄▄▄    ▄▄▄▄▄▄    ▄▄▄▄▄▄▄▄  ▄▄▄   ▄▄  ▄▄▄▄▄       ▄▄▄▄      ▄▄▄▄    ▄▄▄▄▄▄",
    "  ██▀▀██   ██▀▀▀▀█▄  ██▀▀▀▀▀▀  ███   ██  ██▀▀▀██    ██▀▀██    ██▀▀██   ██▀▀▀▀██",
    " ██    ██  ██    ██  ██        ██▀█  ██  ██    ██  ██    ██  ██    ██  ██    ██",
    " ██    ██  ██████▀   ███████   ██ ██ ██  ██    ██  ██    ██  ██    ██  ███████",
    " ██    ██  ██        ██        ██  █▄██  ██    ██  ██    ██  ██    ██  ██  ▀██▄",
    "  ██▄▄██   ██        ██▄▄▄▄▄▄  ██   ███  ██▄▄▄██    ██▄▄██    ██▄▄██   ██    ██",
    "   ▀▀▀▀    ▀▀        ▀▀▀▀▀▀▀▀  ▀▀   ▀▀▀  ▀▀▀▀▀       ▀▀▀▀      ▀▀▀▀    ▀▀    ▀▀▀"
};

static const char *const menu_labels[OD_MENU_COUNT] = {
    "Dashboard",
    "Resolve conflicts",
    "History",
    "Quit"
};

static const char *const menu_descriptions[OD_MENU_COUNT] = {
    "Ports in this project",
    "Find and fix port clashes",
    "View and revert past changes",
    NULL
};

typedef struct {
    const char *key;
    const char *label;
} OdGuideItem;

typedef struct {
    int panel_x;
    int panel_y;
    int panel_width;
    int panel_height;
    int first_item_y;
    int hints_y;
    int reminder_y;
    int subtitle_y;
    int top;
    size_t logo_height;
    bool show_descriptions;
    bool show_reminder;
} OdMenuLayout;

typedef enum {
    OD_GRID_TOP,
    OD_GRID_MIDDLE,
    OD_GRID_BOTTOM
} OdGridRule;

static const OdGuideItem dashboard_guide[] = {
    {"Up/Down", "Scroll"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem menu_unicode_guide[] = {
    {"↑↓", "Navigate"}, {"Enter", "Select"}, {"q", "Quit"}
};

static const OdGuideItem menu_ascii_guide[] = {
    {"Up/Down", "Navigate"}, {"Enter", "Select"}, {"q", "Quit"}
};

static const OdGuideItem conflicts_apply_guide[] = {
    {"Up/Down", "Select"}, {"Enter", "Apply"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem conflicts_back_guide[] = {
    {"Up/Down", "Select"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem history_ready_guide[] = {
    {"Up/Down", "Select"}, {"Enter", "Revert"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem history_back_guide[] = {
    {"Up/Down", "Select"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem history_confirm_guide[] = {
    {"y", "Confirm revert"}, {"n/q/Esc", "Cancel"}
};

OdMenuItem od_menu_dispatch(size_t selected) {
    if (selected >= (size_t)OD_MENU_COUNT) return OD_MENU_QUIT;
    return (OdMenuItem)selected;
}

static size_t available_width(const OdCanvas *canvas, int x) {
    return x >= 0 && (size_t)x < canvas->width ? canvas->width - (size_t)x : 0U;
}

static size_t dashboard_table_capacity(size_t height) {
    size_t capacity = height > 16U ? (height - 16U) / 2U : 0U;
    return capacity == 0U ? 1U : capacity;
}

static void draw_grid_rule(OdCanvas *canvas,
                           int x,
                           int y,
                           const int *widths,
                           size_t count,
                           bool ascii,
                           OdGridRule rule) {
    const char *horizontal = ascii ? "-" : "─";
    const char *left = "+";
    const char *junction = "+";
    const char *right = "+";
    if (!ascii) {
        if (rule == OD_GRID_TOP) {
            left = "┌";
            junction = "┬";
            right = "┐";
        } else if (rule == OD_GRID_MIDDLE) {
            left = "├";
            junction = "┼";
            right = "┤";
        } else {
            left = "└";
            junction = "┴";
            right = "┘";
        }
    }

    int cursor = x;
    od_canvas_put(canvas, cursor, y, left, OD_ROLE_FOCUSED_BORDER, 0U);
    for (size_t index = 0U; index < count; ++index) {
        for (int column = 0; column < widths[index]; ++column) {
            od_canvas_put(canvas, cursor + 1 + column, y, horizontal,
                          OD_ROLE_FOCUSED_BORDER, 0U);
        }
        cursor += widths[index] + 1;
        od_canvas_put(canvas, cursor, y,
                      index + 1U == count ? right : junction,
                      OD_ROLE_FOCUSED_BORDER, 0U);
    }
}

static void draw_grid_row(OdCanvas *canvas,
                          int x,
                          int y,
                          const int *widths,
                          const char *const *cells,
                          size_t count,
                          bool ascii,
                          OdStyleRole role,
                          unsigned attributes) {
    const char *vertical = ascii ? "|" : "│";
    int cursor = x;
    od_canvas_put(canvas, cursor, y, vertical, OD_ROLE_FOCUSED_BORDER, 0U);
    for (size_t index = 0U; index < count; ++index) {
        if (widths[index] > 2) {
            od_canvas_write(canvas, cursor + 2, y, cells[index],
                            (size_t)(widths[index] - 2), role, attributes);
        }
        cursor += widths[index] + 1;
        od_canvas_put(canvas, cursor, y, vertical,
                      OD_ROLE_FOCUSED_BORDER, 0U);
    }
}

static void draw_guide(OdCanvas *canvas,
                       int y,
                       const OdGuideItem *items,
                       size_t count) {
    int x = 1;
    for (size_t index = 0U; index < count; ++index) {
        size_t key_width = strlen(items[index].key);
        size_t label_width = strlen(items[index].label);
        if (x >= (int)canvas->width - 1) break;
        od_canvas_write(canvas, x, y, items[index].key, key_width,
                        OD_ROLE_PRIMARY, 1U);
        x += (int)key_width;
        if (x < (int)canvas->width - 1) {
            od_canvas_write(canvas, x, y, " ", 1U, OD_ROLE_MUTED, 0U);
            ++x;
        }
        od_canvas_write(canvas, x, y, items[index].label, label_width,
                        OD_ROLE_MUTED, 0U);
        x += (int)label_width + 3;
    }
}

static void draw_centered_guide(OdCanvas *canvas,
                                int y,
                                const OdGuideItem *items,
                                size_t count) {
    size_t total_width = 0U;
    for (size_t index = 0U; index < count; ++index) {
        total_width += od_text_columns(items[index].key) + 1U +
            od_text_columns(items[index].label);
        if (index + 1U < count) total_width += 3U;
    }
    int x = total_width >= canvas->width ? 0 :
        (int)((canvas->width - total_width) / 2U);
    for (size_t index = 0U; index < count; ++index) {
        size_t key_width = od_text_columns(items[index].key);
        size_t label_width = od_text_columns(items[index].label);
        od_canvas_write(canvas, x, y, items[index].key, key_width,
                        OD_ROLE_PRIMARY, 1U);
        x += (int)key_width;
        od_canvas_write(canvas, x, y, " ", 1U, OD_ROLE_MUTED, 0U);
        ++x;
        od_canvas_write(canvas, x, y, items[index].label, label_width,
                        OD_ROLE_MUTED, 0U);
        x += (int)label_width;
        if (index + 1U < count) x += 3;
    }
}

static void status_line(OdCanvas *canvas, const char *status) {
    if (status == NULL || canvas->height < 2U) return;
    od_canvas_write(canvas, 1, (int)canvas->height - 2, status,
                    canvas->width > 2U ? canvas->width - 2U : 0U,
                    OD_ROLE_MUTED, 0U);
}

static void write_centered_in(OdCanvas *canvas,
                              int left,
                              int width,
                              int y,
                              const char *text,
                              OdStyleRole role,
                              unsigned attributes) {
    size_t columns = od_text_columns(text);
    int x = left;
    if (columns < (size_t)width) x += (width - (int)columns) / 2;
    od_canvas_write(canvas, x, y, text, width > 0 ? (size_t)width : 0U,
                    role, attributes);
}

void od_render_resize_required(OdCanvas *canvas) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    int middle = (int)(canvas->height / 2U);
    od_canvas_write_centered(canvas, middle - 1, "Terminal too small",
                             OD_ROLE_WARNING, 1U);
    od_canvas_write_centered(canvas, middle + 1, "Resize to at least 60 x 18",
                             OD_ROLE_MUTED, 0U);
}

static OdMenuLayout menu_layout(size_t width, size_t height, bool ascii) {
    size_t logo_height = !ascii && width >= 84U && height >= 26U ?
        sizeof(banner) / sizeof(banner[0]) : 1U;
    bool show_descriptions = height >= 22U;
    bool show_reminder = height >= 20U;
    int panel_height = (int)OD_MENU_COUNT + 4;
    int composition_height = (int)logo_height + panel_height + 5 +
        (show_reminder ? 2 : 0);
    int top = (int)height > composition_height ?
        ((int)height - composition_height) / 2 : 0;
    int panel_width = 58;
    if ((size_t)panel_width > width - 4U) panel_width = (int)width - 4;
    int panel_x = ((int)width - panel_width) / 2;
    int subtitle_y = top + (int)logo_height + 1;
    int panel_y = subtitle_y + 2;
    int hints_y = panel_y + panel_height + 1;
    return (OdMenuLayout){
        .panel_x = panel_x,
        .panel_y = panel_y,
        .panel_width = panel_width,
        .panel_height = panel_height,
        .first_item_y = panel_y + 2,
        .hints_y = hints_y,
        .reminder_y = hints_y + 2,
        .subtitle_y = subtitle_y,
        .top = top,
        .logo_height = logo_height,
        .show_descriptions = show_descriptions,
        .show_reminder = show_reminder
    };
}

static int menu_item_y(const OdMenuLayout *layout, size_t item) {
    return layout->first_item_y + (int)item;
}

static void draw_menu_frame(OdCanvas *canvas,
                            const OdMenuLayout *layout,
                            bool ascii) {
    const char *top_left = ascii ? "+" : "╭";
    const char *top_right = ascii ? "+" : "╮";
    const char *bottom_left = ascii ? "+" : "╰";
    const char *bottom_right = ascii ? "+" : "╯";
    const char *horizontal = ascii ? "-" : "─";
    const char *vertical = ascii ? "|" : "│";
    const char *title = "|OpenDoor|";
    int right = layout->panel_x + layout->panel_width - 1;
    int bottom = layout->panel_y + layout->panel_height - 1;

    od_canvas_put(canvas, layout->panel_x, layout->panel_y,
                  top_left, OD_ROLE_FOCUSED_BORDER, 0U);
    od_canvas_put(canvas, right, layout->panel_y,
                  top_right, OD_ROLE_FOCUSED_BORDER, 0U);
    od_canvas_put(canvas, layout->panel_x, bottom,
                  bottom_left, OD_ROLE_FOCUSED_BORDER, 0U);
    od_canvas_put(canvas, right, bottom,
                  bottom_right, OD_ROLE_FOCUSED_BORDER, 0U);
    for (int x = layout->panel_x + 1; x < right; ++x) {
        od_canvas_put(canvas, x, layout->panel_y,
                      horizontal, OD_ROLE_FOCUSED_BORDER, 0U);
        od_canvas_put(canvas, x, bottom,
                      horizontal, OD_ROLE_FOCUSED_BORDER, 0U);
    }
    for (int y = layout->panel_y + 1; y < bottom; ++y) {
        od_canvas_put(canvas, layout->panel_x, y,
                      vertical, OD_ROLE_FOCUSED_BORDER, 0U);
        od_canvas_put(canvas, right, y,
                      vertical, OD_ROLE_FOCUSED_BORDER, 0U);
    }
    int title_x = layout->panel_x +
        (layout->panel_width - (int)strlen(title)) / 2;
    od_canvas_write(canvas, title_x, layout->panel_y, title, strlen(title),
                    OD_ROLE_DEFAULT, 1U);
}

static void compact_project_path(const char *project,
                                 char *output,
                                 size_t output_size) {
    const char *input = project == NULL || project[0] == '\0' ? "." : project;
    char resolved[OD_PATH_CAP];
    const char *source = realpath(input, resolved) == NULL ? input : resolved;
    char normalized[OD_PATH_CAP];
    (void)snprintf(normalized, sizeof(normalized), "%s", source);
    size_t length = strlen(normalized);
    while (length > 1U && normalized[length - 1U] == '/') {
        normalized[--length] = '\0';
    }

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        char resolved_home[OD_PATH_CAP];
        const char *home_source = realpath(home, resolved_home) == NULL ?
            home : resolved_home;
        char normalized_home[OD_PATH_CAP];
        (void)snprintf(normalized_home, sizeof(normalized_home), "%s", home_source);
        size_t home_length = strlen(normalized_home);
        while (home_length > 1U && normalized_home[home_length - 1U] == '/') {
            normalized_home[--home_length] = '\0';
        }
        if (strcmp(normalized, normalized_home) == 0) {
            (void)snprintf(output, output_size, "~");
            return;
        }
        if (strncmp(normalized, normalized_home, home_length) == 0 &&
            normalized[home_length] == '/') {
            if (output_size == 0U) return;
            if (output_size == 1U) {
                output[0] = '\0';
                return;
            }
            output[0] = '~';
            size_t suffix_length = strlen(normalized + home_length);
            size_t available = output_size - 2U;
            size_t copied = suffix_length < available ?
                suffix_length : available;
            memcpy(output + 1, normalized + home_length, copied);
            output[copied + 1U] = '\0';
            return;
        }
    }

    char components[OD_PATH_CAP];
    (void)snprintf(components, sizeof(components), "%s", normalized);
    char *last_separator = strrchr(components, '/');
    if (last_separator == NULL) {
        (void)snprintf(output, output_size, "%s",
                       components[0] == '\0' ? "project" : components);
        return;
    }
    char *last = last_separator + 1;
    *last_separator = '\0';
    char *previous_separator = strrchr(components, '/');
    const char *previous = previous_separator == NULL ? components : previous_separator + 1;
    if (previous[0] == '\0') {
        (void)snprintf(output, output_size, "%s",
                       last[0] == '\0' ? "project" : last);
    } else {
        if (output_size == 0U) return;
        size_t previous_length = strlen(previous);
        size_t last_length = strlen(last);
        size_t used = previous_length < output_size - 1U ?
            previous_length : output_size - 1U;
        memcpy(output, previous, used);
        if (used < output_size - 1U) output[used++] = '/';
        size_t remaining = output_size - 1U - used;
        size_t copied = last_length < remaining ? last_length : remaining;
        memcpy(output + used, last, copied);
        output[used + copied] = '\0';
    }
}

static void write_centered_truncated(OdCanvas *canvas,
                                     int y,
                                     const char *text,
                                     size_t maximum_columns,
                                     bool ascii,
                                     OdStyleRole role,
                                     unsigned attributes) {
    size_t width = od_text_columns(text);
    if (width <= maximum_columns) {
        od_canvas_write_centered(canvas, y, text, role, attributes);
        return;
    }
    const char *ellipsis = ascii ? "..." : "…";
    size_t ellipsis_width = od_text_columns(ellipsis);
    if (maximum_columns <= ellipsis_width) return;
    size_t content_width = maximum_columns - ellipsis_width;
    int x = ((int)canvas->width - (int)maximum_columns) / 2;
    od_canvas_write(canvas, x, y, text, content_width, role, attributes);
    od_canvas_write(canvas, x + (int)content_width, y, ellipsis,
                    ellipsis_width, role, attributes);
}

static OdMouseAction guide_action_at(const OdGuideItem *items,
                                     const OdMouseAction *actions,
                                     size_t count,
                                     int x) {
    int left = 1;
    for (size_t index = 0U; index < count; ++index) {
        int width = (int)(strlen(items[index].key) + 1U +
                          strlen(items[index].label));
        if (x >= left && x < left + width) return actions[index];
        left += width + 3;
    }
    return OD_MOUSE_NONE;
}

OdMouseTarget od_menu_mouse_target(size_t width,
                                   size_t height,
                                   bool ascii,
                                   int x,
                                   int y) {
    if (width < 60U || height < 18U) return (OdMouseTarget){0};
    OdMenuLayout layout = menu_layout(width, height, ascii);
    if (x <= layout.panel_x || x >= layout.panel_x + layout.panel_width - 1) {
        return (OdMouseTarget){0};
    }
    for (size_t item = 0U; item < (size_t)OD_MENU_COUNT; ++item) {
        if (y == menu_item_y(&layout, item)) {
            return (OdMouseTarget){OD_MOUSE_MENU_ITEM, item};
        }
    }
    return (OdMouseTarget){0};
}

OdMouseTarget od_dashboard_mouse_target(size_t width,
                                        size_t height,
                                        int x,
                                        int y) {
    static const OdMouseAction actions[] = {
        OD_MOUSE_NONE, OD_MOUSE_REFRESH, OD_MOUSE_BACK
    };
    if (width < 60U || height < 18U || y != (int)height - 1 ||
        x < 0 || x >= (int)width) {
        return (OdMouseTarget){0};
    }
    return (OdMouseTarget){
        guide_action_at(dashboard_guide, actions,
                        sizeof(actions) / sizeof(actions[0]), x),
        0U
    };
}


void od_render_main_menu(OdCanvas *canvas,
                         size_t selected,
                         const char *project,
                         const char *status,
                         bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    if (selected >= (size_t)OD_MENU_COUNT) selected = (size_t)OD_MENU_QUIT;

    OdMenuLayout layout = menu_layout(canvas->width, canvas->height, ascii);
    size_t logo_height = layout.logo_height;
    int top = layout.top;

    if (logo_height == 1U) {
        od_canvas_write_centered(canvas, top, "OPENDOOR", OD_ROLE_PRIMARY, 1U);
    } else {
        const size_t banner_width = 80U;
        int logo_x = banner_width >= canvas->width ? 0 :
            (int)((canvas->width - banner_width) / 2U);
        for (size_t index = 0U; index < logo_height; ++index) {
            od_canvas_write(canvas, logo_x, top + (int)index, banner[index],
                            available_width(canvas, logo_x), OD_ROLE_PRIMARY, 1U);
        }
    }

    char compact_project[OD_PATH_CAP];
    compact_project_path(project, compact_project, sizeof(compact_project));
    char subtitle[OD_PATH_CAP + 64U];
    (void)snprintf(subtitle, sizeof(subtitle), "%s%s",
                   ascii ? "Pre-start port check - " : "Pre-start port check · ",
                   compact_project);
    write_centered_truncated(canvas, layout.subtitle_y, subtitle,
                             canvas->width - 4U, ascii,
                             OD_ROLE_MUTED, 0U);

    draw_menu_frame(canvas, &layout, ascii);
    for (size_t index = 0U; index < (size_t)OD_MENU_COUNT; ++index) {
        bool is_selected = index == selected;
        int row_y = menu_item_y(&layout, index);
        bool has_description = layout.show_descriptions &&
            menu_descriptions[index] != NULL;
        if (is_selected) {
            for (int x = layout.panel_x + 1;
                 x < layout.panel_x + layout.panel_width - 1; ++x) {
                od_canvas_put(canvas, x, row_y,
                              " ", OD_ROLE_SELECTED, 0U);
            }
            od_canvas_put(canvas, layout.panel_x + 2, row_y,
                          ascii ? ">" : "▶", OD_ROLE_SELECTED, 1U);
        }
        od_canvas_write(canvas, layout.panel_x + 4, row_y,
                        menu_labels[index], 17U,
                        is_selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        1U);
        if (has_description) {
            int description_x = layout.panel_x + 25;
            int description_width = layout.panel_x + layout.panel_width - 1 -
                description_x;
            od_canvas_write(canvas, description_x, row_y,
                            menu_descriptions[index],
                            description_width > 0 ?
                                (size_t)description_width : 0U,
                            is_selected ? OD_ROLE_SELECTED : OD_ROLE_MUTED,
                            0U);
        }
    }
    const OdGuideItem *guide = ascii ? menu_ascii_guide : menu_unicode_guide;
    draw_centered_guide(canvas, layout.hints_y, guide,
                        sizeof(menu_ascii_guide) / sizeof(menu_ascii_guide[0]));
    if (layout.show_reminder && status != NULL) {
        write_centered_truncated(canvas, layout.reminder_y, status,
                                 canvas->width - 4U, ascii,
                                 OD_ROLE_MUTED, 0U);
    }
}

void od_render_dashboard(OdCanvas *canvas,
                         const OdDashboard *dashboard,
                         const char *status,
                         bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    od_canvas_write(canvas, 2, 1, "OPENDOOR / Dashboard",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Project declarations and live endpoints",
                    available_width(canvas, 2),
                    OD_ROLE_MUTED, 0U);
    od_canvas_write(canvas, 2, 3,
                    "Pre-start check — stop project services before interpreting conflicts",
                    available_width(canvas, 2), OD_ROLE_WARNING, 0U);

    int box_x = 1;
    int box_y = 5;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    int inner_width = box_width - 4;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height,
                  ascii, OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y, " Ports in use ",
                    (size_t)inner_width, OD_ROLE_PRIMARY, 1U);

    const size_t column_count = 5U;
    int grid_x = box_x + 2;
    int grid_y = box_y + 2;
    int grid_width = box_width - 4;
    int content_width = grid_width - (int)column_count - 1;
    int widths[5] = {7, 0, 16, 0, 0};
    if (content_width >= 80) {
        widths[3] = 24;
    } else if (content_width >= 60) {
        widths[3] = 18;
    } else {
        widths[3] = 9;
    }
    int path_width = content_width - widths[0] - widths[2] - widths[3];
    widths[4] = path_width * 2 / 5;
    if (widths[4] < 8) widths[4] = 8;
    if (path_width - widths[4] < 8) widths[4] = path_width - 8;
    widths[1] = path_width - widths[4];
    const char *headers[] = {
        "PORT", "RELATIVE FOLDER", "STATUS", "PROCESS", "SOURCE"
    };
    draw_grid_rule(canvas, grid_x, grid_y, widths, column_count,
                   ascii, OD_GRID_TOP);
    draw_grid_row(canvas, grid_x, grid_y + 1, widths, headers, column_count,
                  ascii, OD_ROLE_MUTED, 1U);
    draw_grid_rule(canvas, grid_x, grid_y + 2, widths, column_count,
                   ascii, OD_GRID_MIDDLE);

    size_t capacity = dashboard_table_capacity(canvas->height);
    size_t start = dashboard == NULL ? 0U : dashboard->scroll;
    size_t end = dashboard == NULL ? 0U : start + capacity;
    if (dashboard != NULL && end > dashboard->count) end = dashboard->count;
    size_t visible = 0U;
    for (size_t index = start; index < end; ++index, ++visible) {
        const OdPortRow *row = &dashboard->rows[index];
        char port[16];
        (void)snprintf(port, sizeof(port), "%u", (unsigned)row->port);
        const char *row_status = "not running";
        if (row->status == OD_PORT_RUNNING) row_status = "running";
        if (row->status == OD_PORT_IN_USE_OTHER) row_status = "in use (other)";
        const char *cells[] = {
            port,
            row->relative_folder,
            row_status,
            row->process,
            row->source
        };
        OdStyleRole role = row->status == OD_PORT_RUNNING ?
            OD_ROLE_SUCCESS : OD_ROLE_DEFAULT;
        int row_y = grid_y + 3 + (int)visible * 2;
        draw_grid_row(canvas, grid_x, row_y, widths, cells, column_count,
                      ascii, role, 0U);
        if (row->status == OD_PORT_IN_USE_OTHER) {
            int status_x = grid_x + widths[0] + widths[1] + 4;
            od_canvas_write(canvas, status_x, row_y, row_status,
                            (size_t)(widths[2] - 2), OD_ROLE_DANGER, 1U);
        }
        draw_grid_rule(canvas, grid_x, row_y + 1, widths, column_count,
                       ascii, index + 1U == end ? OD_GRID_BOTTOM : OD_GRID_MIDDLE);
    }
    if (dashboard == NULL || dashboard->count == 0U) {
        draw_grid_rule(canvas, grid_x, grid_y + 3, widths, column_count,
                       ascii, OD_GRID_BOTTOM);
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          grid_y + 4, "No ports found in this project",
                          OD_ROLE_MUTED, 0U);
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          grid_y + 5,
                          "Supported: Compose, .env, package.json, Makefile",
                          OD_ROLE_MUTED, 0U);
    }

    char page[96];
    size_t total = dashboard == NULL ? 0U : dashboard->count;
    size_t first = total == 0U ? 0U : start + 1U;
    size_t last = end;
    (void)snprintf(page, sizeof(page), "Showing %zu–%zu of %zu port(s)",
                   first, last, total);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)inner_width, OD_ROLE_MUTED, 0U);
    status_line(canvas, status);
    draw_guide(canvas, (int)canvas->height - 1, dashboard_guide,
               sizeof(dashboard_guide) / sizeof(dashboard_guide[0]));
}


size_t od_scan_warning_rows(const OdScanReport *report) {
    return report == NULL ? 0U : report->count + (report->total_count > report->count ? 1U : 0U);
}

static void clear_canvas_line(OdCanvas *canvas, int y) {
    for (size_t x = 0U; x < canvas->width; ++x) od_canvas_put(canvas, (int)x, y, " ", OD_ROLE_DEFAULT, 0U);
}

static void draw_scan_warning(OdCanvas *canvas, int y, const OdScanReport *report,
                               size_t index, bool selected) {
    if (index >= od_scan_warning_rows(report)) return;
    char omitted[64];
    const char *text;
    OdStyleRole role = OD_ROLE_WARNING;
    const char *marker = "";
    if (index >= report->count) {
        (void)snprintf(omitted, sizeof(omitted), "and %zu more", report->total_count - report->count);
        text = omitted;
    } else {
        bool blocks = od_scan_warning_severity(report->items[index].kind) == OD_WARNING_BLOCKING;
        marker = blocks ? "[blocks apply] " : "[info] ";
        role = blocks ? OD_ROLE_DANGER : OD_ROLE_WARNING;
        text = report->items[index].text;
    }
    if (selected) {
        role = OD_ROLE_SELECTED;
        for (size_t x = 1U; x + 1U < canvas->width; ++x)
            od_canvas_put(canvas, (int)x, y, " ", role, 0U);
    }
    od_canvas_write(canvas, 2, y, marker, canvas->width - 4U, role, 1U);
    int x = 2 + (int)strlen(marker);
    od_canvas_write(canvas, x, y, text, canvas->width - (size_t)x - 2U, role, 0U);
}

static const OdGuideItem dashboard_report_guide[] = {
    {"Up/Down", "Scroll"}, {"r", "Refresh"}, {"q/Esc", "Back"}, {"w", "Warnings"}
};
static const OdGuideItem dashboard_warnings_guide[] = {
    {"Up/Down", "Scroll"}, {"w", "Ports"}, {"q/Esc", "Ports"}, {"r", "Refresh"}
};

OdMouseTarget od_dashboard_report_mouse_target(size_t width, size_t height,
                                               const OdScanReport *report,
                                               bool warnings_view, int x, int y) {
    if (od_scan_warning_rows(report) == 0U) return od_dashboard_mouse_target(width, height, x, y);
    if (width < 60U || height < 18U || y != (int)height - 1 || x < 0 || (size_t)x >= width)
        return (OdMouseTarget){0};
    static const OdMouseAction ports_actions[] = {
        OD_MOUSE_NONE, OD_MOUSE_REFRESH, OD_MOUSE_BACK, OD_MOUSE_WARNINGS
    };
    static const OdMouseAction warnings_actions[] = {
        OD_MOUSE_NONE, OD_MOUSE_WARNINGS, OD_MOUSE_BACK, OD_MOUSE_REFRESH
    };
    return (OdMouseTarget){guide_action_at(
        warnings_view ? dashboard_warnings_guide : dashboard_report_guide,
        warnings_view ? warnings_actions : ports_actions, 4U, x), 0U};
}

void od_render_dashboard_report(OdCanvas *canvas, const OdDashboard *dashboard,
                                const char *status, bool ascii,
                                const OdScanReport *report, bool warnings_view,
                                size_t warning_scroll) {
    od_render_dashboard(canvas, dashboard, status, ascii);
    if (canvas->width < 60U || canvas->height < 18U || od_scan_warning_rows(report) == 0U) return;
    if (warnings_view) {
        for (int y = 4; y < (int)canvas->height - 2; ++y) clear_canvas_line(canvas, y);
        od_canvas_write(canvas, 2, 4, "Scan warnings", canvas->width - 4U, OD_ROLE_WARNING, 1U);
        size_t rows = od_scan_warning_rows(report);
        if (warning_scroll >= rows) warning_scroll = rows - 1U;
        for (size_t i = warning_scroll; i < rows && i - warning_scroll < canvas->height - 9U; ++i)
            draw_scan_warning(canvas, 6 + (int)(i - warning_scroll), report, i, false);
    }
    char summary[96];
    (void)snprintf(summary, sizeof(summary), "Scan incomplete (%zu warning%s)", report->total_count,
                   report->total_count == 1U ? "" : "s");
    /* The completeness summary must not conceal a fatal scan error. Normal
     * refreshes pass the same summary; failures keep their reason visible. */
    if (status != NULL && status[0] != '\0' && strcmp(status, summary) != 0) {
        clear_canvas_line(canvas, 2);
        od_canvas_write(canvas, 2, 2, status, canvas->width - 4U, OD_ROLE_DANGER, 1U);
    }
    clear_canvas_line(canvas, (int)canvas->height - 2);
    od_canvas_write(canvas, 1, (int)canvas->height - 2, summary, canvas->width - 2U, OD_ROLE_WARNING, 1U);
    clear_canvas_line(canvas, (int)canvas->height - 1);
    draw_guide(canvas, (int)canvas->height - 1,
               warnings_view ? dashboard_warnings_guide : dashboard_report_guide, 4U);
}

void od_render_dashboard_loading(OdCanvas *canvas, bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    od_canvas_write(canvas, 2, 1, "OPENDOOR / Dashboard",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Project declarations and live endpoints",
                    available_width(canvas, 2), OD_ROLE_MUTED, 0U);

    int box_x = 1;
    int box_y = 5;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    int middle = box_y + box_height / 2;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height,
                  ascii, OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y, " Scanning ",
                    (size_t)(box_width - 4), OD_ROLE_PRIMARY, 1U);
    write_centered_in(canvas, box_x + 1, box_width - 2, middle - 1,
                      ascii ? "[*] Scanning project ports..." :
                              "◌ Scanning project ports…",
                      OD_ROLE_PRIMARY, 1U);
    write_centered_in(canvas, box_x + 1, box_width - 2, middle + 1,
                      "Discovering declarations and checking live endpoints",
                      OD_ROLE_MUTED, 0U);
    status_line(canvas, "Please wait; no project files are being changed");
}

static void render_operation_loading(OdCanvas *canvas,
                                     const char *title,
                                     const char *subtitle,
                                     const char *panel_title,
                                     const char *headline,
                                     const char *detail,
                                     const char *status,
                                     bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    od_canvas_write(canvas, 2, 1, title,
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2, subtitle,
                    available_width(canvas, 2), OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 5;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    int middle = box_y + box_height / 2;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height,
                  ascii, OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y, panel_title,
                    (size_t)(box_width - 4), OD_ROLE_PRIMARY, 1U);
    write_centered_in(canvas, box_x + 1, box_width - 2, middle - 1,
                      headline, OD_ROLE_PRIMARY, 1U);
    write_centered_in(canvas, box_x + 1, box_width - 2, middle + 1,
                      detail, OD_ROLE_MUTED, 0U);
    status_line(canvas, status);
}

void od_render_conflicts_loading(OdCanvas *canvas, bool ascii) {
    render_operation_loading(
        canvas,
        "OPENDOOR / Resolve conflicts",
        "Pre-start check — external listeners on declared ports are conflicts",
        " Scanning ",
        ascii ? "[*] Scanning for port conflicts..." :
                "◌ Scanning for port conflicts…",
        "Discovering declarations and checking live endpoints",
        "Please wait; no project files are being changed",
        ascii);
}

void od_render_conflicts_apply_loading(OdCanvas *canvas, bool ascii) {
    render_operation_loading(
        canvas,
        "OPENDOOR / Resolve conflicts",
        "Pre-start check — external listeners on declared ports are conflicts",
        " Applying ",
        ascii ? "[*] Revalidating before apply..." :
                "◌ Revalidating before apply…",
        "Rescanning listeners before writing project files",
        "Please wait; changes begin only after validation succeeds",
        ascii);
}

void od_render_history_loading(OdCanvas *canvas, bool ascii) {
    render_operation_loading(
        canvas,
        "OPENDOOR / History",
        "Recorded port changes, newest first",
        " Loading ",
        ascii ? "[*] Loading change history..." :
                "◌ Loading change history…",
        "Reading the log and checking recorded declarations",
        "Please wait; no project files are being changed",
        ascii);
}

size_t od_history_page_size(size_t height) {
    size_t capacity = height > 12U ? (height - 12U) / 2U : 0U;
    return capacity == 0U ? 1U : capacity;
}

size_t od_history_visible_scroll(size_t count, size_t selected,
                                 size_t scroll, size_t height) {
    if (count == 0U) return 0U;
    if (selected >= count) selected = count - 1U;
    size_t capacity = od_history_page_size(height);
    size_t maximum = count > capacity ? count - capacity : 0U;
    if (scroll > maximum) scroll = maximum;
    if (selected < scroll) scroll = selected;
    if (selected - scroll >= capacity) scroll = selected - capacity + 1U;
    return scroll;
}

static bool history_ready(const OdHistory *history, size_t selected) {
    return history != NULL && selected < history->count &&
        history->items[selected].availability == OD_HISTORY_READY;
}

typedef struct {
    int x;
    int widths[6];
    size_t count;
    size_t action_index;
    bool padded;
} OdTableLayout;

static OdTableLayout history_layout(size_t width) {
    OdTableLayout layout = {0};
    if (width >= 91U) {
        int flexible = (int)width - 62;
        int source = flexible * 2 / 5;
        if (source < 12) source = 12;
        layout = (OdTableLayout){
            .x = 1,
            .widths = {21, flexible - source, source, 16, 17},
            .count = 5U,
            .action_index = 4U,
            .padded = true
        };
    } else if (width >= 79U) {
        layout = (OdTableLayout){
            .x = ((int)width - 79) / 2,
            .widths = {19, 15, 10, 14, 15},
            .count = 5U,
            .action_index = 4U,
            .padded = false
        };
    } else {
        layout = (OdTableLayout){
            .x = ((int)width - 59) / 2,
            .widths = {19, 10, 14, 11, 0},
            .count = 4U,
            .action_index = 3U,
            .padded = false
        };
    }
    return layout;
}

static int table_layout_right(const OdTableLayout *layout) {
    int right = layout->x;
    for (size_t index = 0U; index < layout->count; ++index) {
        right += layout->widths[index] + 1;
    }
    return right;
}

static int table_cell_text_x(const OdTableLayout *layout,
                               size_t cell_index) {
    int x = layout->x;
    for (size_t index = 0U; index < cell_index; ++index) {
        x += layout->widths[index] + 1;
    }
    return x + (layout->padded ? 2 : 1);
}

static size_t table_cell_content_width(const OdTableLayout *layout,
                                         size_t cell_index) {
    int padding = layout->padded ? 2 : 0;
    int width = layout->widths[cell_index] - padding;
    return width > 0 ? (size_t)width : 0U;
}

static void draw_table_row(OdCanvas *canvas,
                             int y,
                             const OdTableLayout *layout,
                             const char *const *cells,
                             OdStyleRole role,
                             unsigned attributes,
                             bool selected,
                             bool ascii) {
    const char *vertical = ascii ? "|" : "│";
    int cursor = layout->x;
    if (selected) {
        for (int x = cursor + 1; x < table_layout_right(layout); ++x) {
            od_canvas_put(canvas, x, y, " ", OD_ROLE_SELECTED, 0U);
        }
    }
    od_canvas_put(canvas, cursor, y, vertical, OD_ROLE_FOCUSED_BORDER, 0U);
    for (size_t index = 0U; index < layout->count; ++index) {
        int padding = layout->padded ? 1 : 0;
        int content_width = layout->widths[index] - padding * 2;
        if (content_width > 0) {
            od_canvas_write(canvas, cursor + 1 + padding, y, cells[index],
                            (size_t)content_width, role, attributes);
        }
        cursor += layout->widths[index] + 1;
        bool last = index + 1U == layout->count;
        od_canvas_put(canvas, cursor, y, vertical,
                      selected && !last ? OD_ROLE_SELECTED : OD_ROLE_FOCUSED_BORDER,
                      0U);
    }
}


static OdTableLayout conflicts_layout(size_t width) {
    if (width >= 91U) {
        return (OdTableLayout){.x = 1, .widths = {7, 10, 17, 12, (int)width - 64, 9},
                               .count = 6U, .action_index = 5U, .padded = true};
    }
    if (width >= 79U) {
        return (OdTableLayout){.x = 1, .widths = {7, 10, 12, (int)width - 46, 9},
                               .count = 5U, .action_index = 4U, .padded = true};
    }
    return (OdTableLayout){.x = 1, .widths = {7, 10, (int)width - 33, 9},
                           .count = 4U, .action_index = 3U, .padded = true};
}

/* Keep all distinct contributing paths; no fixed-size buffer can silently
 * discard a linked file before the canvas clips it to the terminal width. */
static char *conflict_paths(const OdResolution *resolution,
                            const OdResolutionItem *representative) {
    size_t capacity = 1U;
    for (size_t i = 0U; i < resolution->count; ++i) {
        const OdResolutionItem *item = &resolution->items[i];
        if (item->allocation_index != representative->allocation_index) continue;
        size_t length = strlen(item->relative_path == NULL ? "./?" : item->relative_path);
        if (length > SIZE_MAX - capacity - 2U) return NULL;
        capacity += length + 2U;
    }
    char *paths = calloc(capacity, 1U);
    if (paths == NULL) return NULL;
    for (size_t i = 0U; i < resolution->count; ++i) {
        const OdResolutionItem *item = &resolution->items[i];
        if (item->allocation_index != representative->allocation_index) continue;
        const char *path = item->relative_path == NULL ? "./?" : item->relative_path;
        bool duplicate = false;
        for (size_t j = 0U; j < i; ++j) {
            const OdResolutionItem *previous = &resolution->items[j];
            if (previous->allocation_index == item->allocation_index &&
                strcmp(previous->relative_path == NULL ? "./?" : previous->relative_path,
                       path) == 0) duplicate = true;
        }
        if (duplicate) continue;
        if (paths[0] != '\0') strcat(paths, ", ");
        strcat(paths, path);
    }
    return paths;
}


size_t od_conflicts_report_scroll(const OdResolution *resolution,
                                  const OdScanReport *report, size_t selected,
                                  size_t scroll, size_t height) {
    size_t warnings = od_scan_warning_rows(report);
    size_t groups = od_resolution_group_count(resolution);
    if (warnings == 0U) return od_history_visible_scroll(groups, selected, scroll / 2U, height) * 2U;
    size_t capacity = height > 9U ? height - 9U : 1U;
    size_t prefix = warnings + 2U;
    size_t length = prefix + (groups == 0U ? 1U : 4U + groups * 2U);
    size_t maximum = length > capacity ? length - capacity : 0U;
    if (scroll > maximum) scroll = maximum;
    if (warnings + groups == 0U) return 0U;
    if (selected >= warnings + groups) selected = warnings + groups - 1U;
    /* The empty-result message follows the warnings but has no selectable
     * conflict row. Reveal that tail when reaching the final warning. */
    if (groups == 0U && selected + 1U == warnings) return maximum;
    size_t target = selected < warnings ? 1U + selected :
        prefix + 3U + 2U * (selected - warnings);
    if (target < scroll) scroll = target;
    if (target >= scroll + capacity) scroll = target - capacity + 1U;
    return scroll;
}

OdMouseTarget od_conflicts_report_mouse_target(size_t width, size_t height,
                                               const OdResolution *resolution,
                                               size_t selected, size_t scroll,
                                               const OdScanReport *report,
                                               int x, int y) {
    if (width < 60U || height < 18U || x < 0 || y < 0 ||
        (size_t)x >= width || (size_t)y >= height) return (OdMouseTarget){0};
    size_t warnings = od_scan_warning_rows(report);
    size_t count = warnings + od_resolution_group_count(resolution);
    if (count > 0U && selected >= count) selected = count - 1U;
    const OdResolutionItem *chosen = selected < warnings ? NULL :
        od_resolution_group_item(resolution, selected - warnings);
    bool blocked = od_scan_report_blocks_apply(report);
    if ((size_t)y == height - 1U) {
        static const OdMouseAction ready_actions[] = {
            OD_MOUSE_NONE, OD_MOUSE_CONFLICT_APPLY, OD_MOUSE_REFRESH, OD_MOUSE_BACK
        };
        static const OdMouseAction back_actions[] = {OD_MOUSE_NONE, OD_MOUSE_REFRESH, OD_MOUSE_BACK};
        bool ready = chosen != NULL && chosen->automatic && !blocked;
        return (OdMouseTarget){guide_action_at(
            ready ? conflicts_apply_guide : conflicts_back_guide,
            ready ? ready_actions : back_actions, ready ? 4U : 3U, x), selected};
    }
    if (y < 4 || (size_t)y >= height - 5U) return (OdMouseTarget){0};
    if (warnings == 0U && y < 7) return (OdMouseTarget){0};
    scroll = od_conflicts_report_scroll(resolution, report, selected, scroll, height);
    size_t line = (size_t)y - 4U + scroll;
    if (warnings > 0U && line >= 1U && line <= warnings)
        return (OdMouseTarget){OD_MOUSE_CONFLICT_ROW, line - 1U};
    size_t table = warnings == 0U ? 0U : warnings + 2U;
    OdTableLayout layout = conflicts_layout(width);
    if (x <= layout.x || x >= table_layout_right(&layout) ||
        line < table + 3U || (line - table - 3U) % 2U != 0U) return (OdMouseTarget){0};
    size_t group = (line - table - 3U) / 2U;
    const OdResolutionItem *item = od_resolution_group_item(resolution, group);
    if (item == NULL) return (OdMouseTarget){0};
    int action_x = table_cell_text_x(&layout, layout.action_index);
    return (OdMouseTarget){item->automatic && !blocked && x >= action_x && x < action_x + 7 ?
        OD_MOUSE_CONFLICT_APPLY : OD_MOUSE_CONFLICT_ROW, warnings + group};
}

OdMouseTarget od_conflicts_mouse_target(size_t width, size_t height,
                                        const OdResolution *resolution,
                                        size_t selected, size_t scroll, int x, int y) {
    return od_conflicts_report_mouse_target(width, height, resolution, selected,
                                             scroll * 2U, NULL, x, y);
}

void od_render_conflicts(OdCanvas *canvas, const OdResolution *resolution,
                         size_t selected, size_t scroll, bool prepared,
                         const char *status, bool ascii) {
    od_render_conflicts_report(canvas, resolution, selected, scroll * 2U,
                               prepared, status, ascii, NULL);
}

void od_render_conflicts_report(OdCanvas *canvas, const OdResolution *resolution,
                         size_t selected, size_t scroll, bool prepared,
                         const char *status, bool ascii, const OdScanReport *report) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "OPENDOOR / Resolve conflicts",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Pre-start check — stop project services before applying",
                    available_width(canvas, 2), OD_ROLE_WARNING, 0U);
    size_t warning_rows = od_scan_warning_rows(report);
    size_t count = od_resolution_group_count(resolution);
    size_t entries = warning_rows + count;
    if (entries > 0U && selected >= entries) selected = entries - 1U;
    bool blocked = od_scan_report_blocks_apply(report);
    bool redundant_status = false;
    if (warning_rows > 0U) {
        char summary[96];
        (void)snprintf(summary, sizeof(summary), "Scan incomplete (%zu warning%s)", report->total_count,
                   report->total_count == 1U ? "" : "s");
        clear_canvas_line(canvas, 2);
        od_canvas_write(canvas, 2, 2, summary, canvas->width - 4U, OD_ROLE_WARNING, 1U);
        redundant_status = status != NULL && strcmp(status, summary) == 0;
    }
    /* Keep operation outcomes/errors, but do not echo the summary beneath itself. */
    if (status != NULL && !redundant_status)
        od_canvas_write(canvas, 2, 3, status, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    scroll = od_conflicts_report_scroll(resolution, report, selected, scroll, canvas->height);
    OdCanvas body = {canvas->width, canvas->height - 9U, canvas->cells + 4U * canvas->width};
    int table_y = warning_rows == 0U ? 0 : (int)warning_rows + 2 - (int)scroll;
    if (warning_rows > 0U) {
        od_canvas_write(&body, 2, -(int)scroll, "Scan warnings", body.width - 4U, OD_ROLE_WARNING, 1U);
        for (size_t i = 0U; i < warning_rows; ++i)
            draw_scan_warning(&body, 1 + (int)i - (int)scroll, report, i, i == selected);
    }
    OdTableLayout layout = conflicts_layout(canvas->width);
    const char *wide[] = {"PORT", "PROPOSED", "RELATIVE FOLDER", "PROCESS", "SOURCE", "ACTION"};
    const char *medium[] = {"PORT", "PROPOSED", "PROCESS", "SOURCE", "ACTION"};
    const char *narrow[] = {"PORT", "PROPOSED", "SOURCE", "ACTION"};
    const char *const *headers = layout.count == 6U ? wide :
        layout.count == 5U ? medium : narrow;
    if (count > 0U) {
        draw_grid_rule(&body, layout.x, table_y, layout.widths, layout.count, ascii, OD_GRID_TOP);
        draw_table_row(&body, table_y + 1, &layout, headers, OD_ROLE_MUTED, 1U, false, ascii);
        draw_grid_rule(&body, layout.x, table_y + 2, layout.widths, layout.count, ascii, OD_GRID_MIDDLE);
    }
    size_t first = 0U, visible = 0U;
    for (size_t offset = 0U; offset < count; ++offset) {
        int y = table_y + 3 + (int)offset * 2 - (warning_rows == 0U ? (int)scroll : 0);
        if (warning_rows == 0U && y < 3) continue;
        if (y < 0 || y >= (int)body.height) continue;
        if (visible == 0U) first = offset;
        ++visible;
        const OdResolutionItem *item = od_resolution_group_item(resolution, offset);
        char port[6], proposed[6], folder[OD_PATH_CAP];
        (void)snprintf(port, sizeof(port), "%u", (unsigned)item->old_port);
        (void)snprintf(proposed, sizeof(proposed), "%u", (unsigned)item->new_port);
        (void)od_display_relative_folder(item->relative_path, folder, sizeof(folder));
        char *paths = conflict_paths(resolution, item);
        size_t source_capacity = (paths == NULL ? 0U : strlen(paths)) +
            (item->variable == NULL ? 0U : strlen(item->variable)) + 64U;
        char *source = malloc(source_capacity);
        if (source != NULL) (void)od_display_source(item->source_kind, item->variable,
                                                   paths, source, source_capacity);
        const char *process = item->process[0] == '\0' ? "-" : item->process;
        const char *source_text = source == NULL ? "Source unavailable" : source;
        const char *wide_cells[] = {port, proposed, folder, process, source_text, ""};
        const char *medium_cells[] = {port, proposed, process, source_text, ""};
        const char *narrow_cells[] = {port, proposed, source_text, ""};
        const char *const *cells = layout.count == 6U ? wide_cells :
            layout.count == 5U ? medium_cells : narrow_cells;
        bool is_selected = warning_rows + offset == selected;
        draw_table_row(&body, y, &layout, cells,
                       is_selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                       0U, is_selected, ascii);
        od_canvas_write(&body, table_cell_text_x(&layout, layout.action_index), y,
                        item->automatic ? (blocked ? "Blocked" : "[Apply]") : "Manual", 7U,
                        is_selected ? OD_ROLE_SELECTED :
                        item->automatic && !blocked ? OD_ROLE_PRIMARY : OD_ROLE_WARNING, 1U);
        draw_grid_rule(&body, layout.x, y + 1, layout.widths, layout.count, ascii,
                       offset + 1U == count ? OD_GRID_BOTTOM : OD_GRID_MIDDLE);
        if (is_selected) {
            status_line(canvas, paths == NULL ? "Unable to display source paths" : paths);
            const char *before = item->line_before == NULL ? "" : item->line_before;
            const char *after = item->line_after == NULL ? "" : item->line_after;
            size_t length = strlen(before) + strlen(after) + 5U;
            char *diff = malloc(length);
            if (diff != NULL) {
                (void)snprintf(diff, length, "%s | %s", before, after);
                od_canvas_write(canvas, 1, (int)canvas->height - 3,
                                item->automatic ? diff :
                                item->manual_reason == NULL ? "Manual edit required" : item->manual_reason,
                                canvas->width - 2U, OD_ROLE_MUTED, 0U);
                free(diff);
            }
        }
        free(source);
        free(paths);
    }
    if (count == 0U) {
        od_canvas_write(&body, 2, table_y, !prepared ? "Unable to prepare proposal" :
                        warning_rows > 0U ? "No conflicts detected in scanned data." : "No conflicts found",
                        canvas->width - 4U, !prepared ? OD_ROLE_DANGER : warning_rows > 0U ? OD_ROLE_WARNING : OD_ROLE_SUCCESS, 1U);
    }
    char page[96];
    (void)snprintf(page, sizeof(page), "Showing %zu-%zu of %zu conflict(s)",
                   visible == 0U ? 0U : first + 1U, first + visible, count);
    od_canvas_write(canvas, 1, (int)canvas->height - 4, page, canvas->width - 2U, OD_ROLE_MUTED, 0U);
    if (warning_rows > 0U) {
        char notice[160];
        (void)snprintf(notice, sizeof(notice),
            "Scan was incomplete (%zu warning%s); undiscovered declarations or unknown owners are possible.",
            report->total_count, report->total_count == 1U ? "" : "s");
        clear_canvas_line(canvas, (int)canvas->height - 3);
        od_canvas_write(canvas, 1, (int)canvas->height - 3, blocked ? OD_SCAN_APPLY_BLOCKED : notice,
                         canvas->width - 2U, OD_ROLE_WARNING, 1U);
    }
    const OdResolutionItem *item = selected < warning_rows ? NULL :
        od_resolution_group_item(resolution, selected - warning_rows);
    bool ready = item != NULL && item->automatic && !blocked;
    draw_guide(canvas, (int)canvas->height - 1,
               ready ? conflicts_apply_guide : conflicts_back_guide, ready ? 4U : 3U);
}

OdMouseTarget od_history_mouse_target(size_t width,
                                      size_t height,
                                      const OdHistory *history,
                                      size_t selected,
                                      size_t scroll,
                                      bool confirming,
                                      int x,
                                      int y) {
    if (width < 60U || height < 18U || x < 0 || y < 0 ||
        (size_t)x >= width || (size_t)y >= height) return (OdMouseTarget){0};
    if (confirming) {
        static const OdMouseAction actions[] = {OD_MOUSE_HISTORY_CONFIRM, OD_MOUSE_BACK};
        if ((size_t)y != height - 1U) return (OdMouseTarget){0};
        return (OdMouseTarget){guide_action_at(history_confirm_guide, actions, 2U, x), 0U};
    }
    size_t count = history == NULL ? 0U : history->count;
    if (count > 0U && selected >= count) selected = count - 1U;
    if ((size_t)y == height - 1U) {
        static const OdMouseAction ready_actions[] = {
            OD_MOUSE_NONE, OD_MOUSE_HISTORY_REVERT, OD_MOUSE_REFRESH, OD_MOUSE_BACK
        };
        static const OdMouseAction back_actions[] = {
            OD_MOUSE_NONE, OD_MOUSE_REFRESH, OD_MOUSE_BACK
        };
        bool ready = history_ready(history, selected);
        return (OdMouseTarget){
            guide_action_at(ready ? history_ready_guide : history_back_guide,
                            ready ? ready_actions : back_actions, ready ? 4U : 3U, x),
            selected
        };
    }
    OdTableLayout layout = history_layout(width);
    int right = table_layout_right(&layout);
    if (x <= layout.x || x >= right || y < 7 || (y - 7) % 2 != 0) {
        return (OdMouseTarget){0};
    }
    scroll = od_history_visible_scroll(count, selected, scroll, height);
    size_t visible = (size_t)(y - 7) / 2U;
    if (visible >= od_history_page_size(height) || visible >= count - scroll) {
        return (OdMouseTarget){0};
    }
    size_t item = scroll + visible;
    int action_x = table_cell_text_x(&layout, layout.action_index);
    OdMouseAction action = history_ready(history, item) &&
        x >= action_x && x < action_x + 8 ? OD_MOUSE_HISTORY_REVERT : OD_MOUSE_HISTORY_ROW;
    return (OdMouseTarget){action, item};
}

/* Write full detail components directly, clipping only at the canvas edge. */
static void history_detail_part(OdCanvas *canvas, int *x, const char *text) {
    if (text == NULL || *x >= (int)canvas->width - 1) return;
    size_t available = canvas->width - 1U - (size_t)*x;
    od_canvas_write(canvas, *x, (int)canvas->height - 2, text, available,
                    OD_ROLE_MUTED, 0U);
    size_t columns = od_text_columns(text);
    *x += (int)(columns > available ? available : columns);
}

static int history_detail(OdCanvas *canvas, const OdHistoryRecord *record) {
    char change[40];
    (void)snprintf(change, sizeof(change), " | %u -> %u",
                   (unsigned)record->old_port, (unsigned)record->new_port);
    int x = 1;
    history_detail_part(canvas, &x, record->relative_path);
    history_detail_part(canvas, &x, " | ");
    history_detail_part(canvas, &x, record->environment_key == NULL ||
                         record->environment_key[0] == '\0' ? "-" : record->environment_key);
    history_detail_part(canvas, &x, change);
    if (record->availability != OD_HISTORY_READY) {
        history_detail_part(canvas, &x, " | ");
        history_detail_part(canvas, &x, record->reason == NULL ?
                             "Availability has not been checked" : record->reason);
    }
    return x;
}

void od_render_history(OdCanvas *canvas,
                        const OdHistory *history,
                        size_t selected,
                        size_t scroll,
                        const char *status,
                        const char *error_detail,
                        bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    size_t count = history == NULL ? 0U : history->count;
    if (count > 0U && selected >= count) selected = count - 1U;
    od_canvas_write(canvas, 2, 1, "OPENDOOR / History",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2, status == NULL ?
                    "Recorded port changes, newest first" : status,
                    available_width(canvas, 2), OD_ROLE_MUTED, 0U);
    if (history != NULL && history->warning_count > 0U) {
        char warning[OD_ERROR_MESSAGE_CAP + 64U];
        (void)snprintf(warning, sizeof(warning), "%zu history warning(s): %s",
                       history->warning_count, history->warnings[0]);
        od_canvas_write(canvas, 2, 3, warning, available_width(canvas, 2),
                        OD_ROLE_WARNING, 0U);
    }

    OdTableLayout layout = history_layout(canvas->width);
    const char *wide_headers[] = {
        "WHEN", "RELATIVE FOLDER", "SOURCE", "CHANGE", "ACTION / STATUS"
    };
    const char *narrow_headers[] = {"WHEN", "SOURCE", "CHANGE", "STATUS"};
    const char *const *headers = layout.count == 5U ? wide_headers : narrow_headers;
    draw_grid_rule(canvas, layout.x, 4, layout.widths, layout.count,
                   ascii, OD_GRID_TOP);
    draw_table_row(canvas, 5, &layout, headers, OD_ROLE_MUTED, 1U, false,
                     ascii);
    draw_grid_rule(canvas, layout.x, 6, layout.widths, layout.count,
                   ascii, OD_GRID_MIDDLE);
    scroll = od_history_visible_scroll(count, selected, scroll, canvas->height);
    size_t visible = count - scroll;
    size_t capacity = od_history_page_size(canvas->height);
    if (visible > capacity) visible = capacity;
    for (size_t offset = 0U; offset < visible; ++offset) {
        size_t index = scroll + offset;
        const OdHistoryRecord *record = &history->items[index];
        int y = 7 + (int)offset * 2;
        OdStyleRole role = index == selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT;
        char when[20] = "";
        char folder[OD_PATH_CAP] = "";
        char source[OD_PROCESS_CAP + 32U] = "";
        (void)od_display_history_when(record->timestamp, when, sizeof(when));
        (void)od_display_relative_folder(record->relative_path,
                                         folder, sizeof(folder));
        (void)od_display_source(record->source_kind, record->environment_key,
                                NULL, source, sizeof(source));
        char change[32];
        (void)snprintf(change, sizeof(change), "%u -> %u",
                       (unsigned)record->old_port, (unsigned)record->new_port);
        const char *wide_cells[] = {when, folder, source, change, ""};
        const char *narrow_cells[] = {when, source, change, ""};
        const char *const *cells = layout.count == 5U ? wide_cells : narrow_cells;
        draw_table_row(canvas, y, &layout, cells, role, 0U,
                         index == selected, ascii);
        bool ready = record->availability == OD_HISTORY_READY;
        od_canvas_write(canvas,
                        table_cell_text_x(&layout, layout.action_index), y,
                        ready ? "[Revert]" : "Unavailable",
                        table_cell_content_width(&layout, layout.action_index),
                        index == selected ? OD_ROLE_SELECTED :
                        (ready ? OD_ROLE_PRIMARY : OD_ROLE_DANGER), 1U);
        draw_grid_rule(canvas, layout.x, y + 1, layout.widths, layout.count, ascii,
                       offset + 1U == visible ? OD_GRID_BOTTOM : OD_GRID_MIDDLE);
    }
    int detail_x = 1;
    if (count == 0U) {
        draw_grid_rule(canvas, layout.x, 7, layout.widths, layout.count,
                       ascii, OD_GRID_BOTTOM);
        od_canvas_write_centered(canvas, 9, "No history yet", OD_ROLE_MUTED, 0U);
    } else {
        detail_x = history_detail(canvas, &history->items[selected]);
    }
    if (error_detail != NULL && error_detail[0] != '\0') {
        if (count > 0U) history_detail_part(canvas, &detail_x, " | ");
        history_detail_part(canvas, &detail_x, error_detail);
    }
    char page[96];
    (void)snprintf(page, sizeof(page), "Showing %zu%s%zu of %zu change(s)",
                   count == 0U ? 0U : scroll + 1U, ascii ? "-" : "–",
                   scroll + visible, count);
    od_canvas_write(canvas, 2, (int)canvas->height - 4, page,
                    available_width(canvas, 2), OD_ROLE_MUTED, 0U);
    bool ready = history_ready(history, selected);
    draw_guide(canvas, (int)canvas->height - 1,
               ready ? history_ready_guide : history_back_guide, ready ? 4U : 3U);
}

void od_render_history_confirmation(OdCanvas *canvas,
                                     const OdHistoryRecord *record,
                                     bool ascii) {
    (void)ascii;
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "OPENDOOR / History",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    if (record == NULL) return;
    char heading[80];
    (void)snprintf(heading, sizeof(heading), "Confirm revert #%" PRIu64, record->id);
    od_canvas_write(canvas, 2, 4, heading, available_width(canvas, 2), OD_ROLE_WARNING, 1U);
    od_canvas_write(canvas, 2, 6, record->relative_path,
                    available_width(canvas, 2), OD_ROLE_DEFAULT, 0U);
    od_canvas_write(canvas, 2, 7, "Key: ", 5U, OD_ROLE_MUTED, 0U);
    od_canvas_write(canvas, 7, 7, record->environment_key == NULL ||
                    record->environment_key[0] == '\0' ? "-" : record->environment_key,
                    available_width(canvas, 7), OD_ROLE_DEFAULT, 0U);
    char change[40];
    (void)snprintf(change, sizeof(change), "Revert %u -> %u",
                   (unsigned)record->new_port, (unsigned)record->old_port);
    od_canvas_write(canvas, 2, 9, change, available_width(canvas, 2), OD_ROLE_WARNING, 1U);
    od_canvas_write(canvas, 2, 11, "Only y or Confirm revert writes. Enter does nothing.",
                    available_width(canvas, 2), OD_ROLE_MUTED, 0U);
    draw_guide(canvas, (int)canvas->height - 1, history_confirm_guide, 2U);
}
