#ifndef OPENDOOR_ONBOARDING_H
#define OPENDOOR_ONBOARDING_H

#include "opendoor/discovery.h"
#include "opendoor/model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    OdCandidateList candidates;
    bool *reviewed;
    size_t selected;
    size_t page_size;
    uint16_t port_min;
    uint16_t port_max;
} OdOnboarding;

OdStatus od_onboarding_init(OdOnboarding *onboarding,
                            const OdCandidateList *candidates,
                            size_t page_size,
                            uint16_t port_min,
                            uint16_t port_max,
                            OdError *error);
void od_onboarding_free(OdOnboarding *onboarding);
size_t od_onboarding_page(const OdOnboarding *onboarding);
size_t od_onboarding_page_count(const OdOnboarding *onboarding);
size_t od_onboarding_page_start(const OdOnboarding *onboarding);
void od_onboarding_move(OdOnboarding *onboarding, int rows);
void od_onboarding_move_page(OdOnboarding *onboarding, int pages);
void od_onboarding_toggle_selected(OdOnboarding *onboarding);
void od_onboarding_review_selected(OdOnboarding *onboarding);
bool od_onboarding_all_reviewed(const OdOnboarding *onboarding);
OdStatus od_onboarding_edit_selected(OdOnboarding *onboarding,
                                     const char *name,
                                     const char *group,
                                     const char *variable,
                                     uint16_t port,
                                     unsigned protocols,
                                     OdError *error);
OdStatus od_onboarding_add_manual(OdOnboarding *onboarding,
                                  const char *name,
                                  const char *group,
                                  const char *variable,
                                  uint16_t port,
                                  unsigned protocols,
                                  OdError *error);
OdStatus od_onboarding_build_profile(const OdOnboarding *onboarding,
                                     const char *project_name,
                                     const char *assignment_file,
                                     OdProfile *profile,
                                     OdError *error);

#endif
