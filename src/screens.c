#include "opendoor/screens.h"

#include <stdio.h>
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
    "Quit"
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
    int row_stride;
    int top;
    size_t logo_height;
} OdMenuLayout;

typedef enum {
    OD_GRID_TOP,
    OD_GRID_MIDDLE,
    OD_GRID_BOTTOM
} OdGridRule;

static const OdGuideItem menu_guide[] = {
    {"Up/Down", "Navigate"}, {"Enter", "Select"}, {"q", "Quit"}
};

static const OdGuideItem dashboard_guide[] = {
    {"Up/Down", "Scroll"}, {"r", "Refresh"}, {"q/Esc", "Back"}
};

static const OdGuideItem conflicts_apply_guide[] = {
    {"Up/Down", "Scroll"}, {"Enter", "Apply all"}, {"q/Esc", "Cancel"}
};

static const OdGuideItem conflicts_back_guide[] = {
    {"Up/Down", "Scroll"}, {"q/Esc", "Back"}
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

static size_t conflicts_table_capacity(size_t height) {
    size_t capacity = height > 14U ? (height - 14U) / 2U : 0U;
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
    int row_stride = height >= 30U ? 2 : 1;
    int panel_height = 4 + row_stride * (int)OD_MENU_COUNT;
    int composition_height = (int)logo_height + 4 + panel_height;
    int usable_height = (int)height - 3;
    int top = usable_height > composition_height ?
        (usable_height - composition_height) / 2 : 0;
    int panel_width = (int)((width * 2U) / 5U);
    if (panel_width < 52) panel_width = 52;
    if (panel_width > 84) panel_width = 84;
    if ((size_t)panel_width > width - 4U) panel_width = (int)width - 4;
    int panel_x = ((int)width - panel_width) / 2;
    int panel_y = top + (int)logo_height + 4;
    return (OdMenuLayout){
        .panel_x = panel_x,
        .panel_y = panel_y,
        .panel_width = panel_width,
        .panel_height = panel_height,
        .first_item_y = panel_y + 2,
        .row_stride = row_stride,
        .top = top,
        .logo_height = logo_height
    };
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
    int relative = y - layout.first_item_y;
    if (x < layout.panel_x || x >= layout.panel_x + layout.panel_width ||
        relative < 0 || relative % layout.row_stride != 0) {
        return (OdMouseTarget){0};
    }
    size_t item = (size_t)(relative / layout.row_stride);
    if (item >= (size_t)OD_MENU_COUNT) return (OdMouseTarget){0};
    return (OdMouseTarget){OD_MOUSE_MENU_ITEM, item};
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

OdMouseTarget od_conflicts_mouse_target(size_t width,
                                        size_t height,
                                        bool apply_available,
                                        int x,
                                        int y) {
    static const OdMouseAction apply_actions[] = {
        OD_MOUSE_NONE, OD_MOUSE_APPLY_ALL, OD_MOUSE_BACK
    };
    static const OdMouseAction back_actions[] = {
        OD_MOUSE_NONE, OD_MOUSE_BACK
    };
    if (width < 60U || height < 18U || y != (int)height - 1 ||
        x < 0 || x >= (int)width) {
        return (OdMouseTarget){0};
    }
    if (apply_available) {
        return (OdMouseTarget){
            guide_action_at(conflicts_apply_guide, apply_actions,
                            sizeof(apply_actions) / sizeof(apply_actions[0]), x),
            0U
        };
    }
    return (OdMouseTarget){
        guide_action_at(conflicts_back_guide, back_actions,
                        sizeof(back_actions) / sizeof(back_actions[0]), x),
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
        od_canvas_write_centered(canvas, top, "OPEN DOOR", OD_ROLE_PRIMARY, 1U);
    } else {
        const size_t banner_width = 80U;
        int logo_x = banner_width >= canvas->width ? 0 :
            (int)((canvas->width - banner_width) / 2U);
        for (size_t index = 0U; index < logo_height; ++index) {
            od_canvas_write(canvas, logo_x, top + (int)index, banner[index],
                            available_width(canvas, logo_x), OD_ROLE_PRIMARY, 1U);
        }
    }

    char project_line[512];
    (void)snprintf(project_line, sizeof(project_line), "Project  %s",
                   project == NULL ? "." : project);
    od_canvas_write_centered(canvas, top + (int)logo_height + 1,
                             project_line, OD_ROLE_DEFAULT, 1U);
    od_canvas_write_centered(canvas, top + (int)logo_height + 2,
                             "Pre-start port check", OD_ROLE_WARNING, 0U);

    od_canvas_box(canvas, layout.panel_x, layout.panel_y,
                  layout.panel_width, layout.panel_height,
                  ascii, OD_ROLE_FOCUSED_BORDER);
    for (size_t index = 0U; index < (size_t)OD_MENU_COUNT; ++index) {
        char line[96];
        bool is_selected = index == selected;
        (void)snprintf(line, sizeof(line), "%s %s",
                       is_selected ? ">" : " ", menu_labels[index]);
        od_canvas_write(canvas, layout.panel_x + 3,
                        layout.first_item_y + (int)index * layout.row_stride,
                        line, (size_t)(layout.panel_width - 6),
                        is_selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        is_selected ? 1U : 0U);
    }
    status_line(canvas, status);
    draw_guide(canvas, (int)canvas->height - 1, menu_guide,
               sizeof(menu_guide) / sizeof(menu_guide[0]));
}

void od_render_dashboard(OdCanvas *canvas,
                         const OdDashboard *dashboard,
                         const char *config_path,
                         const char *status,
                         bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    od_canvas_write(canvas, 2, 1, "OPEN DOOR / Dashboard",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    char path[512];
    (void)snprintf(path, sizeof(path), "Wanted ports  %s",
                   config_path == NULL ? ".ports.env" : config_path);
    od_canvas_write(canvas, 2, 2, path, available_width(canvas, 2),
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
    int widths[5] = {7, 10, 10, 0, 0};
    int flexible_width = content_width - widths[0] - widths[1] - widths[2];
    widths[3] = flexible_width >= 48 ? 24 : flexible_width / 2;
    widths[4] = flexible_width - widths[3];
    const char *headers[] = {"PORT", "STATUS", "CONFLICT", "PROCESS", "DIRECTORY"};
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
        const char *cells[] = {
            port,
            row->running ? "running" : "free",
            row->conflict ? "yes" : "no",
            row->process,
            row->directory
        };
        OdStyleRole role = row->conflict ? OD_ROLE_DANGER :
            (row->running ? OD_ROLE_SUCCESS : OD_ROLE_DEFAULT);
        unsigned attributes = row->conflict ? 1U : 0U;
        int row_y = grid_y + 3 + (int)visible * 2;
        draw_grid_row(canvas, grid_x, row_y, widths, cells, column_count,
                      ascii, role, attributes);
        draw_grid_rule(canvas, grid_x, row_y + 1, widths, column_count,
                       ascii, index + 1U == end ? OD_GRID_BOTTOM : OD_GRID_MIDDLE);
    }
    if (dashboard == NULL || dashboard->count == 0U) {
        draw_grid_rule(canvas, grid_x, grid_y + 3, widths, column_count,
                       ascii, OD_GRID_BOTTOM);
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          grid_y + 4, "No ports found",
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

void od_render_conflicts(OdCanvas *canvas,
                         const OdResolution *resolution,
                         size_t scroll,
                         bool apply_available,
                         bool prepared,
                         const char *status,
                         bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    od_canvas_write(canvas, 2, 1, "OPEN DOOR / Resolve conflicts",
                    available_width(canvas, 2), OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Pre-start check — occupied configured ports are conflicts",
                    available_width(canvas, 2), OD_ROLE_WARNING, 0U);

    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 7;
    int inner_width = box_width - 4;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height,
                  ascii, OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y, " Proposed changes ",
                    (size_t)inner_width, OD_ROLE_PRIMARY, 1U);

    if (!prepared) {
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          box_y + box_height / 2,
                          "Unable to prepare proposal",
                          OD_ROLE_DANGER, 1U);
    } else if (resolution == NULL || resolution->count == 0U) {
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          box_y + box_height / 2, "No conflicts found",
                          OD_ROLE_SUCCESS, 1U);
    } else {
        const size_t column_count = 2U;
        int grid_x = box_x + 2;
        int grid_y = box_y + 2;
        int grid_width = box_width - 4;
        int content_width = grid_width - (int)column_count - 1;
        int widths[2] = {content_width / 2,
                         content_width - content_width / 2};
        const char *headers[] = {"OLD PORT", "NEW PORT"};
        draw_grid_rule(canvas, grid_x, grid_y, widths, column_count,
                       ascii, OD_GRID_TOP);
        draw_grid_row(canvas, grid_x, grid_y + 1, widths, headers, column_count,
                      ascii, OD_ROLE_MUTED, 1U);
        draw_grid_rule(canvas, grid_x, grid_y + 2, widths, column_count,
                       ascii, OD_GRID_MIDDLE);

        size_t capacity = conflicts_table_capacity(canvas->height);
        size_t end = scroll + capacity;
        if (end > resolution->count) end = resolution->count;
        size_t visible = 0U;
        for (size_t index = scroll; index < end; ++index, ++visible) {
            char old_port[16];
            char new_port[16];
            (void)snprintf(old_port, sizeof(old_port), "%u",
                           (unsigned)resolution->items[index].old_port);
            (void)snprintf(new_port, sizeof(new_port), "%u",
                           (unsigned)resolution->items[index].new_port);
            const char *cells[] = {old_port, new_port};
            int row_y = grid_y + 3 + (int)visible * 2;
            draw_grid_row(canvas, grid_x, row_y, widths, cells, column_count,
                          ascii, OD_ROLE_DEFAULT, 0U);
            draw_grid_rule(canvas, grid_x, row_y + 1, widths, column_count,
                           ascii,
                           index + 1U == end ? OD_GRID_BOTTOM : OD_GRID_MIDDLE);
        }
        char page[96];
        size_t first = resolution->count == 0U ? 0U : scroll + 1U;
        (void)snprintf(page, sizeof(page), "Showing %zu–%zu of %zu conflict(s)",
                       first, end, resolution->count);
        od_canvas_write(canvas, box_x + 2, box_y + box_height - 2,
                        page, (size_t)inner_width, OD_ROLE_MUTED, 0U);
    }

    status_line(canvas, status);
    if (resolution != NULL && resolution->count > 0U && apply_available) {
        draw_guide(canvas, (int)canvas->height - 1, conflicts_apply_guide,
                   sizeof(conflicts_apply_guide) / sizeof(conflicts_apply_guide[0]));
    } else {
        draw_guide(canvas, (int)canvas->height - 1, conflicts_back_guide,
                   sizeof(conflicts_back_guide) / sizeof(conflicts_back_guide[0]));
    }
}
