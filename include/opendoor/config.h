#ifndef OPENDOOR_CONFIG_H
#define OPENDOOR_CONFIG_H

#include "opendoor/model.h"

OdStatus od_profile_parse(const char *text, size_t length, OdProfile *profile, OdError *error);
OdStatus od_profile_load(const char *path, OdProfile *profile, OdError *error);
OdStatus od_profile_render(const OdProfile *profile, char **text, size_t *length, OdError *error);
OdStatus od_settings_parse(const char *text, size_t length, OdSettings *settings, OdError *error);
OdStatus od_settings_load(const char *path, OdSettings *settings, OdError *error);
OdStatus od_settings_render(const OdSettings *settings, char **text, size_t *length, OdError *error);

#endif
