#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/resolution.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static OdAssignments parse(const char *text) {
    OdAssignments assignments;
    OdError error;
    CHECK(od_config_parse(text, strlen(text), &assignments, &error) == OD_OK);
    return assignments;
}

static void make_directory(const char *path) {
    CHECK(mkdir(path, 0700) == 0);
}

static const OdResolutionItem *find_resolution_item(const OdResolution *resolution,
                                                    size_t declaration_index) {
    for (size_t index = 0U; index < resolution->count; ++index) {
        if (resolution->items[index].declaration_index == declaration_index) {
            return &resolution->items[index];
        }
    }
    return NULL;
}

static bool contains_port(const uint16_t *ports, size_t count, uint16_t port) {
    for (size_t index = 0U; index < count; ++index) {
        if (ports[index] == port) return true;
    }
    return false;
}

static void test_conflicts_receive_whole_plan_proposals(void) {
    OdAssignments wanted = parse("API_PORT=3000\nWEB_PORT=3001\n");
    const uint16_t occupied[] = {3000U, 3002U};
    OdAllocationPlan plan;
    OdResolution resolution;
    OdError error;

    CHECK(od_allocate(&wanted, occupied, 2U, &plan, &error) == OD_OK);
    CHECK(plan.count == 2U);
    if (plan.count == 2U) {
        CHECK(strcmp(plan.items[0].variable, "API_PORT") == 0);
        CHECK(plan.items[0].old_port == 3000U);
        CHECK(plan.items[0].new_port == 3003U);
        CHECK(plan.items[0].reason == OD_ALLOC_REASSIGNED);
        CHECK(plan.items[1].old_port == 3001U);
        CHECK(plan.items[1].new_port == 3001U);
        CHECK(plan.items[1].reason == OD_ALLOC_UNCHANGED);
    }

    CHECK(od_resolution_init(&resolution, &plan, &error) == OD_OK);
    CHECK(resolution.count == 1U);
    if (resolution.count == 1U) {
        CHECK(strcmp(resolution.items[0].variable, "API_PORT") == 0);
        CHECK(resolution.items[0].old_port == 3000U);
        CHECK(resolution.items[0].new_port == 3003U);
    }
    od_resolution_free(&resolution);
    od_allocation_plan_free(&plan);
    od_assignments_free(&wanted);
}

static void test_duplicate_wanted_ports_are_resolved(void) {
    OdAssignments wanted = parse("FIRST_PORT=4000\nSECOND_PORT=4000\n");
    OdAllocationPlan plan;
    OdResolution resolution;
    OdError error;

    CHECK(od_allocate(&wanted, NULL, 0U, &plan, &error) == OD_OK);
    CHECK(plan.count == 2U);
    if (plan.count == 2U) {
        CHECK(plan.items[0].new_port == 4000U);
        CHECK(plan.items[0].reason == OD_ALLOC_UNCHANGED);
        CHECK(plan.items[1].new_port == 4001U);
        CHECK(plan.items[1].reason == OD_ALLOC_REASSIGNED);
    }
    CHECK(od_resolution_init(&resolution, &plan, &error) == OD_OK);
    CHECK(resolution.count == 1U);
    od_resolution_free(&resolution);
    od_allocation_plan_free(&plan);
    od_assignments_free(&wanted);
}

static void test_replacement_range_has_a_1024_floor_and_wraps(void) {
    OdAssignments low = parse("HTTP_PORT=80\n");
    const uint16_t low_occupied[] = {80U};
    OdAllocationPlan plan;
    OdError error;
    CHECK(od_allocate(&low, low_occupied, 1U, &plan, &error) == OD_OK);
    CHECK(plan.count == 1U && plan.items[0].new_port == 1024U);
    od_allocation_plan_free(&plan);
    od_assignments_free(&low);

    OdAssignments high = parse("LAST_PORT=65535\n");
    const uint16_t high_occupied[] = {65535U};
    CHECK(od_allocate(&high, high_occupied, 1U, &plan, &error) == OD_OK);
    CHECK(plan.count == 1U && plan.items[0].new_port == 1024U);
    od_allocation_plan_free(&plan);
    od_assignments_free(&high);
}

static void test_no_conflicts_produces_empty_resolution(void) {
    OdAssignments wanted = parse("API_PORT=4100\n");
    OdAllocationPlan plan;
    OdResolution resolution;
    OdError error;
    CHECK(od_allocate(&wanted, NULL, 0U, &plan, &error) == OD_OK);
    CHECK(od_resolution_init(&resolution, &plan, &error) == OD_OK);
    CHECK(resolution.count == 0U);
    od_resolution_free(&resolution);
    od_allocation_plan_free(&plan);
    od_assignments_free(&wanted);
}

static void test_selective_allocation_preserves_unselected_occupied_ports(void) {
    OdAssignments wanted = parse(
        "PROJECT_PORT=3000\n"
        "EXTERNAL_PORT=3001\n"
        "LOW_PORT=80\n"
        "HIGH_PORT=65535\n");
    const uint16_t occupied[] = {80U, 1024U, 3000U, 3001U, 3002U, 65535U};
    const bool must_reassign[] = {false, true, true, true};
    OdAllocationPlan plan;
    OdError error;
    CHECK(od_allocate_selected(&wanted, occupied,
                               sizeof(occupied) / sizeof(occupied[0]),
                               must_reassign,
                               sizeof(must_reassign) / sizeof(must_reassign[0]),
                               &plan, &error) == OD_OK);
    CHECK(plan.count == 4U);
    if (plan.count == 4U) {
        CHECK(plan.items[0].new_port == 3000U);
        CHECK(plan.items[0].reason == OD_ALLOC_UNCHANGED);
        CHECK(plan.items[1].new_port == 3003U);
        CHECK(plan.items[2].new_port == 1025U);
        CHECK(plan.items[3].new_port == 1026U);
        for (size_t index = 1U; index < plan.count; ++index) {
            CHECK(!contains_port(occupied,
                                 sizeof(occupied) / sizeof(occupied[0]),
                                 plan.items[index].new_port));
        }
    }
    od_allocation_plan_free(&plan);
    od_assignments_free(&wanted);
}

static void test_source_aware_conflict_proposals_and_snapshot_validation(void) {
    char root[] = "/tmp/opendoor-resolution-project-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char service[512];
    char outside[512];
    (void)snprintf(service, sizeof(service), "%s/service", root);
    (void)snprintf(outside, sizeof(outside), "%s-outside", root);
    make_directory(service);
    make_directory(outside);
    char paths[9][512];
    static const char *const names[] = {
        ".env", "compose.yaml", "compose.yaml", "package.json", "Makefile",
        "service/.env", "service/.env.local", "service/compose.yaml", "service/.env"
    };
    for (size_t index = 0U; index < 9U; ++index) {
        (void)snprintf(paths[index], sizeof(paths[index]), "%s/%s", root, names[index]);
    }
    OdPortDeclaration declarations[] = {
        {
            .source_kind = OD_SOURCE_ENV,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_ENV_LITERAL,
            .port = 3000U,
            .line = 1U,
            .column = 10U,
            .byte_offset = 9U,
            .byte_length = 4U,
            .file_size = 14U,
            .file_hash = 11U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[0],
            .relative_path = "./.env",
            .relative_folder = "./",
            .environment_key = "API_PORT",
            .line_text = "API_PORT=3000",
            .manual_reason = ""
        },
        {
            .source_kind = OD_SOURCE_COMPOSE,
            .declaration_kind = OD_DECLARATION_ENV_REFERENCE,
            .write_kind = OD_WRITE_MANUAL_ONLY,
            .port = 3000U,
            .line = 4U,
            .column = 10U,
            .byte_offset = 30U,
            .byte_length = 11U,
            .file_size = 60U,
            .file_hash = 12U,
            .definition_index = 0U,
            .absolute_path = paths[1],
            .relative_path = "./compose.yaml",
            .relative_folder = "./",
            .environment_key = "API_PORT",
            .line_text = "      - \"${API_PORT}:80\"",
            .manual_reason = "environment reference is manual-only"
        },
        {
            .source_kind = OD_SOURCE_COMPOSE,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_COMPOSE_LITERAL,
            .port = 3100U,
            .line = 5U,
            .column = 10U,
            .byte_offset = 65U,
            .byte_length = 4U,
            .file_size = 90U,
            .file_hash = 12U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[2],
            .relative_path = "./compose.yaml",
            .relative_folder = "./",
            .environment_key = "",
            .line_text = "      - \"3100:81\"",
            .manual_reason = ""
        },
        {
            .source_kind = OD_SOURCE_PACKAGE_JSON,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_MANUAL_ONLY,
            .port = 3200U,
            .line = 2U,
            .column = 25U,
            .byte_offset = 40U,
            .byte_length = 4U,
            .file_size = 70U,
            .file_hash = 13U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[3],
            .relative_path = "./package.json",
            .relative_folder = "./",
            .environment_key = "",
            .line_text = "    \"dev\": \"vite --port 3200\"",
            .manual_reason = "package scripts are manual-only"
        },
        {
            .source_kind = OD_SOURCE_MAKEFILE,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_MANUAL_ONLY,
            .port = 3300U,
            .line = 1U,
            .column = 8U,
            .byte_offset = 7U,
            .byte_length = 4U,
            .file_size = 12U,
            .file_hash = 14U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[4],
            .relative_path = "./Makefile",
            .relative_folder = "./",
            .environment_key = "",
            .line_text = "PORT = 3300",
            .manual_reason = "Makefile declarations are manual-only"
        },
        {
            .source_kind = OD_SOURCE_ENV,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_ENV_LITERAL,
            .port = 3400U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[5],
            .relative_path = "./service/.env",
            .relative_folder = "./service",
            .environment_key = "PROJECT_PORT",
            .line_text = "PROJECT_PORT=3400",
            .manual_reason = ""
        },
        {
            .source_kind = OD_SOURCE_ENV,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_ENV_LITERAL,
            .port = 3500U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[6],
            .relative_path = "./service/.env.local",
            .relative_folder = "./service",
            .environment_key = "IDLE_PORT",
            .line_text = "IDLE_PORT=3500",
            .manual_reason = ""
        },
        {
            .source_kind = OD_SOURCE_COMPOSE,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_COMPOSE_LITERAL,
            .port = 3600U,
            .line = 4U,
            .column = 10U,
            .byte_length = 4U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[7],
            .relative_path = "./service/compose.yaml",
            .relative_folder = "./service",
            .environment_key = "",
            .line_text = "      - \"3600:80\"",
            .manual_reason = ""
        },
        {
            .source_kind = OD_SOURCE_ENV,
            .declaration_kind = OD_DECLARATION_LITERAL,
            .write_kind = OD_WRITE_ENV_LITERAL,
            .port = 3600U,
            .line = 2U,
            .column = 12U,
            .byte_length = 4U,
            .definition_index = SIZE_MAX,
            .absolute_path = paths[8],
            .relative_path = "./service/.env",
            .relative_folder = "./service",
            .environment_key = "OTHER_PORT",
            .line_text = "OTHER_PORT=3600",
            .manual_reason = ""
        }
    };
    OdProjectDiscovery discovery = {
        .items = declarations,
        .count = sizeof(declarations) / sizeof(declarations[0])
    };
    const uint16_t ports[] = {3000U, 3001U, 3100U, 3101U, 3200U, 3300U,
                              3400U, 3600U};
    OdEndpoint endpoints[8] = {0};
    for (size_t index = 0U; index < 8U; ++index) {
        endpoints[index].local_port = ports[index];
        endpoints[index].pid = (pid_t)(100 + index);
        (void)snprintf(endpoints[index].directory,
                       sizeof(endpoints[index].directory), "%s", outside);
    }
    (void)snprintf(endpoints[6].directory, sizeof(endpoints[6].directory),
                   "%s/service", root);
    endpoints[5].directory[0] = '\0';
    endpoints[5].executable[0] = '\0';
    OdScanSnapshot snapshot = {.endpoints = endpoints, .endpoint_count = 8U};
    OdResolution resolution;
    OdError error;
    CHECK(od_resolution_build(root, &discovery, &snapshot,
                              &resolution, &error) == OD_OK);
    CHECK(resolution.count == 7U);
    CHECK(resolution.automatic_count == 4U);
    CHECK(resolution.manual_count == 3U);
    const OdResolutionItem *definition = find_resolution_item(&resolution, 0U);
    const OdResolutionItem *reference = find_resolution_item(&resolution, 1U);
    const OdResolutionItem *compose = find_resolution_item(&resolution, 2U);
    const OdResolutionItem *package = find_resolution_item(&resolution, 3U);
    const OdResolutionItem *make = find_resolution_item(&resolution, 4U);
    const OdResolutionItem *duplicate_compose = find_resolution_item(&resolution, 7U);
    const OdResolutionItem *duplicate_env = find_resolution_item(&resolution, 8U);
    CHECK(definition != NULL && definition->automatic);
    CHECK(reference != NULL && !reference->automatic);
    if (definition != NULL && reference != NULL) {
        CHECK(definition->new_port == reference->new_port);
        CHECK(strstr(definition->line_before, "- API_PORT=3000") != NULL);
        CHECK(strstr(definition->line_after, "+ API_PORT=") != NULL);
    }
    CHECK(compose != NULL && compose->automatic &&
          compose->write_kind == OD_WRITE_COMPOSE_LITERAL);
    CHECK(package != NULL && !package->automatic &&
          package->source_kind == OD_SOURCE_PACKAGE_JSON);
    CHECK(make != NULL && !make->automatic &&
          make->source_kind == OD_SOURCE_MAKEFILE);
    CHECK(find_resolution_item(&resolution, 5U) == NULL);
    CHECK(find_resolution_item(&resolution, 6U) == NULL);
    CHECK(duplicate_compose != NULL && duplicate_env != NULL);
    if (duplicate_compose != NULL && duplicate_env != NULL) {
        CHECK(duplicate_compose->new_port != duplicate_env->new_port);
    }
    for (size_t index = 0U; index < resolution.count; ++index) {
        CHECK(!contains_port(ports, sizeof(ports) / sizeof(ports[0]),
                             resolution.items[index].new_port));
        CHECK(resolution.items[index].absolute_path != NULL);
        CHECK(resolution.items[index].relative_path[0] == '.');
    }
    CHECK(od_resolution_validate_snapshot(root, &resolution,
                                          &snapshot, &error) == OD_OK);
    if (resolution.count > 0U) {
        OdEndpoint changed_endpoint = {
            .local_port = resolution.items[0].new_port,
            .pid = 999
        };
        OdScanSnapshot changed = {
            .endpoints = &changed_endpoint,
            .endpoint_count = 1U
        };
        CHECK(od_resolution_validate_snapshot(root, &resolution, &changed,
                                              &error) == OD_ERROR_CHANGED);
    }
    OdScanSnapshot empty = {0};
    CHECK(od_resolution_validate_snapshot(root, &resolution, &empty,
                                          &error) == OD_ERROR_CHANGED);
    od_resolution_free(&resolution);

    OdEndpoint project_only = {.local_port = 3400U, .pid = 700};
    (void)snprintf(project_only.directory, sizeof(project_only.directory),
                   "%s/service", root);
    OdScanSnapshot project_snapshot = {
        .endpoints = &project_only,
        .endpoint_count = 1U
    };
    OdProjectDiscovery project_discovery = {
        .items = &declarations[5],
        .count = 1U
    };
    CHECK(od_resolution_build(root, &project_discovery, &project_snapshot,
                              &resolution, &error) == OD_OK);
    CHECK(resolution.count == 0U);
    od_resolution_free(&resolution);

    CHECK(rmdir(outside) == 0);
    CHECK(rmdir(service) == 0);
    CHECK(rmdir(root) == 0);
}

int main(void) {
    test_conflicts_receive_whole_plan_proposals();
    test_duplicate_wanted_ports_are_resolved();
    test_replacement_range_has_a_1024_floor_and_wraps();
    test_no_conflicts_produces_empty_resolution();
    test_selective_allocation_preserves_unselected_occupied_ports();
    test_source_aware_conflict_proposals_and_snapshot_validation();
    if (failures != 0) {
        fprintf(stderr, "%d allocation/resolution checks failed\n", failures);
        return 1;
    }
    puts("allocation/resolution checks passed");
    return 0;
}
