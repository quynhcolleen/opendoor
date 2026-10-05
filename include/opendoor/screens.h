#ifndef OPENDOOR_SCREENS_H
#define OPENDOOR_SCREENS_H

#include "opendoor/dashboard.h"
#include "opendoor/history.h"
#include "opendoor/resolution.h"
#include "opendoor/ui.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OD_MENU_DASHBOARD = 0,
    OD_MENU_RESOLVE_CONFLICTS,
    OD_MENU_HISTORY,
    OD_MENU_QUIT,
    OD_MENU_COUNT
} OdMenuItem;

typedef enum {
    OD_MOUSE_NONE = 0,
    OD_MOUSE_MENU_ITEM,
    OD_MOUSE_REFRESH,
    OD_MOUSE_BACK,
    OD_MOUSE_APPLY_ALL,
    OD_MOUSE_HISTORY_ROW,
    OD_MOUSE_HISTORY_REVERT,
    OD_MOUSE_HISTORY_CONFIRM
} OdMouseAction;

typedef struct {
    OdMouseAction action;
    size_t item;
} OdMouseTarget;

OdMenuItem od_menu_dispatch(size_t selected);
OdMouseTarget od_menu_mouse_target(size_t width,
                                   size_t height,
                                   bool ascii,
                                   int x,
                                   int y);
OdMouseTarget od_dashboard_mouse_target(size_t width,
                                        size_t height,
                                        int x,
                                        int y);
OdMouseTarget od_conflicts_mouse_target(size_t width,
                                        size_t height,
                                        bool apply_available,
                                        int x,
                                        int y);
OdMouseTarget od_history_mouse_target(size_t width,
                                      size_t height,
                                      const OdHistory *history,
                                      size_t selected,
                                      size_t scroll,
                                      bool confirming,
                                      int x,
                                      int y);
void od_render_resize_required(OdCanvas *canvas);
void od_render_main_menu(OdCanvas *canvas,
                         size_t selected,
                         const char *project,
                         const char *status,
                         bool ascii);
void od_render_dashboard(OdCanvas *canvas,
                         const OdDashboard *dashboard,
                         const char *status,
                         bool ascii);
size_t od_resolution_visual_line_count(const OdResolution *resolution);
void od_render_conflicts(OdCanvas *canvas,
                         const OdResolution *resolution,
                         size_t scroll,
                         bool apply_available,
                         bool prepared,
                         const char *status,
                         bool ascii);
size_t od_history_page_size(size_t height);
size_t od_history_visible_scroll(size_t count, size_t selected,
                                 size_t scroll, size_t height);
/* status is general outcome feedback. Backend refusal/refresh messages belong
 * in error_detail, which is rendered only on the bottom detail line. */
void od_render_history(OdCanvas *canvas,
                        const OdHistory *history,
                        size_t selected,
                        size_t scroll,
                        const char *status,
                        const char *error_detail,
                        bool ascii);
void od_render_history_confirmation(OdCanvas *canvas,
                                     const OdHistoryRecord *record,
                                     bool ascii);

#endif
