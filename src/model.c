#include "opendoor/model.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static void string_list_free(OdStringList *list) {
    if (list == NULL) {
        return;
    }
    for (size_t index = 0U; index < list->count; ++index) {
        free(list->items[index]);
    }
    free(list->items);
    *list = (OdStringList){0};
}

static OdStatus string_list_copy(const OdStringList *source, OdStringList *destination, OdError *error) {
    *destination = (OdStringList){0};
    if (source == NULL || source->count == 0U) {
        return OD_OK;
    }
    destination->items = calloc(source->count, sizeof(*destination->items));
    if (destination->items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate string list");
        return OD_ERROR_MEMORY;
    }
    destination->count = source->count;
    for (size_t index = 0U; index < source->count; ++index) {
        destination->items[index] = copy_string(source->items[index]);
        if (destination->items[index] == NULL) {
            string_list_free(destination);
            od_error_set(error, OD_ERROR_MEMORY, "unable to copy string list");
            return OD_ERROR_MEMORY;
        }
    }
    return OD_OK;
}

void od_service_clear(OdService *service) {
    if (service == NULL) {
        return;
    }
    free(service->id);
    free(service->name);
    free(service->group);
    free(service->variable);
    string_list_free(&service->sources);
    *service = (OdService){0};
}

void od_profile_init(OdProfile *profile) {
    if (profile == NULL) {
        return;
    }
    *profile = (OdProfile){0};
    profile->schema_version = 1U;
    profile->port_min = 1024U;
    profile->port_max = 65535U;
}

void od_profile_free(OdProfile *profile) {
    if (profile == NULL) {
        return;
    }
    free(profile->project_name);
    free(profile->assignment_file);
    string_list_free(&profile->compose_files);
    string_list_free(&profile->env_files);
    string_list_free(&profile->package_files);
    for (size_t index = 0U; index < profile->service_count; ++index) {
        od_service_clear(&profile->services[index]);
    }
    free(profile->services);
    *profile = (OdProfile){0};
}

OdStatus od_profile_add_service(OdProfile *profile, const OdService *service, OdError *error) {
    if (profile == NULL || service == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "profile and service are required");
        return OD_ERROR_INVALID;
    }
    OdService *services = realloc(profile->services,
                                  (profile->service_count + 1U) * sizeof(*services));
    if (services == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow service list");
        return OD_ERROR_MEMORY;
    }
    profile->services = services;
    OdService *copy = &profile->services[profile->service_count];
    *copy = (OdService){0};
    copy->id = copy_string(service->id);
    copy->name = copy_string(service->name);
    copy->group = copy_string(service->group);
    copy->variable = copy_string(service->variable);
    copy->preferred_port = service->preferred_port;
    copy->selected_port = service->selected_port;
    copy->protocols = service->protocols;
    copy->managed = service->managed;
    if (copy->id == NULL || copy->name == NULL || copy->group == NULL ||
        copy->variable == NULL ||
        string_list_copy(&service->sources, &copy->sources, error) != OD_OK) {
        od_service_clear(copy);
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy service");
        return OD_ERROR_MEMORY;
    }
    ++profile->service_count;
    return OD_OK;
}

static bool valid_variable(const char *variable) {
    if (variable == NULL || !(variable[0] == '_' ||
        (variable[0] >= 'A' && variable[0] <= 'Z'))) {
        return false;
    }
    for (size_t index = 1U; variable[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)variable[index];
        if (!(character == '_' || isdigit(character) ||
              (character >= 'A' && character <= 'Z'))) {
            return false;
        }
    }
    return true;
}

OdStatus od_profile_validate(const OdProfile *profile, OdError *error) {
    if (profile == NULL || profile->project_name == NULL || profile->project_name[0] == '\0' ||
        profile->assignment_file == NULL || profile->assignment_file[0] == '\0') {
        od_error_set(error, OD_ERROR_INVALID, "project_name and assignment_file are required");
        return OD_ERROR_INVALID;
    }
    if (profile->schema_version != 1U) {
        od_error_set(error, OD_ERROR_UNSUPPORTED, "unsupported schema version %u", profile->schema_version);
        return OD_ERROR_UNSUPPORTED;
    }
    if (profile->port_min == 0U || profile->port_max < profile->port_min) {
        od_error_set(error, OD_ERROR_INVALID, "invalid port range");
        return OD_ERROR_INVALID;
    }
    for (size_t index = 0U; index < profile->service_count; ++index) {
        const OdService *service = &profile->services[index];
        if (service->id == NULL || service->id[0] == '\0' || service->name == NULL ||
            service->name[0] == '\0' || service->group == NULL || service->group[0] == '\0' ||
            !valid_variable(service->variable) || service->preferred_port < profile->port_min ||
            service->preferred_port > profile->port_max || service->protocols == 0U) {
            od_error_set(error, OD_ERROR_INVALID, "invalid service at index %zu", index);
            return OD_ERROR_INVALID;
        }
        for (size_t other = 0U; other < index; ++other) {
            if (strcmp(service->id, profile->services[other].id) == 0) {
                od_error_set(error, OD_ERROR_INVALID, "duplicate service id: %s", service->id);
                return OD_ERROR_INVALID;
            }
            if (strcmp(service->variable, profile->services[other].variable) == 0) {
                od_error_set(error, OD_ERROR_INVALID, "duplicate variable: %s", service->variable);
                return OD_ERROR_INVALID;
            }
        }
    }
    od_error_clear(error);
    return OD_OK;
}

void od_settings_defaults(OdSettings *settings) {
    if (settings == NULL) {
        return;
    }
    *settings = (OdSettings){0};
    settings->schema_version = 1U;
    (void)strcpy(settings->theme, "midnight");
    settings->unicode_mode = OD_UNICODE_AUTO;
    settings->mouse = true;
    settings->refresh_seconds = 5U;
}

