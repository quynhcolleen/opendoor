#include "opendoor/screens.h"

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

const char *const *od_banner_lines(void) {
    return banner;
}

size_t od_banner_line_count(void) {
    return sizeof(banner) / sizeof(banner[0]);
}

static int maximum(int left, int right) {
    return left > right ? left : right;
}

void od_render_resize_required(OdCanvas *canvas) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    int middle = (int)(canvas->height / 2U);
    od_canvas_write_centered(canvas, middle - 1, "Terminal too small",
                             OD_ROLE_WARNING, 1U);
    od_canvas_write_centered(canvas, middle + 1, "Resize to at least 60 x 18",
                             OD_ROLE_MUTED, 0U);
}

void od_render_loading(OdCanvas *canvas,
                       OdLoadingStage stage,
                       unsigned spinner_frame,
                       unsigned elapsed_milliseconds,
                       bool reduced_motion,
                       bool ascii,
                       size_t warning_count) {
    static const char *const labels[] = {
        "Project files",
        "Kernel sockets",
        "Process ownership",
        "Docker",
        "Candidate reconciliation"
    };
    static const char *const ascii_spinner[] = {"|", "/", "-", "\\"};
    static const char *const unicode_spinner[] = {"◐", "◓", "◑", "◒"};
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    int start = maximum(1, (int)(canvas->height - OD_LOAD_STAGE_COUNT - 8U) / 2);
    od_canvas_write_centered(canvas, start, "OPEN DOOR", OD_ROLE_PRIMARY, 1U);
    od_canvas_write_centered(canvas, start + 2, "Discovering ports", OD_ROLE_DEFAULT, 1U);
    int left = maximum(1, (int)(canvas->width / 2U) - 18);
    for (size_t index = 0U; index < OD_LOAD_STAGE_COUNT; ++index) {
        char line[96];
        const char *marker;
        if (index < (size_t)stage) {
            marker = "[done]";
        } else if (index == (size_t)stage) {
            if (reduced_motion) {
                marker = "[....]";
            } else {
                const char *spinner = ascii ? ascii_spinner[spinner_frame % 4U] :
                                               unicode_spinner[spinner_frame % 4U];
                (void)snprintf(line, sizeof(line), "[ %s ] %s", spinner, labels[index]);
                marker = NULL;
            }
        } else {
            marker = "[    ]";
        }
        if (marker != NULL) (void)snprintf(line, sizeof(line), "%s %s", marker, labels[index]);
        OdThemeRole role = index < (size_t)stage ? OD_ROLE_SUCCESS :
                           (index == (size_t)stage ? OD_ROLE_PRIMARY : OD_ROLE_MUTED);
        od_canvas_write(canvas, left, start + 4 + (int)index, line,
                        canvas->width > (size_t)left ? canvas->width - (size_t)left : 0U,
                        role, index == (size_t)stage ? 1U : 0U);
    }
    char footer[96];
    (void)snprintf(footer, sizeof(footer), "Elapsed: %u ms  Warnings: %zu",
                   elapsed_milliseconds, warning_count);
    od_canvas_write_centered(canvas, (int)canvas->height - 2, footer,
                             warning_count == 0U ? OD_ROLE_MUTED : OD_ROLE_WARNING, 0U);
    od_canvas_write_centered(canvas, (int)canvas->height - 1,
                             "Esc Skip animation", OD_ROLE_MUTED, 0U);
}

static size_t menu_banner_height(size_t viewport_width,
                                 size_t viewport_height,
                                 bool ascii) {
    return !ascii && viewport_width >= 84U && viewport_height >= 26U ?
        od_banner_line_count() : 1U;
}

static int menu_row_stride(size_t viewport_height) {
    return viewport_height >= 36U ? 2 : 1;
}

size_t od_menu_page_size(size_t viewport_width,
                         size_t viewport_height,
                         size_t item_count,
                         bool ascii) {
    size_t content_height = viewport_height > 3U ? viewport_height - 3U : 1U;
    size_t overhead = menu_banner_height(viewport_width, viewport_height, ascii) + 4U;
    size_t panel_height = content_height > overhead ? content_height - overhead : 5U;
    size_t usable_rows = panel_height > 4U ? panel_height - 4U : 1U;
    size_t visible = usable_rows / (size_t)menu_row_stride(viewport_height);
    if (visible == 0U) visible = 1U;
    return visible < item_count ? visible : item_count;
}

size_t od_menu_page_start(size_t selected, size_t page_size) {
    return page_size == 0U ? 0U : selected / page_size * page_size;
}

void od_main_menu_layout(size_t viewport_width,
                         size_t viewport_height,
                         size_t item_count,
                         size_t selected,
                         bool ascii,
                         OdMenuLayout *layout) {
    if (layout == NULL) return;
    size_t visible = od_menu_page_size(viewport_width, viewport_height,
                                       item_count, ascii);
    int stride = menu_row_stride(viewport_height);
    size_t logo_height = menu_banner_height(viewport_width, viewport_height, ascii);
    int box_height = (int)visible * stride + 4;
    int content_height = viewport_height > 3U ? (int)viewport_height - 3 : 1;
    int composition_height = (int)logo_height + 4 + box_height;
    int composition_y = content_height > composition_height ?
        (content_height - composition_height) / 2 : 0;
    int box_width = (int)((viewport_width * 2U) / 5U);
    if (box_width < 52) box_width = 52;
    if (box_width > 84) box_width = 84;
    if ((size_t)box_width > viewport_width - 4U) box_width = (int)viewport_width - 4;
    *layout = (OdMenuLayout){
        .box_x = ((int)viewport_width - box_width) / 2,
        .box_y = composition_y + (int)logo_height + 4,
        .box_width = box_width,
        .box_height = box_height,
        .first_item_y = composition_y + (int)logo_height + 6,
        .row_stride = stride,
        .visible_count = visible,
        .page_start = od_menu_page_start(selected, visible)
    };
}

typedef struct {
    const char *key;
    const char *label;
} OdGuideItem;

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

static int centered_text_x(int left, int width, const char *text) {
    int text_width = (int)strlen(text);
    return left + (text_width < width ? (width - text_width) / 2 : 0);
}

static void write_centered_in(OdCanvas *canvas,
                              int left,
                              int width,
                              int y,
                              const char *text,
                              OdThemeRole role,
                              unsigned attributes) {
    int x = centered_text_x(left, width, text);
    od_canvas_write(canvas, x, y, text,
                    width > 0 ? (size_t)width : 0U, role, attributes);
}

void od_render_main_menu(OdCanvas *canvas, const OdMenuView *view, bool ascii) {
    static const char *const first_run[] = {
        "Discover this project",
        "Open listener explorer",
        "Settings and appearance",
        "Help",
        "Quit"
    };
    static const char *const configured[] = {
        "Open dashboard",
        "Resolve conflicts",
        "Rescan",
        "Edit project profile",
        "Settings and appearance",
        "Help",
        "Quit"
    };
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    const char *const *items = view->configured ? configured : first_run;
    size_t item_count = view->configured ? sizeof(configured) / sizeof(configured[0]) :
                                          sizeof(first_run) / sizeof(first_run[0]);
    OdMenuLayout layout;
    od_main_menu_layout(canvas->width, canvas->height, item_count,
                        view->selected_item, ascii, &layout);
    int logo_height = (int)menu_banner_height(canvas->width, canvas->height, ascii);
    int composition_y = layout.box_y - logo_height - 4;
    if (logo_height == 1) {
        od_canvas_write_centered(canvas, composition_y, "OPEN DOOR",
                                 OD_ROLE_PRIMARY, 1U);
    } else {
        const size_t banner_width = 80U;
        int logo_x = banner_width >= canvas->width ? 0 :
                     (int)((canvas->width - banner_width) / 2U);
        for (size_t index = 0U; index < od_banner_line_count(); ++index) {
            od_canvas_write(canvas, logo_x, composition_y + (int)index,
                            banner[index], canvas->width - (size_t)logo_x,
                            OD_ROLE_PRIMARY, 1U);
        }
    }
    char project[512];
    (void)snprintf(project, sizeof(project), "Project  %s",
                   view->project_name == NULL ? "." : view->project_name);
    od_canvas_write_centered(canvas, composition_y + logo_height + 1,
                             project, OD_ROLE_DEFAULT, 1U);
    od_canvas_write_centered(canvas, composition_y + logo_height + 2,
                             view->configured ? "Profile: configured" :
                                                "Profile: not configured",
                             view->configured ? OD_ROLE_SUCCESS : OD_ROLE_WARNING, 0U);

    size_t page_end = layout.page_start + layout.visible_count;
    if (page_end > item_count) page_end = item_count;
    od_canvas_box(canvas, layout.box_x, layout.box_y,
                  layout.box_width, layout.box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    for (size_t index = layout.page_start; index < page_end; ++index) {
        char line[96];
        bool selected = index == view->selected_item;
        (void)snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", items[index]);
        od_canvas_write(canvas, layout.box_x + 3,
                        layout.first_item_y +
                            (int)(index - layout.page_start) * layout.row_stride,
                        line, (size_t)(layout.box_width - 6),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    char page[64];
    size_t page_count = (item_count + layout.visible_count - 1U) /
                        layout.visible_count;
    (void)snprintf(page, sizeof(page), "Menu page %zu/%zu",
                   layout.page_start / layout.visible_count + 1U, page_count);
    od_canvas_write(canvas, layout.box_x + 3,
                    layout.box_y + layout.box_height - 2, page,
                    (size_t)(layout.box_width - 6), OD_ROLE_MUTED, 0U);
    if (view->status != NULL) {
        od_canvas_write(canvas, 1, (int)canvas->height - 2, view->status,
                        canvas->width - 2U, OD_ROLE_MUTED, 0U);
    }
    static const OdGuideItem guide[] = {
        {"Up/Down", "Navigate"}, {"Enter", "Select"},
        {"?", "Help"}, {"q", "Quit"}
    };
    draw_guide(canvas, (int)canvas->height - 1, guide,
               sizeof(guide) / sizeof(guide[0]));
}

typedef enum {
    OD_TABLE_USE,
    OD_TABLE_REVIEW,
    OD_TABLE_CONFIDENCE,
    OD_TABLE_NAME,
    OD_TABLE_SOURCE,
    OD_TABLE_VARIABLE,
    OD_TABLE_PORT,
    OD_TABLE_GROUP,
    OD_TABLE_FIELD_COUNT
} OdTableField;

typedef struct {
    OdTableField fields[OD_TABLE_FIELD_COUNT];
    size_t widths[OD_TABLE_FIELD_COUNT];
    size_t count;
    size_t total_width;
} OdOnboardingTableLayout;

static const char *table_header(OdTableField field, bool abbreviated) {
    static const char *const headers[] = {
        "USE", "REVIEW", "CONFIDENCE", "NAME",
        "SOURCE", "VARIABLE", "PORT", "GROUP"
    };
    if (abbreviated && field == OD_TABLE_REVIEW) return "REV";
    return headers[field];
}

static const char *candidate_confidence(const OdCandidate *candidate) {
    if (candidate->confidence == OD_CONFIDENCE_CONFIRMED) return "Confirmed";
    if (candidate->confidence == OD_CONFIDENCE_LIKELY) return "Likely";
    return "Possible";
}

static const char *candidate_field(const OdCandidate *candidate,
                                   bool reviewed,
                                   OdTableField field,
                                   char *number,
                                   size_t number_capacity) {
    switch (field) {
        case OD_TABLE_USE: return candidate->selected ? "[x]" : "[ ]";
        case OD_TABLE_REVIEW: return reviewed ? "yes" : "-";
        case OD_TABLE_CONFIDENCE: return candidate_confidence(candidate);
        case OD_TABLE_NAME: return candidate->name;
        case OD_TABLE_SOURCE:
            return candidate->sources.count == 0U ? "unknown" :
                   candidate->sources.items[0];
        case OD_TABLE_VARIABLE: return candidate->variable;
        case OD_TABLE_PORT:
            (void)snprintf(number, number_capacity, "%u", (unsigned)candidate->port);
            return number;
        case OD_TABLE_GROUP: return candidate->group;
        case OD_TABLE_FIELD_COUNT: return "";
    }
    return "";
}

static bool table_field_flexible(OdTableField field) {
    return field == OD_TABLE_NAME || field == OD_TABLE_SOURCE ||
           field == OD_TABLE_VARIABLE || field == OD_TABLE_GROUP;
}

static size_t table_field_minimum(OdTableField field, int mode) {
    switch (field) {
        case OD_TABLE_USE: return 3U;
        case OD_TABLE_REVIEW: return mode == 2 ? 6U : 3U;
        case OD_TABLE_CONFIDENCE: return 10U;
        case OD_TABLE_NAME: return mode == 2 ? 10U : 12U;
        case OD_TABLE_SOURCE: return 12U;
        case OD_TABLE_VARIABLE: return mode == 0 ? 15U : 18U;
        case OD_TABLE_PORT: return 5U;
        case OD_TABLE_GROUP: return 8U;
        case OD_TABLE_FIELD_COUNT: return 1U;
    }
    return 1U;
}

static void onboarding_table_layout(const OdOnboarding *onboarding,
                                    size_t viewport_width,
                                    OdOnboardingTableLayout *layout) {
    static const OdTableField wide_fields[] = {
        OD_TABLE_USE, OD_TABLE_REVIEW, OD_TABLE_CONFIDENCE, OD_TABLE_NAME,
        OD_TABLE_SOURCE, OD_TABLE_VARIABLE, OD_TABLE_PORT, OD_TABLE_GROUP
    };
    static const OdTableField medium_fields[] = {
        OD_TABLE_USE, OD_TABLE_REVIEW, OD_TABLE_CONFIDENCE,
        OD_TABLE_NAME, OD_TABLE_VARIABLE, OD_TABLE_PORT
    };
    static const OdTableField compact_fields[] = {
        OD_TABLE_USE, OD_TABLE_REVIEW, OD_TABLE_NAME,
        OD_TABLE_VARIABLE, OD_TABLE_PORT
    };
    int box_width = viewport_width > 2U ? (int)viewport_width - 2 : 1;
    int mode = box_width >= 110 ? 2 : (box_width >= 76 ? 1 : 0);
    const OdTableField *fields = mode == 2 ? wide_fields :
                                 (mode == 1 ? medium_fields : compact_fields);
    size_t count = mode == 2 ? sizeof(wide_fields) / sizeof(wide_fields[0]) :
                   (mode == 1 ? sizeof(medium_fields) / sizeof(medium_fields[0]) :
                                sizeof(compact_fields) / sizeof(compact_fields[0]));
    size_t table_width = box_width > 4 ? (size_t)(box_width - 4) : 1U;
    size_t separator_width = count > 0U ? (count - 1U) * 3U : 0U;
    size_t used = separator_width;
    size_t demand[OD_TABLE_FIELD_COUNT] = {0U};
    *layout = (OdOnboardingTableLayout){0};
    layout->count = count;
    layout->total_width = table_width;
    for (size_t column = 0U; column < count; ++column) {
        OdTableField field = fields[column];
        layout->fields[column] = field;
        layout->widths[column] = table_field_minimum(field, mode);
        demand[column] = od_text_columns(table_header(field, mode != 2));
        if (demand[column] < layout->widths[column]) demand[column] = layout->widths[column];
        used += layout->widths[column];
    }
    if (onboarding != NULL) {
        for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
            const OdCandidate *candidate = &onboarding->candidates.items[index];
            for (size_t column = 0U; column < count; ++column) {
                char number[16];
                const char *value = candidate_field(candidate,
                    onboarding->reviewed != NULL && onboarding->reviewed[index],
                    layout->fields[column], number, sizeof(number));
                size_t columns = od_text_columns(value);
                if (columns > demand[column]) demand[column] = columns;
            }
        }
    }
    if (used > table_width) {
        size_t excess = used - table_width;
        while (excess > 0U) {
            bool changed = false;
            for (size_t column = count; column > 0U && excess > 0U; --column) {
                size_t at = column - 1U;
                if (table_field_flexible(layout->fields[at]) &&
                    layout->widths[at] > 4U) {
                    --layout->widths[at];
                    --excess;
                    changed = true;
                }
            }
            if (!changed) break;
        }
        return;
    }
    size_t extra = table_width - used;
    while (extra > 0U) {
        size_t best = SIZE_MAX;
        size_t largest_deficit = 0U;
        for (size_t column = 0U; column < count; ++column) {
            if (!table_field_flexible(layout->fields[column])) continue;
            size_t deficit = demand[column] > layout->widths[column] ?
                demand[column] - layout->widths[column] : 0U;
            if (best == SIZE_MAX || deficit > largest_deficit) {
                best = column;
                largest_deficit = deficit;
            }
        }
        if (best == SIZE_MAX) break;
        if (largest_deficit == 0U) {
            for (size_t column = 0U; column < count && extra > 0U; ++column) {
                if (table_field_flexible(layout->fields[column])) {
                    ++layout->widths[column];
                    --extra;
                }
            }
        } else {
            ++layout->widths[best];
            --extra;
        }
    }
}

static size_t wrapped_line_count(const char *text, size_t width) {
    size_t columns = od_text_columns(text);
    if (width == 0U || columns == 0U) return 1U;
    return (columns + width - 1U) / width;
}

static size_t candidate_row_height(const OdOnboarding *onboarding,
                                   size_t index,
                                   const OdOnboardingTableLayout *layout) {
    size_t height = 1U;
    if (onboarding == NULL || index >= onboarding->candidates.count) return height;
    const OdCandidate *candidate = &onboarding->candidates.items[index];
    for (size_t column = 0U; column < layout->count; ++column) {
        char number[16];
        const char *value = candidate_field(candidate,
            onboarding->reviewed != NULL && onboarding->reviewed[index],
            layout->fields[column], number, sizeof(number));
        size_t lines = wrapped_line_count(value, layout->widths[column]);
        if (lines > height) height = lines;
    }
    return height;
}

static size_t onboarding_row_height(const OdOnboarding *onboarding,
                                    const OdOnboardingTableLayout *layout) {
    size_t height = 1U;
    if (onboarding == NULL) return height;
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        size_t candidate_height = candidate_row_height(onboarding, index, layout);
        if (candidate_height > height) height = candidate_height;
    }
    return height;
}

static size_t onboarding_data_lines(size_t viewport_height) {
    return viewport_height > 13U ? viewport_height - 13U : 1U;
}

size_t od_onboarding_row_height_for_viewport(const OdOnboarding *onboarding,
                                              size_t viewport_width,
                                              size_t viewport_height) {
    OdOnboardingTableLayout layout;
    onboarding_table_layout(onboarding, viewport_width, &layout);
    size_t height = onboarding_row_height(onboarding, &layout);
    size_t available = onboarding_data_lines(viewport_height);
    return height > available ? available : height;
}

size_t od_onboarding_page_size_for_viewport(const OdOnboarding *onboarding,
                                             size_t viewport_width,
                                             size_t viewport_height) {
    size_t row_height = od_onboarding_row_height_for_viewport(
        onboarding, viewport_width, viewport_height);
    size_t available = onboarding_data_lines(viewport_height);
    size_t rows = (available + 1U) / (row_height + 1U);
    return rows == 0U ? 1U : rows;
}

static void draw_inner_rule(OdCanvas *canvas,
                            int x,
                            int y,
                            int width,
                            bool ascii,
                            OdThemeRole role) {
    const char *glyph = ascii ? "-" : "─";
    for (int column = 1; column < width - 1; ++column) {
        od_canvas_put(canvas, x + column, y, glyph, role, 0U);
    }
}

static void fill_table_line(OdCanvas *canvas,
                            int x,
                            int y,
                            size_t width,
                            OdThemeRole role,
                            unsigned attributes) {
    for (size_t column = 0U; column < width; ++column) {
        od_canvas_put(canvas, x + (int)column, y, " ", role, attributes);
    }
}

static void draw_table_line(OdCanvas *canvas,
                            const OdOnboardingTableLayout *layout,
                            const OdCandidate *candidate,
                            bool reviewed,
                            bool header,
                            bool abbreviated,
                            size_t wrapped_line,
                            int x,
                            int y,
                            const char *divider,
                            OdThemeRole role,
                            unsigned attributes) {
    fill_table_line(canvas, x, y, layout->total_width, role, attributes);
    int column_x = x;
    for (size_t column = 0U; column < layout->count; ++column) {
        OdTableField field = layout->fields[column];
        char number[16];
        const char *value = header ? table_header(field, abbreviated) :
            candidate_field(candidate, reviewed, field, number, sizeof(number));
        od_canvas_write_slice(canvas, column_x, y, value,
                              wrapped_line * layout->widths[column],
                              layout->widths[column], role, attributes);
        column_x += (int)layout->widths[column];
        if (column + 1U < layout->count) {
            od_canvas_put(canvas, column_x + 1, y, divider, role, attributes);
            column_x += 3;
        }
    }
}

void od_render_onboarding(OdCanvas *canvas,
                          const OdOnboarding *onboarding,
                          bool ascii,
                          const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Discover this project", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    size_t reviewed_count = 0U;
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        if (onboarding->reviewed[index]) ++reviewed_count;
    }
    char summary[160];
    (void)snprintf(summary, sizeof(summary),
                   "Review every candidate • %zu of %zu reviewed • Space toggles use",
                   reviewed_count, onboarding->candidates.count);
    od_canvas_write(canvas, 2, 2, summary, canvas->width - 4U,
                    reviewed_count == onboarding->candidates.count ? OD_ROLE_SUCCESS : OD_ROLE_MUTED,
                    0U);

    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    const char *divider = ascii ? "|" : "│";
    OdOnboardingTableLayout table;
    onboarding_table_layout(onboarding, canvas->width, &table);
    draw_table_line(canvas, &table, NULL, false, true,
                    box_width < 110, 0U, box_x + 2, box_y + 1,
                    divider, OD_ROLE_MUTED, 1U);
    draw_inner_rule(canvas, box_x, box_y + 2, box_width, ascii,
                    OD_ROLE_INACTIVE_BORDER);

    size_t start = od_onboarding_page_start(onboarding);
    size_t row_height = od_onboarding_row_height_for_viewport(
        onboarding, canvas->width, canvas->height);
    size_t capacity = od_onboarding_page_size_for_viewport(
        onboarding, canvas->width, canvas->height);
    if (capacity > onboarding->page_size) capacity = onboarding->page_size;
    size_t end = start + capacity;
    if (end > onboarding->candidates.count) end = onboarding->candidates.count;
    bool page_has_clipped_row = false;
    for (size_t index = start; index < end; ++index) {
        const OdCandidate *candidate = &onboarding->candidates.items[index];
        if (candidate_row_height(onboarding, index, &table) > row_height) {
            page_has_clipped_row = true;
        }
        int row_y = box_y + 3 +
                    (int)(index - start) * (int)(row_height + 1U);
        bool selected = index == onboarding->selected;
        for (size_t line = 0U; line < row_height; ++line) {
            draw_table_line(canvas, &table, candidate, onboarding->reviewed[index],
                            false, false, line, box_x + 2, row_y + (int)line,
                            divider,
                            selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                            selected ? 1U : 0U);
        }
        if (index + 1U < end) {
            draw_inner_rule(canvas, box_x, row_y + (int)row_height,
                            box_width, ascii,
                            OD_ROLE_INACTIVE_BORDER);
        }
    }
    if (onboarding->candidates.count == 0U) {
        write_centered_in(canvas, box_x + 1, box_width - 2,
                          box_y + box_height / 2,
                          "No candidates found - press a to add a service",
                          OD_ROLE_MUTED, 0U);
    }
    char page[80];
    if (page_has_clipped_row) {
        (void)snprintf(page, sizeof(page),
                       "Page %zu/%zu  Long row continues in d Details",
                       od_onboarding_page(onboarding) + 1U,
                       od_onboarding_page_count(onboarding));
    } else {
        (void)snprintf(page, sizeof(page), "Page %zu/%zu",
                       od_onboarding_page(onboarding) + 1U,
                       od_onboarding_page_count(onboarding));
    }
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 3, status,
                        canvas->width - 4U, OD_ROLE_WARNING, 0U);
    }
    static const OdGuideItem guide[] = {
        {"Up/Down", "Select"}, {"PgUp/PgDn", "Page"}, {"Space", "Use"},
        {"Enter", "Review"}, {"d", "Details"}, {"e", "Edit"},
        {"a", "Add"}, {"s", "Continue"}, {"Esc", "Back"}
    };
    draw_guide(canvas, (int)canvas->height - 1, guide,
               sizeof(guide) / sizeof(guide[0]));
}

static const char *dashboard_sort_name(const OdDashboard *dashboard,
                                       OdDashboardWidget widget) {
    static const char *const service_names[] = {
        "name", "group", "variable", "preferred", "port", "status", "conflict"
    };
    static const char *const listener_names[] = {
        "port", "protocol", "bind", "process", "pid", "user", "source"
    };
    static const char *const docker_names[] = {
        "container", "host", "container-port", "protocol", "project"
    };
    if (widget == OD_WIDGET_SERVICES && dashboard->sort < OD_SERVICE_SORT_COUNT) {
        return service_names[(size_t)dashboard->sort];
    }
    if (widget == OD_WIDGET_LISTENERS &&
        dashboard->listener_sort < OD_LISTENER_SORT_COUNT) {
        return listener_names[(size_t)dashboard->listener_sort];
    }
    if (widget == OD_WIDGET_DOCKER && dashboard->docker_sort < OD_DOCKER_SORT_COUNT) {
        return docker_names[(size_t)dashboard->docker_sort];
    }
    return "preferred";
}

static bool dashboard_sort_ascending(const OdDashboard *dashboard,
                                     OdDashboardWidget widget) {
    if (widget == OD_WIDGET_SERVICES) return dashboard->sort_ascending;
    if (widget == OD_WIDGET_LISTENERS) return dashboard->listener_sort_ascending;
    if (widget == OD_WIDGET_DOCKER) return dashboard->docker_sort_ascending;
    return dashboard->conflict_sort_ascending;
}

void od_render_dashboard(OdCanvas *canvas,
                         OdDashboard *dashboard,
                         bool ascii,
                         OdHitMap *hit_map,
                         const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    od_hitmap_free(hit_map);
    od_hitmap_init(hit_map);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }

    char header[384];
    char refresh[64];
    (void)snprintf(refresh, sizeof(refresh),
                   dashboard->auto_refresh ? "auto %us" : "manual",
                   dashboard->refresh_seconds);
    (void)snprintf(header, sizeof(header),
                   "OPEN DOOR / %s / profile: %s / scan #%llu / refresh: %s%s",
                   dashboard->project_name,
                   dashboard->profile_saved ? "saved" : "listener-only",
                   (unsigned long long)dashboard->snapshot->generation,
                   refresh,
                   dashboard->snapshot->process_permissions_limited ?
                       "  [process details limited]" : "");
    od_canvas_write(canvas, 1, 0, header, canvas->width - 2U, OD_ROLE_PRIMARY, 1U);

    char summary[256];
    (void)snprintf(summary, sizeof(summary),
                   "Managed %zu  Available %zu  Conflicts %zu  Reassigned %zu  Listeners %zu  Docker %zu",
                   dashboard->summary.managed, dashboard->summary.available,
                   dashboard->summary.conflicts, dashboard->summary.reassigned,
                   dashboard->summary.listeners, dashboard->summary.docker_mappings);
    od_canvas_write(canvas, 1, 2, summary, canvas->width - 2U,
                    dashboard->summary.conflicts == 0U ? OD_ROLE_SUCCESS : OD_ROLE_WARNING, 0U);

    const int footer_y = (int)canvas->height - 1;
    const int status_y = footer_y - 1;
    const int content_y = 4;
    const int content_height = status_y - content_y;
    if (status != NULL) {
        od_canvas_write(canvas, 1, status_y, status, canvas->width - 2U,
                        OD_ROLE_MUTED, 0U);
    }
    static const OdGuideItem dashboard_guide[] = {
        {"PgUp/PgDn", "Page"}, {"Up/Down", "Select"}, {"Tab", "Focus"},
        {"e", "Expand"}, {"d", "Details"}, {"/", "Search"}, {"Esc", "Back"}
    };
    draw_guide(canvas, footer_y, dashboard_guide,
               sizeof(dashboard_guide) / sizeof(dashboard_guide[0]));

    static const char *const widget_names[] = {
        "Services", "Conflicts", "Host listeners", "Docker mappings"
    };

    /* Keep hit-region allocation deliberately local to rendering. A missed mouse
       region after allocation failure never affects the keyboard-complete UI. */
#define ADD_HIT(X, Y, W, H, ACTION, WIDGET, TARGET)                              \
    do {                                                                          \
        OdHitRegion *grown__ = realloc(hit_map->items,                             \
            (hit_map->count + 1U) * sizeof(*hit_map->items));                      \
        if (grown__ != NULL) {                                                     \
            hit_map->items = grown__;                                              \
            hit_map->items[hit_map->count++] = (OdHitRegion){                      \
                (X), (Y), (W), (H), (ACTION), (WIDGET), (TARGET)                   \
            };                                                                     \
        }                                                                          \
    } while (0)

#define ADD_COLUMN_HIT(TEXT, LABEL, X, Y, WIDGET, TARGET)                         \
    do {                                                                          \
        const char *column__ = strstr((TEXT), (LABEL));                           \
        if (column__ != NULL) {                                                    \
            ADD_HIT((X) + (int)(column__ - (TEXT)), (Y),                          \
                    (int)strlen(LABEL), 1, OD_HIT_SORT_COLUMN,                    \
                    (WIDGET), (size_t)(TARGET));                                  \
        }                                                                          \
    } while (0)

#define DRAW_TITLE(X, Y, W, WIDGET)                                                \
    do {                                                                          \
        char title__[96];                                                          \
        (void)snprintf(title__, sizeof(title__), "%s%s  sort:%s%c",              \
                       widget_names[(size_t)(WIDGET)],                              \
                       dashboard->focused == (WIDGET) ? "  [focused]" : "",       \
                       dashboard_sort_name(dashboard, (WIDGET)),                   \
                       dashboard_sort_ascending(dashboard, (WIDGET)) ? '^' : 'v'); \
        od_canvas_write(canvas, (X) + 2, (Y), title__,                             \
                        (size_t)((W) > 4 ? (W) - 4 : 0),                           \
                        dashboard->focused == (WIDGET) ? OD_ROLE_PRIMARY :         \
                                                         OD_ROLE_MUTED, 1U);        \
        ADD_HIT((X), (Y), (W), 1, OD_HIT_TOGGLE_EXPAND, (WIDGET), 0U);             \
    } while (0)

#define DRAW_BOX(X, Y, W, H, WIDGET)                                              \
    do {                                                                          \
        od_canvas_box(canvas, (X), (Y), (W), (H), ascii,                           \
                      dashboard->focused == (WIDGET) ? OD_ROLE_FOCUSED_BORDER :   \
                                                         OD_ROLE_INACTIVE_BORDER); \
        DRAW_TITLE((X), (Y), (W), (WIDGET));                                      \
    } while (0)

#define DRAW_SCROLL_CONTROLS(X, Y, W, WIDGET)                                     \
    do {                                                                          \
        int up_x__ = (X) + (W) - 8;                                               \
        int down_x__ = (X) + (W) - 4;                                             \
        od_canvas_write(canvas, up_x__, (Y), ascii ? "[^]" : "[↑]", 3U,        \
                        OD_ROLE_MUTED, 0U);                                        \
        od_canvas_write(canvas, down_x__, (Y), ascii ? "[v]" : "[↓]", 3U,      \
                        OD_ROLE_MUTED, 0U);                                        \
        ADD_HIT(up_x__, (Y), 3, 1, OD_HIT_SCROLL_UP, (WIDGET), 0U);                \
        ADD_HIT(down_x__, (Y), 3, 1, OD_HIT_SCROLL_DOWN, (WIDGET), 0U);            \
    } while (0)

#define DRAW_SERVICES(X, Y, W, H)                                                  \
    do {                                                                          \
        int inner_width__ = (W) - 4;                                               \
        size_t rows__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                         \
        od_dashboard_set_page_size(dashboard, rows__);                             \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_SERVICES);                         \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_SERVICES);                  \
        const char *columns__ = inner_width__ >= 72 ?                              \
            "SERVICE        GROUP   VARIABLE      PREF  PORT STATUS   CONFLICT" : \
            "SERVICE       VARIABLE      PREF  PORT STATUS   CFL";                \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, columns__,                       \
                        (size_t)(inner_width__ > 0 ? inner_width__ : 0),            \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_COLUMN_HIT(columns__, "SERVICE", (X) + 2, (Y) + 2,                   \
                       OD_WIDGET_SERVICES, OD_SERVICE_SORT_NAME);                  \
        if (inner_width__ >= 72)                                                   \
            ADD_COLUMN_HIT(columns__, "GROUP", (X) + 2, (Y) + 2,                 \
                           OD_WIDGET_SERVICES, OD_SERVICE_SORT_GROUP);             \
        ADD_COLUMN_HIT(columns__, "VARIABLE", (X) + 2, (Y) + 2,                  \
                       OD_WIDGET_SERVICES, OD_SERVICE_SORT_VARIABLE);              \
        ADD_COLUMN_HIT(columns__, "PREF", (X) + 2, (Y) + 2,                      \
                       OD_WIDGET_SERVICES, OD_SERVICE_SORT_PREFERRED);             \
        ADD_COLUMN_HIT(columns__, "PORT", (X) + 2, (Y) + 2,                      \
                       OD_WIDGET_SERVICES, OD_SERVICE_SORT_SELECTED);              \
        ADD_COLUMN_HIT(columns__, "STATUS", (X) + 2, (Y) + 2,                    \
                       OD_WIDGET_SERVICES, OD_SERVICE_SORT_STATUS);                \
        ADD_COLUMN_HIT(columns__, inner_width__ >= 72 ? "CONFLICT" : "CFL",      \
                       (X) + 2, (Y) + 2, OD_WIDGET_SERVICES,                       \
                       OD_SERVICE_SORT_CONFLICT);                                  \
        size_t end__ = dashboard->page_start + rows__;                            \
        if (end__ > dashboard->visible_count) end__ = dashboard->visible_count;   \
        for (size_t visible__ = dashboard->page_start; visible__ < end__;          \
             ++visible__) {                                                        \
            const OdServiceRow *row__ =                                            \
                &dashboard->services[dashboard->visible_order[visible__]];          \
            char line__[384];                                                      \
            if (inner_width__ >= 72) {                                             \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-14.14s %-7.7s %-12.12s %5u %5u %-8.8s %-8s",               \
                    row__->service, row__->group, row__->variable,                 \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status),                         \
                    row__->conflict ? "YES" : "NO");                             \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-13.13s %-11.11s %5u %5u %-8.8s %-3s",                      \
                    row__->service, row__->variable,                               \
                    (unsigned)row__->preferred_port, (unsigned)row__->selected_port,\
                    od_service_status_name(row__->status),                         \
                    row__->conflict ? "YES" : "NO");                             \
            }                                                                      \
            bool selected__ = visible__ == dashboard->selected_visible;            \
            OdThemeRole role__ = selected__ ? OD_ROLE_SELECTED :                   \
                (row__->conflict ? OD_ROLE_DANGER :                                \
                 ((row__->status == OD_SERVICE_REASSIGNED ||                       \
                   row__->status == OD_SERVICE_STALE) ? OD_ROLE_WARNING :          \
                                                            OD_ROLE_DEFAULT));      \
            int row_y__ = (Y) + 3 + (int)(visible__ - dashboard->page_start);      \
            od_canvas_write(canvas, (X) + 2, row_y__, line__,                      \
                            (size_t)(inner_width__ > 0 ? inner_width__ : 0),        \
                            role__, selected__ ? 2U : 0U);                          \
            ADD_HIT((X) + 1, row_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,               \
                    OD_WIDGET_SERVICES, visible__);                                \
        }                                                                          \
        if (dashboard->visible_count == 0U) {                                      \
            const char *empty__ = dashboard->search[0] != '\0' ?                  \
                "No services match this search" :                                 \
                (dashboard->profile_saved ? "No services are configured" :        \
                                            "Listener explorer only");             \
            write_centered_in(canvas, (X) + 1, (W) - 2, (Y) + (H) / 2,            \
                              empty__, OD_ROLE_MUTED, 0U);                         \
            if (dashboard->search[0] == '\0') {                                   \
                const char *next__ = dashboard->profile_saved ?                   \
                    "Edit the project profile to add one" :                       \
                    "Run Discover this project to manage services";               \
                write_centered_in(canvas, (X) + 1, (W) - 2,                       \
                                  (Y) + (H) / 2 + 1, next__,                       \
                                  OD_ROLE_PRIMARY, 0U);                            \
            }                                                                     \
        }                                                                          \
        char page__[192];                                                          \
        size_t page_number__ = dashboard->visible_count == 0U ? 0U :               \
            dashboard->page_start / dashboard->page_size + 1U;                    \
        size_t page_count__ = dashboard->visible_count == 0U ? 0U :                \
            (dashboard->visible_count + dashboard->page_size - 1U) /               \
                dashboard->page_size;                                              \
        const OdServiceRow *selected_row__ = od_dashboard_selected_service(dashboard);\
        (void)snprintf(page__, sizeof(page__),                                     \
                       "Page %zu/%zu  %zu shown%s%s%s",                           \
                       page_number__, page_count__, dashboard->visible_count,       \
                       dashboard->search[0] == '\0' ? "" : "  Search: ",          \
                       dashboard->search[0] == '\0' ? "" : dashboard->search,     \
                       selected_row__ != NULL && selected_row__->conflict ?         \
                           "  ! conflict" : "");                                  \
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)(inner_width__ > 0 ? inner_width__ : 0),            \
                        selected_row__ != NULL && selected_row__->conflict ?        \
                            OD_ROLE_WARNING : OD_ROLE_MUTED, 0U);                   \
    } while (0)

#define DRAW_CONFLICTS(X, Y, W, H)                                                 \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_CONFLICTS);                        \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_CONFLICTS);                 \
        size_t rows__ = (H) > 4 ? (size_t)((H) - 4) : 1U;                         \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_CONFLICTS, rows__); \
        size_t conflict_ordinal__ = 0U;                                            \
        size_t total__ = dashboard->conflict_visible_count;                        \
        size_t end__ = dashboard->conflict_page_start + rows__;                   \
        if (end__ > total__) end__ = total__;                                      \
        for (size_t visible__ = 0U; visible__ < total__; ++visible__) {             \
            const OdServiceRow *row__ =                                             \
                &dashboard->services[dashboard->conflict_order[visible__]];         \
            if (conflict_ordinal__ < dashboard->conflict_page_start ||             \
                conflict_ordinal__ >= end__) {                                     \
                ++conflict_ordinal__;                                              \
                continue;                                                          \
            }                                                                      \
            char line__[320];                                                      \
            (void)snprintf(line__, sizeof(line__), "! %.96s  %u  %.160s",         \
                           row__->service, (unsigned)row__->preferred_port,         \
                           row__->conflict_detail);                                \
            int line_y__ = (Y) + 2 + (int)(conflict_ordinal__ -                   \
                                             dashboard->conflict_page_start);       \
            bool selected__ = conflict_ordinal__ == dashboard->conflict_selected;  \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED : OD_ROLE_DANGER,         \
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_CONFLICTS, conflict_ordinal__);                      \
            ++conflict_ordinal__;                                                  \
        }                                                                          \
        if (total__ == 0U)                                                         \
            write_centered_in(canvas, (X) + 1, (W) - 2, (Y) + (H) / 2,            \
                              "No conflicts detected", OD_ROLE_SUCCESS, 0U);       \
        char page__[64];                                                           \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu conflict(s)",   \
            total__ == 0U ? 0U : dashboard->conflict_page_start / rows__ + 1U,    \
            total__ == 0U ? 0U : (total__ + rows__ - 1U) / rows__, total__);       \
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_LISTENERS(X, Y, W, H)                                                 \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_LISTENERS);                        \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_LISTENERS);                 \
        size_t capacity__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_LISTENERS,          \
                                          capacity__);                             \
        int listener_inner__ = (W) - 4;                                            \
        const char *listener_columns__ = listener_inner__ >= 72 ?                  \
            " PORT PROTO BIND             PROCESS             PID USER       SOURCE" :\
            " PORT PROTO BIND             PROCESS";                              \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, listener_columns__,              \
                        (size_t)(listener_inner__ > 0 ? listener_inner__ : 0),      \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_COLUMN_HIT(listener_columns__, "PORT", (X) + 2, (Y) + 2,             \
                       OD_WIDGET_LISTENERS, OD_LISTENER_SORT_PORT);                \
        ADD_COLUMN_HIT(listener_columns__, "PROTO", (X) + 2, (Y) + 2,            \
                       OD_WIDGET_LISTENERS, OD_LISTENER_SORT_PROTOCOL);            \
        ADD_COLUMN_HIT(listener_columns__, "BIND", (X) + 2, (Y) + 2,             \
                       OD_WIDGET_LISTENERS, OD_LISTENER_SORT_BIND);                \
        ADD_COLUMN_HIT(listener_columns__, "PROCESS", (X) + 2, (Y) + 2,          \
                       OD_WIDGET_LISTENERS, OD_LISTENER_SORT_PROCESS);             \
        if (listener_inner__ >= 72) {                                              \
            ADD_COLUMN_HIT(listener_columns__, "PID", (X) + 2, (Y) + 2,           \
                           OD_WIDGET_LISTENERS, OD_LISTENER_SORT_PID);             \
            ADD_COLUMN_HIT(listener_columns__, "USER", (X) + 2, (Y) + 2,          \
                           OD_WIDGET_LISTENERS, OD_LISTENER_SORT_USER);            \
            ADD_COLUMN_HIT(listener_columns__, "SOURCE", (X) + 2, (Y) + 2,        \
                           OD_WIDGET_LISTENERS, OD_LISTENER_SORT_SOURCE);          \
        }                                                                          \
        size_t start__ = dashboard->listener_page_start;                          \
        if (start__ >= dashboard->listener_visible_count) start__ = 0U;             \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->listener_visible_count; ++offset__) { \
            const OdEndpoint *endpoint__ =                                         \
                &dashboard->snapshot->endpoints[                                   \
                    dashboard->listener_order[start__ + offset__]];                \
            char line__[384];                                                      \
            if (listener_inner__ >= 72) {                                          \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%5u %-5s %-16.16s %-19.19s %6ld %-10.10s %-8s",             \
                    (unsigned)endpoint__->local_port,                              \
                    endpoint__->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP",      \
                    endpoint__->local_address,                                    \
                    endpoint__->process[0] == '\0' ? "unknown owner" :            \
                                                       endpoint__->process,        \
                    (long)endpoint__->pid, endpoint__->user,                       \
                    endpoint__->permission_limited ? "limited" : "kernel");      \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__), "%5u %-4s %.18s  %.20s",  \
                    (unsigned)endpoint__->local_port,                              \
                    endpoint__->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP",      \
                    endpoint__->local_address,                                    \
                    endpoint__->process[0] == '\0' ? "unknown owner" :            \
                                                       endpoint__->process);       \
            }                                                                      \
            int line_y__ = (Y) + 3 + (int)offset__;                               \
            bool selected__ = start__ + offset__ == dashboard->listener_selected;  \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED :                         \
                                (endpoint__->permission_limited ? OD_ROLE_WARNING :\
                                                                  OD_ROLE_DEFAULT),\
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_LISTENERS, start__ + offset__);                      \
        }                                                                          \
        if (dashboard->listener_visible_count == 0U)                               \
            write_centered_in(canvas, (X) + 1, (W) - 2, (Y) + (H) / 2,            \
                              dashboard->listener_search[0] == '\0' ?              \
                                  "No host listeners found" :                     \
                                  "No listeners match this search",               \
                              OD_ROLE_MUTED, 0U);                                  \
        char page__[64];                                                           \
        size_t total__ = dashboard->listener_visible_count;                        \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu listener(s)",   \
            total__ == 0U ? 0U : start__ / capacity__ + 1U,                       \
            total__ == 0U ? 0U : (total__ + capacity__ - 1U) / capacity__, total__);\
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_DOCKER(X, Y, W, H)                                                    \
    do {                                                                          \
        DRAW_BOX((X), (Y), (W), (H), OD_WIDGET_DOCKER);                           \
        DRAW_SCROLL_CONTROLS((X), (Y), (W), OD_WIDGET_DOCKER);                    \
        size_t capacity__ = (H) > 5 ? (size_t)((H) - 5) : 1U;                     \
        od_dashboard_set_widget_page_size(dashboard, OD_WIDGET_DOCKER, capacity__);\
        int docker_inner__ = (W) - 4;                                              \
        const char *docker_columns__ = docker_inner__ >= 72 ?                      \
            "CONTAINER        HOST PORT  CONTAINER PORT  PROTOCOL PROJECT" :      \
            "CONTAINER          HOST -> CONTAINER/PROTO";                         \
        od_canvas_write(canvas, (X) + 2, (Y) + 2, docker_columns__,                \
                        (size_t)(docker_inner__ > 0 ? docker_inner__ : 0),          \
                        OD_ROLE_MUTED, 1U);                                        \
        ADD_COLUMN_HIT(docker_columns__,                                           \
                       docker_inner__ >= 72 ? "CONTAINER        " :                \
                                                "CONTAINER          ",             \
                       (X) + 2, (Y) + 2, OD_WIDGET_DOCKER,                         \
                       OD_DOCKER_SORT_CONTAINER);                                  \
        ADD_COLUMN_HIT(docker_columns__, "HOST", (X) + 2, (Y) + 2,               \
                       OD_WIDGET_DOCKER, OD_DOCKER_SORT_HOST_PORT);                \
        const char *container_port__ = strstr(docker_columns__ + 1, "CONTAINER"); \
        if (container_port__ != NULL)                                              \
            ADD_HIT((X) + 2 + (int)(container_port__ - docker_columns__),          \
                    (Y) + 2, 9, 1, OD_HIT_SORT_COLUMN, OD_WIDGET_DOCKER,           \
                    OD_DOCKER_SORT_CONTAINER_PORT);                                \
        ADD_COLUMN_HIT(docker_columns__,                                           \
                       docker_inner__ >= 72 ? "PROTOCOL" : "PROTO",              \
                       (X) + 2, (Y) + 2, OD_WIDGET_DOCKER,                         \
                       OD_DOCKER_SORT_PROTOCOL);                                   \
        if (docker_inner__ >= 72)                                                  \
            ADD_COLUMN_HIT(docker_columns__, "PROJECT", (X) + 2, (Y) + 2,         \
                           OD_WIDGET_DOCKER, OD_DOCKER_SORT_PROJECT);              \
        size_t start__ = dashboard->docker_page_start;                            \
        if (start__ >= dashboard->docker_visible_count) start__ = 0U;              \
        for (size_t offset__ = 0U; offset__ < capacity__ &&                        \
             start__ + offset__ < dashboard->docker_visible_count;                 \
             ++offset__) {                                                         \
            const OdDockerMapping *mapping__ =                                     \
                &dashboard->snapshot->docker_mappings[                             \
                    dashboard->docker_order[start__ + offset__]];                  \
            char line__[320];                                                      \
            if (docker_inner__ >= 72) {                                            \
                (void)snprintf(line__, sizeof(line__),                             \
                    "%-16.16s %9u %15u %-8s %-16.16s",                            \
                    mapping__->container, (unsigned)mapping__->host_port,           \
                    (unsigned)mapping__->container_port,                           \
                    mapping__->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp",       \
                    mapping__->project);                                           \
            } else {                                                               \
                (void)snprintf(line__, sizeof(line__), "%-18.18s %5u -> %5u/%s",  \
                    mapping__->container, (unsigned)mapping__->host_port,           \
                    (unsigned)mapping__->container_port,                           \
                    mapping__->protocol == OD_PROTOCOL_UDP ? "udp" : "tcp");      \
            }                                                                      \
            int line_y__ = (Y) + 3 + (int)offset__;                               \
            bool selected__ = start__ + offset__ == dashboard->docker_selected;    \
            od_canvas_write(canvas, (X) + 2, line_y__, line__,                     \
                            (size_t)((W) > 4 ? (W) - 4 : 0),                       \
                            selected__ ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,        \
                            selected__ ? 2U : 0U);                                 \
            ADD_HIT((X) + 1, line_y__, (W) - 2, 1, OD_HIT_SELECT_ROW,              \
                    OD_WIDGET_DOCKER, start__ + offset__);                         \
        }                                                                          \
        if (dashboard->docker_visible_count == 0U)                                 \
            write_centered_in(canvas, (X) + 1, (W) - 2, (Y) + (H) / 2,            \
                              dashboard->docker_search[0] != '\0' ?                \
                                  "No Docker mappings match" :                    \
                                  (dashboard->snapshot->docker_available ?         \
                                      "No published Docker ports" :               \
                                      "Docker unavailable"),                      \
                              OD_ROLE_MUTED, 0U);                                  \
        char page__[64];                                                           \
        size_t total__ = dashboard->docker_visible_count;                          \
        (void)snprintf(page__, sizeof(page__), "Page %zu/%zu  %zu mapping(s)",    \
            total__ == 0U ? 0U : start__ / capacity__ + 1U,                       \
            total__ == 0U ? 0U : (total__ + capacity__ - 1U) / capacity__, total__);\
        od_canvas_write(canvas, (X) + 2, (Y) + (H) - 2, page__,                    \
                        (size_t)((W) > 4 ? (W) - 4 : 0), OD_ROLE_MUTED, 0U);       \
    } while (0)

#define DRAW_WIDGET(WIDGET, X, Y, W, H)                                            \
    do {                                                                          \
        switch (WIDGET) {                                                         \
            case OD_WIDGET_SERVICES: DRAW_SERVICES((X), (Y), (W), (H)); break;    \
            case OD_WIDGET_CONFLICTS: DRAW_CONFLICTS((X), (Y), (W), (H)); break;  \
            case OD_WIDGET_LISTENERS: DRAW_LISTENERS((X), (Y), (W), (H)); break;  \
            case OD_WIDGET_DOCKER: DRAW_DOCKER((X), (Y), (W), (H)); break;        \
            case OD_WIDGET_COUNT: break;                                          \
        }                                                                          \
    } while (0)

    if (dashboard->expanded) {
        DRAW_WIDGET(dashboard->focused, 1, content_y, (int)canvas->width - 2,
                    content_height);
    } else if (content_height >= 18 && canvas->width >= 120U) {
        int available_width = (int)canvas->width - 3;
        int service_width = (available_width * 2) / 3;
        int secondary_x = 2 + service_width;
        int secondary_width = (int)canvas->width - secondary_x - 1;
        int first_height = content_height / 3;
        int second_height = content_height / 3;
        int third_height = content_height - first_height - second_height;
        DRAW_SERVICES(1, content_y, service_width, content_height);
        DRAW_CONFLICTS(secondary_x, content_y, secondary_width, first_height);
        DRAW_LISTENERS(secondary_x, content_y + first_height,
                       secondary_width, second_height);
        DRAW_DOCKER(secondary_x, content_y + first_height + second_height,
                    secondary_width, third_height);
    } else if (content_height >= 16 && canvas->width >= 80U) {
        int primary_height = (content_height * 2) / 3;
        int secondary_height = content_height - primary_height;
        OdDashboardWidget secondary = dashboard->focused == OD_WIDGET_SERVICES ?
            OD_WIDGET_CONFLICTS : dashboard->focused;
        DRAW_SERVICES(1, content_y, (int)canvas->width - 2, primary_height);
        DRAW_WIDGET(secondary, 1, content_y + primary_height,
                    (int)canvas->width - 2, secondary_height);
    } else {
        int tab_x = 1;
        for (size_t offset = 0U; offset < OD_WIDGET_COUNT; ++offset) {
            OdDashboardWidget widget = (OdDashboardWidget)(
                ((size_t)dashboard->focused + offset) % OD_WIDGET_COUNT);
            char tab[48];
            (void)snprintf(tab, sizeof(tab), offset == 0U ? "[%s]" : "%s",
                           widget_names[(size_t)widget]);
            int tab_width = (int)strlen(tab);
            if (tab_x + tab_width >= (int)canvas->width - 1) break;
            od_canvas_write(canvas, tab_x, 3, tab, (size_t)tab_width,
                            offset == 0U ? OD_ROLE_PRIMARY : OD_ROLE_MUTED,
                            offset == 0U ? 1U : 0U);
            ADD_HIT(tab_x, 3, tab_width, 1, OD_HIT_FOCUS_WIDGET, widget, 0U);
            tab_x += tab_width + 2;
        }
        DRAW_WIDGET(dashboard->focused, 1, content_y,
                    (int)canvas->width - 2, content_height);
    }

#undef DRAW_WIDGET
#undef DRAW_DOCKER
#undef DRAW_LISTENERS
#undef DRAW_CONFLICTS
#undef DRAW_SERVICES
#undef DRAW_BOX
#undef DRAW_SCROLL_CONTROLS
#undef DRAW_TITLE
#undef ADD_COLUMN_HIT
#undef ADD_HIT
}

static const OdServiceRow *selected_conflict(const OdDashboard *dashboard) {
    if (dashboard->conflict_selected >= dashboard->conflict_visible_count) return NULL;
    return &dashboard->services[dashboard->conflict_order[dashboard->conflict_selected]];
}

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    bool failed;
} OdDetailText;

static void append_detail(OdDetailText *detail,
                          const char *label,
                          const char *value) {
    if (detail->failed) return;
    const char *display = value == NULL || value[0] == '\0' ? "—" : value;
    int count = snprintf(NULL, 0, "%s: %s\n", label, display);
    if (count < 0) {
        detail->failed = true;
        return;
    }
    size_t required = detail->length + (size_t)count + 1U;
    if (required > detail->capacity) {
        size_t capacity = detail->capacity == 0U ? 256U : detail->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2U) {
                detail->failed = true;
                return;
            }
            capacity *= 2U;
        }
        char *grown = realloc(detail->text, capacity);
        if (grown == NULL) {
            detail->failed = true;
            return;
        }
        detail->text = grown;
        detail->capacity = capacity;
    }
    (void)snprintf(detail->text + detail->length,
                   detail->capacity - detail->length,
                   "%s: %s\n", label, display);
    detail->length += (size_t)count;
}

static void append_detail_number(OdDetailText *detail,
                                 const char *label,
                                 unsigned long long value) {
    char number[48];
    (void)snprintf(number, sizeof(number), "%llu", value);
    append_detail(detail, label, number);
}

static char *finish_detail(OdDetailText *detail) {
    if (detail->failed) {
        free(detail->text);
        return NULL;
    }
    if (detail->text == NULL) detail->text = calloc(1U, 1U);
    return detail->text;
}

static size_t detail_chunk(const char *text, size_t length, size_t width) {
    size_t chunk = length < width ? length : width;
    if (chunk == length) return chunk;
    while (chunk > 0U && (((unsigned char)text[chunk] & 0xc0U) == 0x80U)) --chunk;
    return chunk == 0U ? (length < width ? length : width) : chunk;
}

static size_t wrapped_detail_rows(const char *text, size_t width) {
    size_t rows = 0U;
    const char *cursor = text;
    while (*cursor != '\0') {
        const char *newline = strchr(cursor, '\n');
        size_t length = newline == NULL ? strlen(cursor) : (size_t)(newline - cursor);
        if (length == 0U) {
            ++rows;
        } else {
            size_t consumed = 0U;
            while (consumed < length) {
                consumed += detail_chunk(cursor + consumed, length - consumed, width);
                ++rows;
            }
        }
        if (newline == NULL) break;
        cursor = newline + 1;
    }
    return rows;
}

static char *build_dashboard_detail(const OdDashboard *dashboard,
                                    char *title,
                                    size_t title_capacity) {
    OdDetailText detail = {0};
    if (dashboard->focused == OD_WIDGET_SERVICES ||
        dashboard->focused == OD_WIDGET_CONFLICTS) {
        const OdServiceRow *row = dashboard->focused == OD_WIDGET_SERVICES ?
            od_dashboard_selected_service(dashboard) : selected_conflict(dashboard);
        (void)snprintf(title, title_capacity, "%s",
                       dashboard->focused == OD_WIDGET_SERVICES ?
                           "Service details" : "Conflict details");
        if (row == NULL) {
            append_detail(&detail, "Selection", "No row selected");
            return finish_detail(&detail);
        }
        append_detail(&detail, "Stable ID", row->stable_id);
        append_detail(&detail, "Service", row->service);
        append_detail(&detail, "Group", row->group);
        append_detail(&detail, "Variable", row->variable);
        append_detail_number(&detail, "Preferred port", row->preferred_port);
        append_detail_number(&detail, "Selected port", row->selected_port);
        append_detail(&detail, "Status",
                      od_service_status_name(row->status));
        append_detail(&detail, "Conflict",
                      row->conflict ? row->conflict_detail : "None");
        return finish_detail(&detail);
    }
    if (dashboard->focused == OD_WIDGET_LISTENERS) {
        (void)snprintf(title, title_capacity, "Listener details");
        if (dashboard->listener_selected >= dashboard->listener_visible_count) {
            append_detail(&detail, "Selection", "No row selected");
            return finish_detail(&detail);
        }
        const OdEndpoint *endpoint =
            &dashboard->snapshot->endpoints[
                dashboard->listener_order[dashboard->listener_selected]];
        append_detail(&detail, "Stable ID", endpoint->stable_id);
        append_detail(&detail, "Protocol",
                      endpoint->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP");
        append_detail(&detail, "Local address", endpoint->local_address);
        append_detail_number(&detail, "Local port", endpoint->local_port);
        append_detail(&detail, "Remote address", endpoint->remote_address);
        append_detail_number(&detail, "Remote port", endpoint->remote_port);
        append_detail(&detail, "Process", endpoint->process);
        append_detail_number(&detail, "PID",
                             (unsigned long long)endpoint->pid);
        append_detail(&detail, "User", endpoint->user);
        append_detail_number(&detail, "UID",
                             (unsigned long long)endpoint->uid);
        append_detail_number(&detail, "Socket inode", endpoint->inode);
        append_detail(&detail, "Executable", endpoint->executable);
        append_detail(&detail, "Command", endpoint->command);
        append_detail(&detail, "Ownership",
                      endpoint->permission_limited ?
                          "Process details limited by permissions" : "Resolved");
        return finish_detail(&detail);
    }
    (void)snprintf(title, title_capacity, "Docker mapping details");
    if (dashboard->docker_selected >= dashboard->docker_visible_count) {
        append_detail(&detail, "Selection", "No row selected");
        return finish_detail(&detail);
    }
    const OdDockerMapping *mapping =
        &dashboard->snapshot->docker_mappings[
            dashboard->docker_order[dashboard->docker_selected]];
    append_detail(&detail, "Container", mapping->container);
    append_detail(&detail, "Container ID", mapping->container_id);
    append_detail(&detail, "Compose project", mapping->project);
    append_detail(&detail, "Compose service", mapping->service);
    append_detail(&detail, "Bind address", mapping->bind_address);
    append_detail_number(&detail, "Host port", mapping->host_port);
    append_detail_number(&detail, "Container port",
                         mapping->container_port);
    append_detail(&detail, "Protocol",
                  mapping->protocol == OD_PROTOCOL_UDP ? "UDP" : "TCP");
    return finish_detail(&detail);
}

static size_t render_detail_document(OdCanvas *canvas,
                                     const char *title,
                                     const char *detail_text,
                                     size_t page,
                                     bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return 1U;
    }
    od_canvas_write(canvas, 2, 1, title, canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    od_canvas_write(canvas, 2, 2,
                    "Full selected-row values • content wraps inside this viewport",
                    canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 7;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    size_t line_width = (size_t)(box_width - 4);
    size_t page_size = box_height > 3 ? (size_t)(box_height - 3) : 1U;
    size_t row_count = wrapped_detail_rows(detail_text, line_width);
    size_t page_count = row_count == 0U ? 1U : (row_count + page_size - 1U) / page_size;
    size_t current_page = page < page_count ? page : page_count - 1U;
    size_t first_row = current_page * page_size;
    size_t final_row = first_row + page_size;
    char *line = malloc(line_width + 1U);
    if (line != NULL) {
        size_t row = 0U;
        const char *cursor = detail_text;
        while (*cursor != '\0' && row < final_row) {
            const char *newline = strchr(cursor, '\n');
            size_t length = newline == NULL ? strlen(cursor) : (size_t)(newline - cursor);
            size_t consumed = 0U;
            do {
                size_t chunk = length == 0U ? 0U :
                    detail_chunk(cursor + consumed, length - consumed, line_width);
                if (row >= first_row && row < final_row) {
                    if (chunk > 0U) memcpy(line, cursor + consumed, chunk);
                    line[chunk] = '\0';
                    od_canvas_write(canvas, box_x + 2,
                                    box_y + 1 + (int)(row - first_row), line,
                                    line_width, OD_ROLE_DEFAULT, 0U);
                }
                consumed += chunk;
                ++row;
            } while (consumed < length && row < final_row);
            if (newline == NULL) break;
            cursor = newline + 1;
        }
        free(line);
    }
    char page_text[96];
    (void)snprintf(page_text, sizeof(page_text), "Page %zu/%zu  %zu wrapped line(s)",
                   current_page + 1U, page_count, row_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page_text,
                    line_width, OD_ROLE_MUTED, 0U);
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "PgUp/PgDn Page  Up/Down Page  Home/End  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
    return page_count;
}

size_t od_render_dashboard_detail(OdCanvas *canvas,
                                  const OdDashboard *dashboard,
                                  size_t page,
                                  bool ascii) {
    char title[96];
    char *detail = build_dashboard_detail(dashboard, title, sizeof(title));
    const char *detail_text = detail == NULL ?
        "Detail content could not be allocated.\n" : detail;
    size_t page_count = render_detail_document(canvas, title, detail_text, page, ascii);
    free(detail);
    return page_count;
}

size_t od_render_candidate_detail(OdCanvas *canvas,
                                  const OdCandidate *candidate,
                                  size_t page,
                                  bool ascii) {
    OdDetailText detail = {0};
    if (candidate == NULL) {
        append_detail(&detail, "Selection", "No candidate selected");
    } else {
        const char *confidence = candidate->confidence == OD_CONFIDENCE_CONFIRMED ?
            "Confirmed" : (candidate->confidence == OD_CONFIDENCE_LIKELY ?
                                "Likely" : "Possible");
        const char *protocols = candidate->protocols ==
                                    (OD_PROTOCOL_TCP | OD_PROTOCOL_UDP) ?
                                    "TCP, UDP" :
                                (candidate->protocols == OD_PROTOCOL_UDP ? "UDP" : "TCP");
        append_detail(&detail, "Stable ID", candidate->stable_id);
        append_detail(&detail, "Name", candidate->name);
        append_detail(&detail, "Confidence", confidence);
        append_detail(&detail, "Group", candidate->group);
        append_detail(&detail, "Variable", candidate->variable);
        append_detail_number(&detail, "Port", candidate->port);
        append_detail(&detail, "Protocols", protocols);
        append_detail(&detail, "Selected for management",
                      candidate->selected ? "Yes" : "No");
        for (size_t index = 0U; index < candidate->sources.count; ++index) {
            char label[48];
            (void)snprintf(label, sizeof(label), "Source %zu", index + 1U);
            append_detail(&detail, label, candidate->sources.items[index]);
        }
    }
    char *text = finish_detail(&detail);
    const char *display = text == NULL ?
        "Detail content could not be allocated.\n" : text;
    size_t page_count = render_detail_document(canvas, "Discovery candidate details",
                                               display, page, ascii);
    free(text);
    return page_count;
}

void od_render_profile_editor(OdCanvas *canvas,
                              const OdProfileView *view,
                              bool ascii) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Edit project profile", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    char paths[640];
    (void)snprintf(paths, sizeof(paths), "Profile: %s  •  Assignments: %s",
                   view->profile_path == NULL ? "unavailable" : view->profile_path,
                   view->assignment_path == NULL ? "unavailable" : view->assignment_path);
    od_canvas_write(canvas, 2, 2, paths, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 6;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "SERVICE               GROUP          VARIABLE             PORT PROTOCOL",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);
    size_t rows = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    size_t selected = view->selected_service;
    if (view->profile->service_count == 0U) selected = 0U;
    if (selected >= view->profile->service_count && view->profile->service_count > 0U) {
        selected = view->profile->service_count - 1U;
    }
    size_t page_start = (selected / rows) * rows;
    size_t end = page_start + rows;
    if (end > view->profile->service_count) end = view->profile->service_count;
    for (size_t index = page_start; index < end; ++index) {
        const OdService *service = &view->profile->services[index];
        const char *protocol = service->protocols == (OD_PROTOCOL_TCP | OD_PROTOCOL_UDP) ?
            "tcp,udp" : (service->protocols == OD_PROTOCOL_UDP ? "udp" : "tcp");
        char row[384];
        (void)snprintf(row, sizeof(row), "%-21.21s %-14.14s %-20.20s %5u %-7s",
                       service->name, service->group, service->variable,
                       (unsigned)service->preferred_port, protocol);
        bool is_selected = index == selected;
        od_canvas_write(canvas, box_x + 2,
                        box_y + 2 + (int)(index - page_start), row,
                        (size_t)(box_width - 4),
                        is_selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        is_selected ? 2U : 0U);
    }
    if (view->profile->service_count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No managed services • press a to add one",
                                 OD_ROLE_MUTED, 0U);
    }
    char page_text[96];
    size_t pages = view->profile->service_count == 0U ? 1U :
        (view->profile->service_count + rows - 1U) / rows;
    (void)snprintf(page_text, sizeof(page_text), "Page %zu/%zu  %zu service(s)",
                   view->profile->service_count == 0U ? 1U : page_start / rows + 1U,
                   pages, view->profile->service_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page_text,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (view->status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, view->status,
                        canvas->width - 4U, OD_ROLE_WARNING, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "x Reset assignments  e Edit  a Add  s Save  Up/Down/Pg Page  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_conflict_resolution(OdCanvas *canvas,
                                   const OdResolution *resolution,
                                   bool ascii,
                                   const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Resolve conflict", canvas->width - 4U,
                    OD_ROLE_PRIMARY, 1U);
    const OdResolutionItem *item = od_resolution_current(resolution);
    if (item == NULL) {
        od_canvas_write_centered(canvas, (int)canvas->height / 2,
                                 "Every conflict has been reviewed",
                                 OD_ROLE_SUCCESS, 1U);
    } else {
        char progress[96];
        (void)snprintf(progress, sizeof(progress), "Conflict %zu of %zu",
                       resolution->current + 1U, resolution->count);
        od_canvas_write(canvas, 2, 2, progress, canvas->width - 4U,
                        OD_ROLE_MUTED, 0U);
        int box_x = 2;
        int box_y = 4;
        int box_width = (int)canvas->width - 4;
        int box_height = (int)canvas->height - 9;
        od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                      OD_ROLE_FOCUSED_BORDER);
        const OdAllocation *allocation =
            &resolution->plan->items[item->allocation_index];
        char line[320];
        (void)snprintf(line, sizeof(line), "Service: %s", item->service);
        od_canvas_write(canvas, box_x + 2, box_y + 2, line,
                        (size_t)(box_width - 4), OD_ROLE_DEFAULT, 1U);
        (void)snprintf(line, sizeof(line), "Requested port: %u",
                       (unsigned)item->original_port);
        od_canvas_write(canvas, box_x + 2, box_y + 4, line,
                        (size_t)(box_width - 4), OD_ROLE_WARNING, 0U);
        (void)snprintf(line, sizeof(line), "Occupied by: %s", item->owner);
        od_canvas_write(canvas, box_x + 2, box_y + 5, line,
                        (size_t)(box_width - 4), OD_ROLE_DANGER, 0U);
        (void)snprintf(line, sizeof(line), "Recommended free port: %u",
                       (unsigned)allocation->new_port);
        od_canvas_write(canvas, box_x + 2, box_y + 7, line,
                        (size_t)(box_width - 4), OD_ROLE_SUCCESS, 1U);
    }
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Accept Enter] [Edit e] [Skip s] [Cancel Esc]",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

static const OdService *screen_find_service(const OdProfile *profile, const char *id) {
    for (size_t index = 0U; index < profile->service_count; ++index) {
        if (strcmp(profile->services[index].id, id) == 0) return &profile->services[index];
    }
    return NULL;
}

void od_render_change_review(OdCanvas *canvas,
                             const OdProfile *profile,
                             const OdAllocationPlan *plan,
                             size_t selected,
                             bool ascii,
                             const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Review assignment changes",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char summary[128];
    (void)snprintf(summary, sizeof(summary),
                   "%zu managed service(s) • no files change until you confirm",
                   plan->count);
    od_canvas_write(canvas, 2, 2, summary, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "SERVICE                    OLD     NEW  REASON",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);
    size_t page_size = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    if (plan->count > 0U && selected >= plan->count) selected = plan->count - 1U;
    size_t page_start = (selected / page_size) * page_size;
    size_t end = page_start + page_size;
    if (end > plan->count) end = plan->count;
    for (size_t index = page_start; index < end; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        const OdService *service = screen_find_service(profile, allocation->service_id);
        const char *reason = allocation->new_port == 0U ? "SKIPPED" :
            (allocation->reason == OD_ALLOC_PRESERVED ? "SAVED" :
             (allocation->reason == OD_ALLOC_REASSIGNED ? "REASSIGNED" : "PREFERRED"));
        char row[320];
        char old_port[16];
        char new_port[16];
        (void)snprintf(old_port, sizeof(old_port), allocation->old_port == 0U ? "-" : "%u",
                       (unsigned)allocation->old_port);
        (void)snprintf(new_port, sizeof(new_port), allocation->new_port == 0U ? "-" : "%u",
                       (unsigned)allocation->new_port);
        (void)snprintf(row, sizeof(row), "%-26.26s %7s %7s  %-10.10s",
                       service == NULL ? allocation->service_id : service->name,
                       old_port, new_port, reason);
        bool is_selected = index == selected;
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)(index - page_start),
                        row, (size_t)(box_width - 4),
                        is_selected ? OD_ROLE_SELECTED :
                            (allocation->new_port == 0U ? OD_ROLE_WARNING : OD_ROLE_DEFAULT),
                        is_selected ? 2U : 0U);
    }
    char page[80];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu",
                   plan->count == 0U ? 0U : page_start / page_size + 1U,
                   plan->count == 0U ? 0U : (plan->count + page_size - 1U) / page_size);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 3, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Save Enter] [Cancel Esc]  Up/Down Select  PgUp/PgDn Page",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_settings(OdCanvas *canvas, const OdSettingsView *view, bool ascii) {
    static const char *const unicode_names[] = {"auto", "always", "never"};
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Settings and appearance",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char path[320];
    (void)snprintf(path, sizeof(path), "Saved globally: %s",
                   view->settings_path == NULL ? "path unavailable" : view->settings_path);
    od_canvas_write(canvas, 2, 2, path, canvas->width - 4U, OD_ROLE_MUTED, 0U);

    int box_x = 2;
    int box_y = 4;
    int box_width = (int)canvas->width - 4;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    char rows[6][160];
    (void)snprintf(rows[0], sizeof(rows[0]), "Theme                 < %s >",
                   view->settings->theme);
    (void)snprintf(rows[1], sizeof(rows[1]), "Unicode               < %s >",
                   unicode_names[view->settings->unicode_mode]);
    (void)snprintf(rows[2], sizeof(rows[2]), "Reduce motion           [%s]",
                   view->settings->reduced_motion ? "on" : "off");
    (void)snprintf(rows[3], sizeof(rows[3]), "Mouse input             [%s]",
                   view->settings->mouse ? "on" : "off");
    (void)snprintf(rows[4], sizeof(rows[4]), "Refresh automatically   [%s]",
                   view->settings->auto_refresh ? "on" : "off");
    (void)snprintf(rows[5], sizeof(rows[5]), "Refresh interval       < %u seconds >",
                   view->settings->refresh_seconds);
    for (size_t index = 0U; index < 6U; ++index) {
        bool selected = index == view->selected_item;
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)index, rows[index],
                        (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 2U : 0U);
    }
    if (view->status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, view->status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "[Save Enter] [Cancel Esc]  Up/Down  Left/Right  Space Toggle",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}

void od_render_help(OdCanvas *canvas, OdHelp *help, bool ascii, const char *status) {
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    if (canvas->width < 60U || canvas->height < 18U) {
        od_render_resize_required(canvas);
        return;
    }
    od_canvas_write(canvas, 2, 1, "Keyboard and workflow help",
                    canvas->width - 4U, OD_ROLE_PRIMARY, 1U);
    char search[180];
    (void)snprintf(search, sizeof(search), "Search: %s%s",
                   help->query[0] == '\0' ? "all topics" : help->query,
                   help->query[0] == '\0' ? "" : "  (press / to change)");
    od_canvas_write(canvas, 2, 2, search, canvas->width - 4U, OD_ROLE_MUTED, 0U);
    int box_x = 1;
    int box_y = 4;
    int box_width = (int)canvas->width - 2;
    int box_height = (int)canvas->height - 8;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    size_t rows = box_height > 4 ? (size_t)(box_height - 4) : 1U;
    od_help_set_page_size(help, rows);
    size_t end = help->page_start + rows;
    if (end > help->visible_count) end = help->visible_count;
    for (size_t index = help->page_start; index < end; ++index) {
        char line[384];
        (void)snprintf(line, sizeof(line), "%-18.18s  %s",
                       od_help_key(help, index), od_help_description(help, index));
        bool selected = index == help->selected;
        od_canvas_write(canvas, box_x + 2,
                        box_y + 2 + (int)(index - help->page_start),
                        line, (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 2U : 0U);
    }
    if (help->visible_count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No help topics match • press / to change the search",
                                 OD_ROLE_MUTED, 0U);
    }
    char page[96];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu  %zu topic(s)",
                   help->visible_count == 0U ? 0U : help->page_start / rows + 1U,
                   help->visible_count == 0U ? 0U :
                       (help->visible_count + rows - 1U) / rows,
                   help->visible_count);
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 2, status,
                        canvas->width - 4U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Select  PgUp/PgDn Page  / Search  Home/End  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}
