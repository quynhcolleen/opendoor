#ifndef OPENDOOR_CONFIG_H
#define OPENDOOR_CONFIG_H

#include "opendoor/dotenv.h"

#include <stddef.h>

OdStatus od_config_parse(const char *text,
                         size_t length,
                         OdAssignments *assignments,
                         OdError *error);
OdStatus od_config_load(const char *path,
                        OdAssignments *assignments,
                        OdError *error);
OdStatus od_config_render(const OdAssignments *assignments,
                          char **text,
                          size_t *length,
                          OdError *error);

#endif
