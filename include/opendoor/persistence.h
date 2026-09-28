#ifndef OPENDOOR_PERSISTENCE_H
#define OPENDOOR_PERSISTENCE_H

#include "opendoor/dotenv.h"

OdStatus od_config_write(const char *path,
                         const OdAssignments *assignments,
                         OdError *error);

#endif
