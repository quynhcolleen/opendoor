#ifndef OPENDOOR_PERSISTENCE_H
#define OPENDOOR_PERSISTENCE_H

#include "opendoor/allocation.h"
#include "opendoor/common.h"
#include "opendoor/model.h"
#include "opendoor/scan.h"

#include <stddef.h>

OdStatus od_assignments_load_file(const char *path,
                                  OdAssignments *assignments,
                                  OdError *error);
OdStatus od_assignments_import_file(const char *path,
                                    OdAssignments *assignments,
                                    OdError *error);
OdStatus od_plan_to_assignments(const OdAllocationPlan *plan,
                                OdAssignments *assignments,
                                OdError *error);
OdStatus od_plan_validate_snapshot(const OdProfile *profile,
                                   const OdAllocationPlan *plan,
                                   const OdScanSnapshot *snapshot,
                                   OdError *error);
OdStatus od_plan_probe_bindings(const OdProfile *profile,
                                const OdAllocationPlan *plan,
                                OdError *error);
OdStatus od_project_save(const char *project_root,
                         const char *profile_path,
                         const OdProfile *profile,
                         const OdAllocationPlan *plan,
                         OdError *error);
OdStatus od_project_save_importing_foreign(const char *project_root,
                                           const char *profile_path,
                                           const OdProfile *profile,
                                           const OdAllocationPlan *plan,
                                           OdError *error);
OdStatus od_project_reset_assignments(const char *project_root,
                                      const OdProfile *profile,
                                      OdError *error);

#endif
