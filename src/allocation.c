#include "opendoor/allocation.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OD_PORT_COUNT 65536U

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

void od_allocation_plan_free(OdAllocationPlan *plan) {
    if (plan == NULL) {
        return;
    }
    for (size_t index = 0U; index < plan->count; ++index) {
        free(plan->items[index].service_id);
        free(plan->items[index].variable);
    }
    free(plan->items);
    *plan = (OdAllocationPlan){0};
}

static bool profile_has_variable(const OdProfile *profile, const char *variable) {
    for (size_t index = 0U; index < profile->service_count; ++index) {
        if (strcmp(profile->services[index].variable, variable) == 0) {
            return true;
        }
    }
    return false;
}

static OdStatus fill_allocation(OdAllocation *allocation,
                                const OdService *service,
                                uint16_t old_port,
                                uint16_t new_port,
                                OdAllocationReason reason,
                                OdError *error) {
    allocation->service_id = copy_string(service->id);
    allocation->variable = copy_string(service->variable);
    allocation->old_port = old_port;
    allocation->new_port = new_port;
    allocation->reason = reason;
    if (allocation->service_id == NULL || allocation->variable == NULL) {
        free(allocation->service_id);
        free(allocation->variable);
        *allocation = (OdAllocation){0};
        od_error_set(error, OD_ERROR_MEMORY, "unable to store allocation");
        return OD_ERROR_MEMORY;
    }
    return OD_OK;
}

OdStatus od_allocate(const OdProfile *profile,
                     const OdOccupiedPort *occupied,
                     size_t occupied_count,
                     const OdAssignments *saved,
                     OdAllocationPlan *plan,
                     OdError *error) {
    if (profile == NULL || plan == NULL || (occupied_count > 0U && occupied == NULL)) {
        od_error_set(error, OD_ERROR_INVALID, "profile, occupied ports, and plan are required");
        return OD_ERROR_INVALID;
    }
    *plan = (OdAllocationPlan){0};
    OdStatus status = od_profile_validate(profile, error);
    if (status != OD_OK) {
        return status;
    }
    if (profile->service_count == 0U) {
        od_error_clear(error);
        return OD_OK;
    }

    bool *blocked = calloc(OD_PORT_COUNT, sizeof(*blocked));
    bool *used = calloc(OD_PORT_COUNT, sizeof(*used));
    size_t *preferred_count = calloc(OD_PORT_COUNT, sizeof(*preferred_count));
    bool *allocated = calloc(profile->service_count, sizeof(*allocated));
    if (blocked == NULL || used == NULL || preferred_count == NULL || allocated == NULL) {
        free(blocked);
        free(used);
        free(preferred_count);
        free(allocated);
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate port map");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < occupied_count; ++index) {
        blocked[occupied[index].port] = true;
    }
    if (saved != NULL) {
        for (size_t index = 0U; index < saved->count; ++index) {
            if (!profile_has_variable(profile, saved->items[index].variable)) {
                blocked[saved->items[index].port] = true;
            }
        }
    }
    for (size_t index = 0U; index < profile->service_count; ++index) {
        const OdService *service = &profile->services[index];
        if (service->managed) ++preferred_count[service->preferred_port];
    }

    plan->items = calloc(profile->service_count, sizeof(*plan->items));
    if (plan->items == NULL) {
        free(blocked);
        free(used);
        free(preferred_count);
        free(allocated);
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate result plan");
        return OD_ERROR_MEMORY;
    }
    plan->count = profile->service_count;

    for (size_t index = 0U; index < profile->service_count && status == OD_OK; ++index) {
        const OdService *service = &profile->services[index];
        if (!service->managed || saved == NULL) {
            continue;
        }
        const OdAssignment *existing = od_assignments_find(saved, service->variable);
        if (existing == NULL || existing->port < profile->port_min || existing->port > profile->port_max ||
            blocked[existing->port] || used[existing->port]) {
            continue;
        }
        status = fill_allocation(&plan->items[index], service, existing->port,
                                 existing->port, OD_ALLOC_PRESERVED, error);
        if (status == OD_OK) {
            used[existing->port] = true;
            allocated[index] = true;
        }
    }

    for (size_t index = 0U; index < profile->service_count && status == OD_OK; ++index) {
        const OdService *service = &profile->services[index];
        if (allocated[index]) {
            continue;
        }
        const OdAssignment *existing = saved == NULL ? NULL :
                                       od_assignments_find(saved, service->variable);
        uint16_t old_port = existing == NULL ? 0U : existing->port;
        uint16_t selected = 0U;
        for (unsigned candidate = service->preferred_port;
             candidate <= (unsigned)profile->port_max;
             ++candidate) {
            uint16_t port = (uint16_t)candidate;
            bool protects_other = port != service->preferred_port &&
                                  preferred_count[port] > 0U;
            if (!blocked[port] && !used[port] && !protects_other) {
                selected = port;
                break;
            }
        }
        if (selected == 0U) {
            od_error_set(error, OD_ERROR_EXHAUSTED,
                         "no free port at or above %u for %s in range %u-%u",
                         (unsigned)service->preferred_port,
                         service->id,
                         (unsigned)profile->port_min,
                         (unsigned)profile->port_max);
            status = OD_ERROR_EXHAUSTED;
            break;
        }
        OdAllocationReason reason =
            (old_port != 0U && old_port != selected) || selected != service->preferred_port ?
                OD_ALLOC_REASSIGNED : OD_ALLOC_PREFERRED;
        status = fill_allocation(&plan->items[index], service, old_port, selected, reason, error);
        if (status == OD_OK) {
            used[selected] = true;
            allocated[index] = true;
        }
    }

    free(blocked);
    free(used);
    free(preferred_count);
    free(allocated);
    if (status != OD_OK) {
        od_allocation_plan_free(plan);
    } else {
        od_error_clear(error);
    }
    return status;
}
