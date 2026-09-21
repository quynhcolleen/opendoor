#ifndef OPENDOOR_SCREENS_H
#define OPENDOOR_SCREENS_H

#include "opendoor/ui.h"
#include "opendoor/onboarding.h"
#include "opendoor/dashboard.h"
#include "opendoor/resolution.h"

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

void od_render_resize_required(OdCanvas *canvas);
void od_render_loading(OdCanvas *canvas,
                       OdLoadingStage stage,
                       unsigned spinner_frame,
                       unsigned elapsed_milliseconds,
                       bool reduced_motion,
                       bool ascii,
                       size_t warning_count);
void od_render_main_menu(OdCanvas *canvas, const OdMenuView *view, bool ascii);
void od_render_onboarding(OdCanvas *canvas,
                          const OdOnboarding *onboarding,
                          bool ascii,
                          const char *status);
void od_render_dashboard(OdCanvas *canvas,
                         OdDashboard *dashboard,
                         bool ascii,
                         OdHitMap *hit_map,
                         const char *status);
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
const char *const *od_banner_lines(void);
size_t od_banner_line_count(void);

#endif
