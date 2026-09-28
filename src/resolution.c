#include "opendoor/resolution.h"

#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdAllocationPlan *plan,
                            OdError *error) {
    if (resolution == NULL || plan == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "port proposal is required");
        return OD_ERROR_INVALID;
    }
    *resolution = (OdResolution){0};
    for (size_t index = 0U; index < plan->count; ++index) {
        if (plan->items[index].reason == OD_ALLOC_REASSIGNED) ++resolution->count;
    }
    if (resolution->count == 0U) {
        od_error_clear(error);
        return OD_OK;
    }
    resolution->items = calloc(resolution->count, sizeof(*resolution->items));
    if (resolution->items == NULL) {
        *resolution = (OdResolution){0};
        od_error_set(error, OD_ERROR_MEMORY, "unable to store conflict list");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < plan->count; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        if (allocation->reason != OD_ALLOC_REASSIGNED) continue;
        OdResolutionItem *item = &resolution->items[output++];
        item->allocation_index = index;
        item->variable = copy_string(allocation->variable);
        item->old_port = allocation->old_port;
        item->new_port = allocation->new_port;
        if (item->variable == NULL) {
            od_resolution_free(resolution);
            od_error_set(error, OD_ERROR_MEMORY, "unable to copy conflict");
            return OD_ERROR_MEMORY;
        }
    }
    od_error_clear(error);
    return OD_OK;
}

void od_resolution_free(OdResolution *resolution) {
    if (resolution == NULL) return;
    for (size_t index = 0U; index < resolution->count; ++index) {
        free(resolution->items[index].variable);
    }
    free(resolution->items);
    *resolution = (OdResolution){0};
}
