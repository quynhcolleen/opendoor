#ifndef OPENDOOR_DISCOVERY_H
#define OPENDOOR_DISCOVERY_H

#include "opendoor/scan.h"

#include <stddef.h>
#include <stdint.h>

OdStatus od_discover_occupied_ports(const OdScanSnapshot *snapshot,
                                    uint16_t **ports,
                                    size_t *count,
                                    OdError *error);

#endif
