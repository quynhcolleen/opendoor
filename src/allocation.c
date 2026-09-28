#include "opendoor/allocation.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OD_PORT_COUNT 65536U

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

void od_allocation_plan_free(OdAllocationPlan *plan) {
    if (plan == NULL) return;
    for (size_t index = 0U; index < plan->count; ++index) {
        free(plan->items[index].variable);
    }
    free(plan->items);
    *plan = (OdAllocationPlan){0};
}

static OdStatus fill_allocation(OdAllocation *allocation,
                                const OdAssignment *assignment,
                                uint16_t new_port,
                                OdError *error) {
    allocation->variable = copy_string(assignment->variable);
    if (allocation->variable == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store port proposal");
        return OD_ERROR_MEMORY;
    }
    allocation->old_port = assignment->port;
    allocation->new_port = new_port;
    allocation->reason = assignment->port == new_port ?
        OD_ALLOC_UNCHANGED : OD_ALLOC_REASSIGNED;
    return OD_OK;
}

static uint16_t find_replacement(uint16_t old_port,
                                 const bool *blocked,
                                 const bool *used,
                                 const size_t *wanted_count) {
    unsigned start = old_port < OD_REPLACEMENT_PORT_MIN ||
                     old_port >= OD_REPLACEMENT_PORT_MAX ?
        OD_REPLACEMENT_PORT_MIN : (unsigned)old_port + 1U;
    for (unsigned candidate = start; candidate <= OD_REPLACEMENT_PORT_MAX; ++candidate) {
        if (!blocked[candidate] && !used[candidate] && wanted_count[candidate] == 0U) {
            return (uint16_t)candidate;
        }
    }
    for (unsigned candidate = OD_REPLACEMENT_PORT_MIN; candidate < start; ++candidate) {
        if (!blocked[candidate] && !used[candidate] && wanted_count[candidate] == 0U) {
            return (uint16_t)candidate;
        }
    }
    return 0U;
}

OdStatus od_allocate(const OdAssignments *wanted,
                     const uint16_t *occupied,
                     size_t occupied_count,
                     OdAllocationPlan *plan,
                     OdError *error) {
    if (wanted == NULL || plan == NULL || (occupied_count > 0U && occupied == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "wanted ports, occupied ports, and output plan are required");
        return OD_ERROR_INVALID;
    }
    *plan = (OdAllocationPlan){0};
    if (wanted->count == 0U) {
        od_error_clear(error);
        return OD_OK;
    }

    bool *blocked = calloc(OD_PORT_COUNT, sizeof(*blocked));
    bool *used = calloc(OD_PORT_COUNT, sizeof(*used));
    size_t *wanted_count = calloc(OD_PORT_COUNT, sizeof(*wanted_count));
    bool *allocated = calloc(wanted->count, sizeof(*allocated));
    plan->items = calloc(wanted->count, sizeof(*plan->items));
    if (blocked == NULL || used == NULL || wanted_count == NULL ||
        allocated == NULL || plan->items == NULL) {
        free(blocked);
        free(used);
        free(wanted_count);
        free(allocated);
        od_allocation_plan_free(plan);
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate port proposal");
        return OD_ERROR_MEMORY;
    }
    plan->count = wanted->count;
    for (size_t index = 0U; index < occupied_count; ++index) blocked[occupied[index]] = true;
    for (size_t index = 0U; index < wanted->count; ++index) {
        ++wanted_count[wanted->items[index].port];
    }

    OdStatus status = OD_OK;
    for (size_t index = 0U; index < wanted->count && status == OD_OK; ++index) {
        uint16_t port = wanted->items[index].port;
        if (blocked[port] || used[port]) continue;
        status = fill_allocation(&plan->items[index], &wanted->items[index], port, error);
        if (status == OD_OK) {
            allocated[index] = true;
            used[port] = true;
        }
    }
    for (size_t index = 0U; index < wanted->count && status == OD_OK; ++index) {
        if (allocated[index]) continue;
        uint16_t selected = find_replacement(wanted->items[index].port,
                                             blocked, used, wanted_count);
        if (selected == 0U) {
            od_error_set(error, OD_ERROR_EXHAUSTED,
                         "no replacement port is available from %u to %u",
                         OD_REPLACEMENT_PORT_MIN, OD_REPLACEMENT_PORT_MAX);
            status = OD_ERROR_EXHAUSTED;
            break;
        }
        status = fill_allocation(&plan->items[index], &wanted->items[index],
                                 selected, error);
        if (status == OD_OK) {
            allocated[index] = true;
            used[selected] = true;
        }
    }

    free(blocked);
    free(used);
    free(wanted_count);
    free(allocated);
    if (status != OD_OK) {
        od_allocation_plan_free(plan);
    } else {
        od_error_clear(error);
    }
    return status;
}
