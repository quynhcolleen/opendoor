#include "opendoor/screens.h"

#include <stdio.h>
#include <string.h>

static const char *const banner[] = {
    "   ____  ____  _______   __   ____  ____  ____  ____",
    "  / __ \\/ __ \\/ ____/ | / /  / __ \\/ __ \\/ __ \\/ __ \\",
    " / / / / /_/ / __/ /  |/ /  / / / / / / / / / / /_/ /",
    "/ /_/ / ____/ /___/ /|  /  / /_/ / /_/ / /_/ / _, _/",
    "\\____/_/   /_____/_/ |_/   \\____/_____/\\____/_/ |_|  OPEN DOOR"
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
    for (size_t index = 0U; index < od_banner_line_count(); ++index) {
        od_canvas_write_centered(canvas, 1 + (int)index, banner[index],
                                 OD_ROLE_PRIMARY, 1U);
    }
    char project[256];
    (void)snprintf(project, sizeof(project), "Project: %s  •  %s",
                   view->project_name == NULL ? "current directory" : view->project_name,
                   view->configured ? "profile configured" : "new project");
    od_canvas_write_centered(canvas, 7, project, OD_ROLE_MUTED, 0U);

    const char *const *items = view->configured ? configured : first_run;
    size_t item_count = view->configured ? sizeof(configured) / sizeof(configured[0]) :
                                          sizeof(first_run) / sizeof(first_run[0]);
    int box_width = 50;
    if ((size_t)box_width > canvas->width - 4U) box_width = (int)canvas->width - 4;
    int box_height = (int)item_count + 4;
    int box_x = ((int)canvas->width - box_width) / 2;
    int box_y = 9;
    od_canvas_box(canvas, box_x, box_y, box_width, box_height, ascii,
                  OD_ROLE_FOCUSED_BORDER);
    for (size_t index = 0U; index < item_count; ++index) {
        char line[96];
        bool selected = index == view->selected_item;
        (void)snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", items[index]);
        od_canvas_write(canvas, box_x + 2, box_y + 2 + (int)index, line,
                        (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    if (view->status != NULL) {
        od_canvas_write(canvas, 1, (int)canvas->height - 2, view->status,
                        canvas->width - 2U, OD_ROLE_MUTED, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Navigate  Enter Select  ? Help  q Quit",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
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
    od_canvas_write(canvas, box_x + 2, box_y + 1,
                    "USE REVIEW CONFIDENCE   NAME                 VARIABLE                 PORT",
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 1U);

    size_t start = od_onboarding_page_start(onboarding);
    size_t end = start + onboarding->page_size;
    if (end > onboarding->candidates.count) end = onboarding->candidates.count;
    for (size_t index = start; index < end; ++index) {
        const OdCandidate *candidate = &onboarding->candidates.items[index];
        const char *confidence = candidate->confidence == OD_CONFIDENCE_CONFIRMED ? "Confirmed" :
                                 (candidate->confidence == OD_CONFIDENCE_LIKELY ? "Likely" : "Possible");
        const char *use = candidate->selected ? (ascii ? "[x]" : "[✓]") : "[ ]";
        const char *review = onboarding->reviewed[index] ? (ascii ? "yes" : "✓") : "—";
        char row[320];
        (void)snprintf(row, sizeof(row), "%-3s %-6s %-12s %-20.20s %-24.24s %5u",
                       use, review, confidence, candidate->name, candidate->variable,
                       (unsigned)candidate->port);
        int row_y = box_y + 2 + (int)(index - start);
        bool selected = index == onboarding->selected;
        od_canvas_write(canvas, box_x + 2, row_y, row, (size_t)(box_width - 4),
                        selected ? OD_ROLE_SELECTED : OD_ROLE_DEFAULT,
                        selected ? 1U : 0U);
    }
    if (onboarding->candidates.count == 0U) {
        od_canvas_write_centered(canvas, box_y + box_height / 2,
                                 "No candidates found • press a to add a service",
                                 OD_ROLE_MUTED, 0U);
    }
    char page[80];
    (void)snprintf(page, sizeof(page), "Page %zu/%zu",
                   od_onboarding_page(onboarding) + 1U,
                   od_onboarding_page_count(onboarding));
    od_canvas_write(canvas, box_x + 2, box_y + box_height - 2, page,
                    (size_t)(box_width - 4), OD_ROLE_MUTED, 0U);
    if (status != NULL) {
        od_canvas_write(canvas, 2, (int)canvas->height - 3, status,
                        canvas->width - 4U, OD_ROLE_WARNING, 0U);
    }
    od_canvas_write(canvas, 1, (int)canvas->height - 1,
                    "Up/Down Select  PgUp/PgDn Page  Space Use  Enter Review  e Edit  a Add  s Continue  Esc Back",
                    canvas->width - 2U, OD_ROLE_MUTED, 0U);
}
