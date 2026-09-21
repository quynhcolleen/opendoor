#ifndef OPENDOOR_MODEL_H
#define OPENDOOR_MODEL_H

#include "opendoor/common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    OD_PROTOCOL_TCP = 1U,
    OD_PROTOCOL_UDP = 2U
};

typedef struct {
    char **items;
    size_t count;
} OdStringList;

typedef struct {
    char *id;
    char *name;
    char *group;
    char *variable;
    uint16_t preferred_port;
    uint16_t selected_port;
    unsigned protocols;
    OdStringList sources;
    bool managed;
} OdService;

typedef struct {
    unsigned schema_version;
    char *project_name;
    char *assignment_file;
    uint16_t port_min;
    uint16_t port_max;
    OdStringList compose_files;
    OdStringList env_files;
    OdStringList package_files;
    OdService *services;
    size_t service_count;
} OdProfile;

typedef enum {
    OD_UNICODE_AUTO,
    OD_UNICODE_ALWAYS,
    OD_UNICODE_NEVER
} OdUnicodeMode;

typedef struct {
    unsigned schema_version;
    char theme[24];
    OdUnicodeMode unicode_mode;
    bool reduced_motion;
    bool mouse;
    bool auto_refresh;
    unsigned refresh_seconds;
} OdSettings;

void od_profile_init(OdProfile *profile);
void od_profile_free(OdProfile *profile);
OdStatus od_profile_add_service(OdProfile *profile, const OdService *service, OdError *error);
OdStatus od_profile_validate(const OdProfile *profile, OdError *error);
void od_settings_defaults(OdSettings *settings);
void od_service_clear(OdService *service);

#endif

