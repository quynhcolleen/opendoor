#ifndef OPENDOOR_DOCKER_H
#define OPENDOOR_DOCKER_H

#include "opendoor/scan.h"

OdStatus od_docker_parse_ps_json_lines(const char *text,
                                       size_t length,
                                       OdScanSnapshot *snapshot,
                                       OdError *error);
OdStatus od_docker_scan(const char *binary,
                        unsigned timeout_milliseconds,
                        size_t output_limit,
                        OdScanSnapshot *snapshot,
                        OdError *error);

#endif

