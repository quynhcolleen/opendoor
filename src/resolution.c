#include "opendoor/resolution.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const OdService *find_service(const OdProfile *profile, const char *id) {
    for (size_t index = 0U; index < profile->service_count; ++index) {
        if (strcmp(profile->services[index].id, id) == 0) return &profile->services[index];
    }
    return NULL;
}

static void describe_owner(const OdScanSnapshot *snapshot,
                           uint16_t port,
                           char *owner,
                           size_t capacity) {
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        if (endpoint->local_port != port) continue;
        (void)snprintf(owner, capacity, "%s%s%ld on %s:%u",
                       endpoint->process[0] == '\0' ? "unknown process" : endpoint->process,
                       endpoint->pid == 0 ? "" : " pid ",
                       endpoint->pid == 0 ? 0L : (long)endpoint->pid,
                       endpoint->local_address[0] == '\0' ? "wildcard" : endpoint->local_address,
                       (unsigned)port);
        return;
    }
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        const OdDockerMapping *mapping = &snapshot->docker_mappings[index];
        if (mapping->host_port != port) continue;
        (void)snprintf(owner, capacity, "Docker %.80s (project %.80s) on port %u",
                       mapping->container,
                       mapping->project[0] == '\0' ? "unknown" : mapping->project,
                       (unsigned)port);
        return;
    }
    (void)snprintf(owner, capacity, "Port %u became unavailable during allocation",
                   (unsigned)port);
}

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdProfile *profile,
                            const OdScanSnapshot *snapshot,
                            OdAllocationPlan *plan,
                            OdError *error) {
    if (resolution == NULL || profile == NULL || snapshot == NULL || plan == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "resolution inputs are required");
        return OD_ERROR_INVALID;
    }
    *resolution = (OdResolution){0};
    resolution->profile = profile;
    resolution->snapshot = snapshot;
    resolution->plan = plan;
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
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare conflict review");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < plan->count; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        if (allocation->reason != OD_ALLOC_REASSIGNED) continue;
        OdResolutionItem *item = &resolution->items[output++];
        item->allocation_index = index;
        item->original_port = allocation->old_port;
        const OdService *service = find_service(profile, allocation->service_id);
        (void)snprintf(item->service, sizeof(item->service), "%s",
                       service == NULL ? allocation->service_id : service->name);
        describe_owner(snapshot,
                       allocation->old_port == 0U && service != NULL ?
                           service->preferred_port : allocation->old_port,
                       item->owner, sizeof(item->owner));
    }
    od_error_clear(error);
    return OD_OK;
}

void od_resolution_free(OdResolution *resolution) {
    if (resolution == NULL) return;
    free(resolution->items);
    *resolution = (OdResolution){0};
}

const OdResolutionItem *od_resolution_current(const OdResolution *resolution) {
    if (resolution == NULL || resolution->current >= resolution->count) return NULL;
    return &resolution->items[resolution->current];
}

bool od_resolution_done(const OdResolution *resolution) {
    return resolution == NULL || resolution->current >= resolution->count;
}

void od_resolution_accept(OdResolution *resolution) {
    if (od_resolution_done(resolution)) return;
    resolution->items[resolution->current].decision = OD_RESOLUTION_ACCEPTED;
    ++resolution->current;
}

void od_resolution_skip(OdResolution *resolution) {
    if (od_resolution_done(resolution)) return;
    OdResolutionItem *item = &resolution->items[resolution->current];
    item->decision = OD_RESOLUTION_SKIPPED;
    resolution->plan->items[item->allocation_index].new_port = 0U;
    ++resolution->current;
}

static bool port_occupied(const OdScanSnapshot *snapshot, uint16_t port) {
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        if (snapshot->endpoints[index].local_port == port) return true;
    }
    for (size_t index = 0U; index < snapshot->docker_mapping_count; ++index) {
        if (snapshot->docker_mappings[index].host_port == port) return true;
    }
    return false;
}

OdStatus od_resolution_edit(OdResolution *resolution,
                            uint16_t port,
                            OdError *error) {
    if (od_resolution_done(resolution)) {
        od_error_set(error, OD_ERROR_INVALID, "no conflict is awaiting review");
        return OD_ERROR_INVALID;
    }
    if (port < resolution->profile->port_min || port > resolution->profile->port_max) {
        od_error_set(error, OD_ERROR_INVALID, "port must be in range %u-%u",
                     (unsigned)resolution->profile->port_min,
                     (unsigned)resolution->profile->port_max);
        return OD_ERROR_INVALID;
    }
    if (port_occupied(resolution->snapshot, port)) {
        od_error_set(error, OD_ERROR_CONFLICT, "port %u is occupied", (unsigned)port);
        return OD_ERROR_CONFLICT;
    }
    size_t allocation_index = resolution->items[resolution->current].allocation_index;
    for (size_t index = 0U; index < resolution->plan->count; ++index) {
        if (index != allocation_index && resolution->plan->items[index].new_port == port) {
            od_error_set(error, OD_ERROR_CONFLICT,
                         "port %u is already selected for another service", (unsigned)port);
            return OD_ERROR_CONFLICT;
        }
    }
    resolution->plan->items[allocation_index].new_port = port;
    od_error_clear(error);
    return OD_OK;
}
