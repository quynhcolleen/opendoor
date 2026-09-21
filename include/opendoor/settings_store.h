#ifndef OPENDOOR_SETTINGS_STORE_H
#define OPENDOOR_SETTINGS_STORE_H

#include "opendoor/common.h"
#include "opendoor/model.h"

#include <stddef.h>

OdStatus od_settings_resolve_path(char *path, size_t capacity, OdError *error);
OdStatus od_settings_save(const char *path, const OdSettings *settings, OdError *error);

#endif
