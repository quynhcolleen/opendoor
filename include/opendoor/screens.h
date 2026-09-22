#ifndef OPENDOOR_SCREENS_H
#define OPENDOOR_SCREENS_H

#include "opendoor/ui.h"
#include "opendoor/onboarding.h"
#include "opendoor/dashboard.h"
#include "opendoor/resolution.h"
#include "opendoor/help.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OD_LOAD_PROJECT_FILES,
    OD_LOAD_KERNEL_SOCKETS,
    OD_LOAD_PROCESS_OWNERS,
    OD_LOAD_DOCKER,
    OD_LOAD_RECONCILIATION,
    OD_LOAD_STAGE_COUNT
} OdLoadingStage;

typedef struct {
    const char *project_name;
    bool configured;
    size_t selected_item;
    const char *status;
} OdMenuView;

typedef struct {
    int box_x;
    int box_y;
    int box_width;
    int box_height;
    int first_item_y;
    int row_stride;
    size_t visible_count;
    size_t page_start;
} OdMenuLayout;

typedef struct {
    const OdSettings *settings;
    size_t selected_item;
    const char *settings_path;
    const char *status;
} OdSettingsView;

typedef struct {
    const OdProfile *profile;
    size_t selected_service;
    const char *profile_path;
    const char *assignment_path;
    const char *status;
} OdProfileView;

void od_render_resize_required(OdCanvas *canvas);
void od_render_loading(OdCanvas *canvas,
                       OdLoadingStage stage,
                       unsigned spinner_frame,
                       unsigned elapsed_milliseconds,
                       bool reduced_motion,
                       bool ascii,
                       size_t warning_count);
void od_render_main_menu(OdCanvas *canvas, const OdMenuView *view, bool ascii);
void od_main_menu_layout(size_t viewport_width,
                         size_t viewport_height,
                         size_t item_count,
                         size_t selected,
                         bool ascii,
                         OdMenuLayout *layout);
size_t od_menu_page_size(size_t viewport_width,
                         size_t viewport_height,
                         size_t item_count,
                         bool ascii);
size_t od_menu_page_start(size_t selected, size_t page_size);
size_t od_onboarding_row_height_for_viewport(const OdOnboarding *onboarding,
                                              size_t viewport_width,
                                              size_t viewport_height);
size_t od_onboarding_page_size_for_viewport(const OdOnboarding *onboarding,
                                             size_t viewport_width,
                                             size_t viewport_height);
void od_render_onboarding(OdCanvas *canvas,
                          const OdOnboarding *onboarding,
                          bool ascii,
                          const char *status);
size_t od_render_candidate_detail(OdCanvas *canvas,
                                  const OdCandidate *candidate,
                                  size_t page,
                                  bool ascii);
void od_render_dashboard(OdCanvas *canvas,
                         OdDashboard *dashboard,
                         bool ascii,
                         OdHitMap *hit_map,
                         const char *status);
size_t od_render_dashboard_detail(OdCanvas *canvas,
                                  const OdDashboard *dashboard,
                                  size_t page,
                                  bool ascii);
void od_render_profile_editor(OdCanvas *canvas,
                              const OdProfileView *view,
                              bool ascii);
void od_render_conflict_resolution(OdCanvas *canvas,
                                   const OdResolution *resolution,
                                   bool ascii,
                                   const char *status);
void od_render_change_review(OdCanvas *canvas,
                             const OdProfile *profile,
                             const OdAllocationPlan *plan,
                             size_t selected,
                             bool ascii,
                             const char *status);
void od_render_settings(OdCanvas *canvas, const OdSettingsView *view, bool ascii);
void od_render_help(OdCanvas *canvas, OdHelp *help, bool ascii, const char *status);
const char *const *od_banner_lines(void);
size_t od_banner_line_count(void);

#endif
