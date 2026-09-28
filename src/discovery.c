#include "opendoor/discovery.h"

#include <stdbool.h>
#include <stdlib.h>

#define OD_PORT_COUNT 65536U

OdStatus od_discover_occupied_ports(const OdScanSnapshot *snapshot,
                                    uint16_t **ports,
                                    size_t *count,
                                    OdError *error) {
    if (snapshot == NULL || ports == NULL || count == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "scan snapshot and port output are required");
        return OD_ERROR_INVALID;
    }
    *ports = NULL;
    *count = 0U;
    bool *present = calloc(OD_PORT_COUNT, sizeof(*present));
    if (present == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to index occupied ports");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        uint16_t port = snapshot->endpoints[index].local_port;
        if (port != 0U && !present[port]) {
            present[port] = true;
            ++*count;
        }
    }
    if (*count > 0U) {
        *ports = malloc(*count * sizeof(**ports));
        if (*ports == NULL) {
            free(present);
            *count = 0U;
            od_error_set(error, OD_ERROR_MEMORY, "unable to store occupied ports");
            return OD_ERROR_MEMORY;
        }
    }
    size_t output = 0U;
    for (unsigned port = 1U; port <= 65535U; ++port) {
        if (present[port]) (*ports)[output++] = (uint16_t)port;
    }
    free(present);
    od_error_clear(error);
    return OD_OK;
}
