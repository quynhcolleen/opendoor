#include "opendoor/onboarding.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static uint64_t fnv1a_string(const char *value, uint64_t hash) {
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        hash ^= (unsigned char)value[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void update_stable_id(OdCandidate *candidate) {
    uint64_t hash = fnv1a_string(candidate->variable, UINT64_C(1469598103934665603));
    hash ^= candidate->port;
    hash *= UINT64_C(1099511628211);
    (void)snprintf(candidate->stable_id, sizeof(candidate->stable_id),
                   "candidate-%016llx", (unsigned long long)hash);
}

static bool valid_variable(const char *variable) {
    if (variable == NULL || !(variable[0] == '_' ||
        (variable[0] >= 'A' && variable[0] <= 'Z'))) return false;
    for (size_t index = 1U; variable[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)variable[index];
        if (!(character == '_' || isdigit(character) ||
              (character >= 'A' && character <= 'Z'))) return false;
    }
    return true;
}

static bool duplicate_variable(const OdOnboarding *onboarding,
                               const char *variable,
                               size_t except) {
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        if (index != except && strcmp(onboarding->candidates.items[index].variable, variable) == 0) {
            return true;
        }
    }
    return false;
}

static OdStatus validate_fields(const OdOnboarding *onboarding,
                                const char *name,
                                const char *group,
                                const char *variable,
                                uint16_t port,
                                size_t except,
                                OdError *error) {
    if (name == NULL || name[0] == '\0') {
        od_error_set(error, OD_ERROR_INVALID, "service name is required");
        return OD_ERROR_INVALID;
    }
    if (group == NULL || group[0] == '\0') {
        od_error_set(error, OD_ERROR_INVALID, "service group is required");
        return OD_ERROR_INVALID;
    }
    if (!valid_variable(variable)) {
        od_error_set(error, OD_ERROR_INVALID, "variable must match [A-Z_][A-Z0-9_]*");
        return OD_ERROR_INVALID;
    }
    if (port < onboarding->port_min || port > onboarding->port_max) {
        od_error_set(error, OD_ERROR_INVALID, "port must be in range %u-%u",
                     (unsigned)onboarding->port_min, (unsigned)onboarding->port_max);
        return OD_ERROR_INVALID;
    }
    if (duplicate_variable(onboarding, variable, except)) {
        od_error_set(error, OD_ERROR_INVALID, "duplicate variable: %s", variable);
        return OD_ERROR_INVALID;
    }
    return OD_OK;
}

OdStatus od_onboarding_init(OdOnboarding *onboarding,
                            const OdCandidateList *candidates,
                            size_t page_size,
                            uint16_t port_min,
                            uint16_t port_max,
                            OdError *error) {
    if (onboarding == NULL || candidates == NULL || page_size == 0U ||
        port_min == 0U || port_max < port_min) {
        od_error_set(error, OD_ERROR_INVALID, "onboarding options are invalid");
        return OD_ERROR_INVALID;
    }
    *onboarding = (OdOnboarding){0};
    od_candidate_list_init(&onboarding->candidates);
    OdStatus status = od_candidates_merge(&onboarding->candidates, candidates, error);
    if (status != OD_OK) return status;
    onboarding->reviewed = calloc(candidates->count, sizeof(*onboarding->reviewed));
    if (candidates->count > 0U && onboarding->reviewed == NULL) {
        od_candidate_list_free(&onboarding->candidates);
        od_error_set(error, OD_ERROR_MEMORY, "unable to track candidate review state");
        return OD_ERROR_MEMORY;
    }
    onboarding->page_size = page_size;
    onboarding->port_min = port_min;
    onboarding->port_max = port_max;
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        if (onboarding->candidates.items[index].confidence == OD_CONFIDENCE_CONFIRMED) {
            onboarding->candidates.items[index].selected = true;
        }
    }
    od_error_clear(error);
    return OD_OK;
}

void od_onboarding_free(OdOnboarding *onboarding) {
    if (onboarding == NULL) return;
    od_candidate_list_free(&onboarding->candidates);
    free(onboarding->reviewed);
    *onboarding = (OdOnboarding){0};
}

size_t od_onboarding_page(const OdOnboarding *onboarding) {
    return onboarding->page_size == 0U ? 0U : onboarding->selected / onboarding->page_size;
}

size_t od_onboarding_page_count(const OdOnboarding *onboarding) {
    if (onboarding->page_size == 0U || onboarding->candidates.count == 0U) return 1U;
    return (onboarding->candidates.count + onboarding->page_size - 1U) / onboarding->page_size;
}

size_t od_onboarding_page_start(const OdOnboarding *onboarding) {
    return od_onboarding_page(onboarding) * onboarding->page_size;
}

static size_t clamp_selection(const OdOnboarding *onboarding, long long target) {
    if (onboarding->candidates.count == 0U || target <= 0LL) return 0U;
    size_t maximum = onboarding->candidates.count - 1U;
    return (unsigned long long)target > maximum ? maximum : (size_t)target;
}

void od_onboarding_move(OdOnboarding *onboarding, int rows) {
    if (onboarding == NULL) return;
    long long target = (long long)onboarding->selected + rows;
    onboarding->selected = clamp_selection(onboarding, target);
}

void od_onboarding_move_page(OdOnboarding *onboarding, int pages) {
    if (onboarding == NULL) return;
    long long distance = (long long)pages * (long long)onboarding->page_size;
    long long target = (long long)onboarding->selected + distance;
    onboarding->selected = clamp_selection(onboarding, target);
}

void od_onboarding_toggle_selected(OdOnboarding *onboarding) {
    if (onboarding == NULL || onboarding->selected >= onboarding->candidates.count) return;
    OdCandidate *candidate = &onboarding->candidates.items[onboarding->selected];
    candidate->selected = !candidate->selected;
}

void od_onboarding_review_selected(OdOnboarding *onboarding) {
    if (onboarding == NULL || onboarding->selected >= onboarding->candidates.count) return;
    onboarding->reviewed[onboarding->selected] = true;
    if (onboarding->selected + 1U < onboarding->candidates.count) ++onboarding->selected;
}

bool od_onboarding_all_reviewed(const OdOnboarding *onboarding) {
    if (onboarding == NULL) return false;
    for (size_t index = 0U; index < onboarding->candidates.count; ++index) {
        if (!onboarding->reviewed[index]) return false;
    }
    return true;
}

OdStatus od_onboarding_edit_selected(OdOnboarding *onboarding,
                                     const char *name,
                                     const char *group,
                                     const char *variable,
                                     uint16_t port,
                                     unsigned protocols,
                                     OdError *error) {
    if (onboarding == NULL || onboarding->selected >= onboarding->candidates.count) {
        od_error_set(error, OD_ERROR_INVALID, "no candidate is selected");
        return OD_ERROR_INVALID;
    }
    if (protocols == 0U ||
        (protocols & ~(unsigned)(OD_PROTOCOL_TCP | OD_PROTOCOL_UDP)) != 0U) {
        od_error_set(error, OD_ERROR_INVALID, "service protocols are invalid");
        return OD_ERROR_INVALID;
    }
    OdStatus status = validate_fields(onboarding, name, group, variable, port,
                                      onboarding->selected, error);
    if (status != OD_OK) return status;
    char *name_copy = copy_string(name);
    char *group_copy = copy_string(group);
    char *variable_copy = copy_string(variable);
    if (name_copy == NULL || group_copy == NULL || variable_copy == NULL) {
        free(name_copy);
        free(group_copy);
        free(variable_copy);
        od_error_set(error, OD_ERROR_MEMORY, "unable to update candidate");
        return OD_ERROR_MEMORY;
    }
    OdCandidate *candidate = &onboarding->candidates.items[onboarding->selected];
    free(candidate->name);
    free(candidate->group);
    free(candidate->variable);
    candidate->name = name_copy;
    candidate->group = group_copy;
    candidate->variable = variable_copy;
    candidate->port = port;
    candidate->protocols = protocols;
    update_stable_id(candidate);
    onboarding->reviewed[onboarding->selected] = true;
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_onboarding_add_manual(OdOnboarding *onboarding,
                                  const char *name,
                                  const char *group,
                                  const char *variable,
                                  uint16_t port,
                                  unsigned protocols,
                                  OdError *error) {
    if (onboarding == NULL || protocols == 0U ||
        (protocols & ~(unsigned)(OD_PROTOCOL_TCP | OD_PROTOCOL_UDP)) != 0U) {
        od_error_set(error, OD_ERROR_INVALID, "manual service protocols are invalid");
        return OD_ERROR_INVALID;
    }
    OdStatus status = validate_fields(onboarding, name, group, variable, port,
                                      SIZE_MAX, error);
    if (status != OD_OK) return status;
    size_t old_count = onboarding->candidates.count;
    OdCandidate *items = realloc(onboarding->candidates.items,
                                 (old_count + 1U) * sizeof(*items));
    if (items == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to add manual candidate");
        return OD_ERROR_MEMORY;
    }
    onboarding->candidates.items = items;
    bool *reviewed = realloc(onboarding->reviewed, (old_count + 1U) * sizeof(*reviewed));
    if (reviewed == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to track manual candidate");
        return OD_ERROR_MEMORY;
    }
    onboarding->reviewed = reviewed;
    OdCandidate *candidate = &items[old_count];
    *candidate = (OdCandidate){0};
    candidate->name = copy_string(name);
    candidate->group = copy_string(group);
    candidate->variable = copy_string(variable);
    candidate->port = port;
    candidate->protocols = protocols;
    candidate->confidence = OD_CONFIDENCE_CONFIRMED;
    candidate->selected = true;
    candidate->sources.items = calloc(1U, sizeof(*candidate->sources.items));
    if (candidate->sources.items != NULL) {
        candidate->sources.items[0] = copy_string("manual:user");
        candidate->sources.count = candidate->sources.items[0] == NULL ? 0U : 1U;
    }
    if (candidate->name == NULL || candidate->group == NULL || candidate->variable == NULL ||
        candidate->sources.count != 1U) {
        free(candidate->name);
        free(candidate->group);
        free(candidate->variable);
        if (candidate->sources.items != NULL) free(candidate->sources.items[0]);
        free(candidate->sources.items);
        *candidate = (OdCandidate){0};
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy manual candidate");
        return OD_ERROR_MEMORY;
    }
    update_stable_id(candidate);
    onboarding->reviewed[old_count] = false;
    onboarding->candidates.count = old_count + 1U;
    onboarding->selected = old_count;
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_onboarding_build_profile(const OdOnboarding *onboarding,
                                     const char *project_name,
                                     const char *assignment_file,
                                     OdProfile *profile,
                                     OdError *error) {
    if (onboarding == NULL || project_name == NULL || assignment_file == NULL || profile == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "onboarding profile inputs are required");
        return OD_ERROR_INVALID;
    }
    if (!od_onboarding_all_reviewed(onboarding)) {
        od_error_set(error, OD_ERROR_CONFLICT, "every discovery candidate must be reviewed");
        return OD_ERROR_CONFLICT;
    }
    od_profile_init(profile);
    profile->project_name = copy_string(project_name);
    profile->assignment_file = copy_string(assignment_file);
    profile->port_min = onboarding->port_min;
    profile->port_max = onboarding->port_max;
    if (profile->project_name == NULL || profile->assignment_file == NULL) {
        od_profile_free(profile);
        od_error_set(error, OD_ERROR_MEMORY, "unable to create project profile");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = OD_OK;
    for (size_t index = 0U; index < onboarding->candidates.count && status == OD_OK; ++index) {
        const OdCandidate *candidate = &onboarding->candidates.items[index];
        if (!candidate->selected) continue;
        OdService service = {0};
        service.id = (char *)candidate->stable_id;
        service.name = candidate->name;
        service.group = candidate->group;
        service.variable = candidate->variable;
        service.preferred_port = candidate->port;
        service.protocols = candidate->protocols;
        service.sources = candidate->sources;
        service.managed = true;
        status = od_profile_add_service(profile, &service, error);
    }
    if (status == OD_OK) status = od_profile_validate(profile, error);
    if (status != OD_OK) od_profile_free(profile);
    return status;
}
