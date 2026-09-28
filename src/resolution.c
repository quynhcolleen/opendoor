#include "opendoor/resolution.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t key;
    uint16_t port;
    bool conflict;
} ResolutionGroup;

static char *copy_string(const char *value) {
    if (value == NULL) value = "";
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static bool path_is_within(const char *root, const char *candidate) {
    size_t root_length = strlen(root);
    return strcmp(root, candidate) == 0 ||
           (strncmp(root, candidate, root_length) == 0 &&
            candidate[root_length] == '/');
}

static bool endpoint_is_project_owned(const char *canonical_root,
                                      const OdEndpoint *endpoint) {
    char *owner = NULL;
    if (endpoint->directory[0] != '\0') {
        owner = realpath(endpoint->directory, NULL);
    } else if (endpoint->executable[0] != '\0') {
        owner = realpath(endpoint->executable, NULL);
        if (owner != NULL) {
            char *slash = strrchr(owner, '/');
            if (slash == owner) {
                slash[1] = '\0';
            } else if (slash != NULL) {
                *slash = '\0';
            } else {
                free(owner);
                owner = NULL;
            }
        }
    }
    bool project_owned = owner != NULL && path_is_within(canonical_root, owner);
    free(owner);
    return project_owned;
}

static bool port_has_external_owner(const char *canonical_root,
                                    const OdScanSnapshot *snapshot,
                                    uint16_t port) {
    for (size_t index = 0U; index < snapshot->endpoint_count; ++index) {
        const OdEndpoint *endpoint = &snapshot->endpoints[index];
        if (endpoint->local_port == port &&
            !endpoint_is_project_owned(canonical_root, endpoint)) return true;
    }
    return false;
}

static char *preview_line(const char *prefix,
                          const char *line,
                          size_t column,
                          size_t token_length,
                          uint16_t new_port,
                          bool replace) {
    if (line == NULL) line = "";
    size_t line_length = strlen(line);
    size_t token_start = column > 0U ? column - 1U : 0U;
    if (token_start > line_length) token_start = line_length;
    if (token_length > line_length - token_start) token_length = 0U;
    char port_text[6];
    int written = snprintf(port_text, sizeof(port_text), "%u", (unsigned)new_port);
    if (written < 0 || (size_t)written >= sizeof(port_text)) return NULL;
    size_t replacement_length = replace ? (size_t)written : 0U;
    size_t kept_length = replace ? line_length - token_length : line_length;
    size_t prefix_length = strlen(prefix);
    if (prefix_length > SIZE_MAX - kept_length - replacement_length - 1U) return NULL;
    char *preview = malloc(prefix_length + kept_length + replacement_length + 1U);
    if (preview == NULL) return NULL;
    memcpy(preview, prefix, prefix_length);
    size_t output = prefix_length;
    if (replace) {
        memcpy(preview + output, line, token_start);
        output += token_start;
        memcpy(preview + output, port_text, replacement_length);
        output += replacement_length;
        memcpy(preview + output, line + token_start + token_length,
               line_length - token_start - token_length);
        output += line_length - token_start - token_length;
    } else {
        memcpy(preview + output, line, line_length);
        output += line_length;
    }
    preview[output] = '\0';
    return preview;
}

static bool automatic_declaration(const OdPortDeclaration *declaration) {
    return declaration->declaration_kind == OD_DECLARATION_LITERAL &&
           (declaration->write_kind == OD_WRITE_ENV_LITERAL ||
            declaration->write_kind == OD_WRITE_COMPOSE_LITERAL);
}

static OdStatus copy_resolution_item(OdResolutionItem *item,
                                     size_t declaration_index,
                                     size_t allocation_index,
                                     const OdPortDeclaration *declaration,
                                     uint16_t new_port,
                                     OdError *error) {
    *item = (OdResolutionItem){
        .declaration_index = declaration_index,
        .allocation_index = allocation_index,
        .old_port = declaration->port,
        .new_port = new_port,
        .automatic = automatic_declaration(declaration),
        .source_kind = declaration->source_kind,
        .write_kind = declaration->write_kind,
        .line = declaration->line,
        .column = declaration->column,
        .byte_offset = declaration->byte_offset,
        .byte_length = declaration->byte_length,
        .file_size = declaration->file_size,
        .file_hash = declaration->file_hash
    };
    item->variable = copy_string(declaration->environment_key);
    item->absolute_path = copy_string(declaration->absolute_path);
    item->relative_path = copy_string(declaration->relative_path);
    item->line_before = preview_line("- ", declaration->line_text,
                                     declaration->column,
                                     declaration->byte_length, new_port, false);
    item->line_after = preview_line("+ ", declaration->line_text,
                                    declaration->column,
                                    declaration->byte_length, new_port, true);
    item->manual_reason = copy_string(declaration->manual_reason);
    if (item->variable == NULL || item->absolute_path == NULL ||
        item->relative_path == NULL || item->line_before == NULL ||
        item->line_after == NULL || item->manual_reason == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to copy conflict proposal");
        return OD_ERROR_MEMORY;
    }
    return OD_OK;
}

void od_resolution_free(OdResolution *resolution) {
    if (resolution == NULL) return;
    for (size_t index = 0U; index < resolution->count; ++index) {
        free(resolution->items[index].variable);
        free(resolution->items[index].absolute_path);
        free(resolution->items[index].relative_path);
        free(resolution->items[index].line_before);
        free(resolution->items[index].line_after);
        free(resolution->items[index].manual_reason);
    }
    free(resolution->items);
    free(resolution->project_root);
    *resolution = (OdResolution){0};
}

OdStatus od_resolution_init(OdResolution *resolution,
                            const OdAllocationPlan *plan,
                            OdError *error) {
    if (resolution == NULL || plan == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "port proposal is required");
        return OD_ERROR_INVALID;
    }
    *resolution = (OdResolution){0};
    for (size_t index = 0U; index < plan->count; ++index) {
        if (plan->items[index].reason == OD_ALLOC_REASSIGNED) ++resolution->count;
    }
    if (resolution->count == 0U) {
        od_error_clear(error);
        return OD_OK;
    }
    resolution->items = calloc(resolution->count, sizeof(*resolution->items));
    if (resolution->items == NULL) {
        *resolution = (OdResolution){0};
        od_error_set(error, OD_ERROR_MEMORY, "unable to store conflict list");
        return OD_ERROR_MEMORY;
    }
    size_t output = 0U;
    for (size_t index = 0U; index < plan->count; ++index) {
        const OdAllocation *allocation = &plan->items[index];
        if (allocation->reason != OD_ALLOC_REASSIGNED) continue;
        OdResolutionItem *item = &resolution->items[output++];
        *item = (OdResolutionItem){
            .declaration_index = SIZE_MAX,
            .allocation_index = index,
            .old_port = allocation->old_port,
            .new_port = allocation->new_port,
            .automatic = true,
            .write_kind = OD_WRITE_ENV_LITERAL
        };
        item->variable = copy_string(allocation->variable);
        item->absolute_path = copy_string("");
        item->relative_path = copy_string("");
        item->manual_reason = copy_string("");
        size_t line_length = strlen(allocation->variable) + 16U;
        item->line_before = malloc(line_length);
        item->line_after = malloc(line_length);
        if (item->variable == NULL || item->absolute_path == NULL ||
            item->relative_path == NULL || item->manual_reason == NULL ||
            item->line_before == NULL || item->line_after == NULL) {
            od_resolution_free(resolution);
            od_error_set(error, OD_ERROR_MEMORY, "unable to copy conflict");
            return OD_ERROR_MEMORY;
        }
        (void)snprintf(item->line_before, line_length, "- %s=%u",
                       allocation->variable, (unsigned)allocation->old_port);
        (void)snprintf(item->line_after, line_length, "+ %s=%u",
                       allocation->variable, (unsigned)allocation->new_port);
        ++resolution->automatic_count;
    }
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_resolution_build(const char *project_root,
                             const OdProjectDiscovery *discovery,
                             const OdScanSnapshot *snapshot,
                             OdResolution *resolution,
                             OdError *error) {
    if (project_root == NULL || discovery == NULL || snapshot == NULL ||
        resolution == NULL ||
        (discovery->count > 0U && discovery->items == NULL) ||
        (snapshot->endpoint_count > 0U && snapshot->endpoints == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "project discovery and scan snapshot are required");
        return OD_ERROR_INVALID;
    }
    *resolution = (OdResolution){0};
    char *canonical_root = realpath(project_root, NULL);
    if (canonical_root == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "unable to resolve conflict project root");
        return OD_ERROR_INVALID;
    }
    resolution->project_root = copy_string(canonical_root);
    if (resolution->project_root == NULL) {
        free(canonical_root);
        od_error_set(error, OD_ERROR_MEMORY,
                     "unable to retain conflict project root");
        return OD_ERROR_MEMORY;
    }
    size_t *group_for = discovery->count == 0U ? NULL :
        malloc(discovery->count * sizeof(*group_for));
    size_t *group_by_key = discovery->count == 0U ? NULL :
        malloc(discovery->count * sizeof(*group_by_key));
    ResolutionGroup *groups = discovery->count == 0U ? NULL :
        calloc(discovery->count, sizeof(*groups));
    if (discovery->count > 0U &&
        (group_for == NULL || group_by_key == NULL || groups == NULL)) {
        free(group_for);
        free(group_by_key);
        free(groups);
        free(canonical_root);
        free(resolution->project_root);
        resolution->project_root = NULL;
        od_error_set(error, OD_ERROR_MEMORY, "unable to group conflict declarations");
        return OD_ERROR_MEMORY;
    }
    for (size_t index = 0U; index < discovery->count; ++index) {
        group_for[index] = SIZE_MAX;
        group_by_key[index] = SIZE_MAX;
    }
    size_t group_count = 0U;
    for (size_t index = 0U; index < discovery->count; ++index) {
        const OdPortDeclaration *declaration = &discovery->items[index];
        if (declaration->port == 0U) continue;
        size_t key = declaration->definition_index < discovery->count ?
            declaration->definition_index : index;
        if (group_by_key[key] == SIZE_MAX) {
            size_t group_index = group_count++;
            group_by_key[key] = group_index;
            groups[group_index].key = key;
            groups[group_index].port = discovery->items[key].port != 0U ?
                discovery->items[key].port : declaration->port;
        }
        size_t group_index = group_by_key[key];
        group_for[index] = group_index;
        if (port_has_external_owner(canonical_root, snapshot, declaration->port)) {
            groups[group_index].conflict = true;
        }
    }

    OdAssignments wanted;
    od_assignments_init(&wanted);
    bool *must_reassign = group_count == 0U ? NULL :
        calloc(group_count, sizeof(*must_reassign));
    uint16_t *occupied = snapshot->endpoint_count == 0U ? NULL :
        malloc(snapshot->endpoint_count * sizeof(*occupied));
    OdStatus status = OD_OK;
    if ((group_count > 0U && must_reassign == NULL) ||
        (snapshot->endpoint_count > 0U && occupied == NULL)) {
        status = OD_ERROR_MEMORY;
        od_error_set(error, status, "unable to build conflict allocation input");
    }
    for (size_t index = 0U; index < snapshot->endpoint_count && status == OD_OK;
         ++index) {
        occupied[index] = snapshot->endpoints[index].local_port;
    }
    for (size_t index = 0U; index < group_count && status == OD_OK; ++index) {
        char name[48];
        int written = snprintf(name, sizeof(name), "DECLARATION_%zu", groups[index].key);
        if (written < 0 || (size_t)written >= sizeof(name)) {
            status = OD_ERROR_INVALID;
            od_error_set(error, status, "too many conflict declarations");
            break;
        }
        status = od_assignments_add(&wanted, name, groups[index].port, error);
        must_reassign[index] = groups[index].conflict;
    }
    OdAllocationPlan plan = {0};
    if (status == OD_OK) {
        status = od_allocate_selected(&wanted, occupied, snapshot->endpoint_count,
                                      must_reassign, group_count, &plan, error);
    }
    if (status == OD_OK) {
        for (size_t index = 0U; index < discovery->count; ++index) {
            size_t group_index = group_for[index];
            if (group_index != SIZE_MAX && groups[group_index].conflict &&
                plan.items[group_index].reason == OD_ALLOC_REASSIGNED) {
                ++resolution->count;
            }
        }
        if (resolution->count > 0U) {
            resolution->items = calloc(resolution->count, sizeof(*resolution->items));
            if (resolution->items == NULL) {
                status = OD_ERROR_MEMORY;
                od_error_set(error, status, "unable to store conflict proposals");
            }
        }
    }
    size_t output = 0U;
    for (size_t index = 0U; index < discovery->count && status == OD_OK; ++index) {
        size_t group_index = group_for[index];
        if (group_index == SIZE_MAX || !groups[group_index].conflict ||
            plan.items[group_index].reason != OD_ALLOC_REASSIGNED) continue;
        status = copy_resolution_item(&resolution->items[output], index, group_index,
                                      &discovery->items[index],
                                      plan.items[group_index].new_port, error);
        if (status == OD_OK) {
            if (resolution->items[output].automatic) {
                ++resolution->automatic_count;
            } else {
                ++resolution->manual_count;
            }
            ++output;
        }
    }
    od_allocation_plan_free(&plan);
    od_assignments_free(&wanted);
    free(must_reassign);
    free(occupied);
    free(group_for);
    free(group_by_key);
    free(groups);
    free(canonical_root);
    if (status != OD_OK) {
        od_resolution_free(resolution);
        return status;
    }
    od_error_clear(error);
    return OD_OK;
}

OdStatus od_resolution_validate_snapshot(const char *project_root,
                                         const OdResolution *resolution,
                                         const OdScanSnapshot *snapshot,
                                         OdError *error) {
    if (project_root == NULL || resolution == NULL || snapshot == NULL ||
        (resolution->count > 0U && resolution->items == NULL) ||
        (snapshot->endpoint_count > 0U && snapshot->endpoints == NULL)) {
        od_error_set(error, OD_ERROR_INVALID,
                     "resolution and fresh scan snapshot are required");
        return OD_ERROR_INVALID;
    }
    char *canonical_root = realpath(project_root, NULL);
    if (canonical_root == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "unable to resolve conflict project root");
        return OD_ERROR_INVALID;
    }
    for (size_t item_index = 0U; item_index < resolution->count; ++item_index) {
        const OdResolutionItem *item = &resolution->items[item_index];
        for (size_t endpoint_index = 0U; endpoint_index < snapshot->endpoint_count;
             ++endpoint_index) {
            if (snapshot->endpoints[endpoint_index].local_port == item->new_port) {
                free(canonical_root);
                od_error_set(error, OD_ERROR_CHANGED,
                             "proposed port %u became occupied; refresh conflicts",
                             (unsigned)item->new_port);
                return OD_ERROR_CHANGED;
            }
        }
        if (!port_has_external_owner(canonical_root, snapshot, item->old_port)) {
            free(canonical_root);
            od_error_set(error, OD_ERROR_CHANGED,
                         "the conflict on port %u changed; refresh conflicts",
                         (unsigned)item->old_port);
            return OD_ERROR_CHANGED;
        }
    }
    free(canonical_root);
    od_error_clear(error);
    return OD_OK;
}
