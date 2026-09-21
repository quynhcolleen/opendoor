#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/dotenv.h"
#include "opendoor/model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static const char *valid_profile =
    "schema_version = 1\n"
    "project_name = \"DSVN\"\n"
    "assignment_file = \".ports.env\"\n"
    "port_min = 3000\n"
    "port_max = 3010\n"
    "[discovery]\n"
    "compose_files = [\"BACKEND/docker-compose.yaml\"]\n"
    "env_files = []\n"
    "package_files = [\"FRONTEND/package.json\"]\n"
    "[[service]]\n"
    "id = \"user-service\"\n"
    "name = \"User service\"\n"
    "group = \"backend\"\n"
    "variable = \"USER_HOST_PORT\"\n"
    "preferred_port = 3000\n"
    "protocols = [\"tcp\"]\n"
    "sources = [\"compose:BACKEND/docker-compose.yaml#user-app\"]\n";

static void test_profile_parsing(void) {
    OdProfile profile;
    OdError error;
    OdStatus status = od_profile_parse(valid_profile, strlen(valid_profile), &profile, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        return;
    }
    CHECK(profile.schema_version == 1U);
    CHECK(strcmp(profile.project_name, "DSVN") == 0);
    CHECK(profile.port_min == 3000U);
    CHECK(profile.service_count == 1U);
    CHECK(strcmp(profile.services[0].variable, "USER_HOST_PORT") == 0);
    CHECK(profile.services[0].protocols == OD_PROTOCOL_TCP);
    CHECK(profile.services[0].sources.count == 1U);
    char *rendered = NULL;
    size_t rendered_length = 0U;
    CHECK(od_profile_render(&profile, &rendered, &rendered_length, &error) == OD_OK);
    if (rendered != NULL) {
        OdProfile round_trip;
        CHECK(od_profile_parse(rendered, rendered_length, &round_trip, &error) == OD_OK);
        CHECK(round_trip.service_count == profile.service_count);
        CHECK(strcmp(round_trip.services[0].id, profile.services[0].id) == 0);
        od_profile_free(&round_trip);
        free(rendered);
    }
    od_profile_free(&profile);
}

static void test_profile_rejects_invalid_documents(void) {
    const char *unsupported = "schema_version = 2\nproject_name = \"x\"\nassignment_file = \".ports.env\"\nport_min = 3000\nport_max = 3010\n";
    const char *malformed = "schema_version = [ definitely not valid\n";
    const char *duplicate =
        "schema_version = 1\nproject_name = \"x\"\nassignment_file = \".ports.env\"\nport_min = 3000\nport_max = 3010\n"
        "[[service]]\nid = \"same\"\nname = \"one\"\ngroup = \"g\"\nvariable = \"ONE_PORT\"\npreferred_port = 3000\nprotocols = [\"tcp\"]\nsources=[]\n"
        "[[service]]\nid = \"same\"\nname = \"two\"\ngroup = \"g\"\nvariable = \"TWO_PORT\"\npreferred_port = 3001\nprotocols = [\"tcp\"]\nsources=[]\n";
    const char *duplicate_variable =
        "schema_version = 1\nproject_name = \"x\"\nassignment_file = \".ports.env\"\nport_min = 3000\nport_max = 3010\n"
        "[[service]]\nid = \"one\"\nname = \"one\"\ngroup = \"g\"\nvariable = \"SAME_PORT\"\npreferred_port = 3000\nprotocols = [\"tcp\"]\nsources=[]\n"
        "[[service]]\nid = \"two\"\nname = \"two\"\ngroup = \"g\"\nvariable = \"SAME_PORT\"\npreferred_port = 3001\nprotocols = [\"tcp\"]\nsources=[]\n";
    OdProfile profile;
    OdError error;

    CHECK(od_profile_parse(unsupported, strlen(unsupported), &profile, &error) == OD_ERROR_UNSUPPORTED);
    CHECK(od_profile_parse(malformed, strlen(malformed), &profile, &error) == OD_ERROR_INVALID);
    CHECK(od_profile_parse(duplicate, strlen(duplicate), &profile, &error) == OD_ERROR_INVALID);
    CHECK(strstr(error.message, "duplicate service id") != NULL);
    CHECK(od_profile_parse(duplicate_variable, strlen(duplicate_variable), &profile, &error) == OD_ERROR_INVALID);
    CHECK(strstr(error.message, "duplicate variable") != NULL);

    const char *unsafe_assignment =
        "schema_version = 1\nproject_name = \"x\"\nassignment_file = \"../ports.env\"\n"
        "port_min = 3000\nport_max = 3010\n";
    CHECK(od_profile_parse(unsafe_assignment, strlen(unsafe_assignment),
                           &profile, &error) == OD_ERROR_INVALID);
    CHECK(strstr(error.message, "safe project-relative") != NULL);
}

static void test_settings(void) {
    const char *text =
        "schema_version = 1\n"
        "theme = \"nord\"\n"
        "unicode = \"never\"\n"
        "reduced_motion = true\n"
        "mouse = false\n"
        "auto_refresh = true\n"
        "refresh_seconds = 12\n";
    OdSettings settings;
    OdError error;
    OdStatus status = od_settings_parse(text, strlen(text), &settings, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        return;
    }
    CHECK(strcmp(settings.theme, "nord") == 0);
    CHECK(settings.unicode_mode == OD_UNICODE_NEVER);
    CHECK(settings.reduced_motion);
    CHECK(!settings.mouse);
    CHECK(settings.auto_refresh);
    CHECK(settings.refresh_seconds == 12U);
}

static void test_dotenv_contract(void) {
    const char *compatible =
        "# Generated by OpenDoor. Machine-local; do not commit.\n"
        "PORTS_CONFIGURED=1\n"
        "OPENDOOR_CONFIGURED=1\n"
        "USER_HOST_PORT=3000\n";
    const char *foreign = "USER_HOST_PORT=3000\n";
    OdAssignments assignments;
    OdError error;
    char *rendered = NULL;
    size_t rendered_length = 0U;

    OdStatus status = od_assignments_parse(compatible, strlen(compatible), &assignments, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        return;
    }
    CHECK(assignments.compatible_marker);
    CHECK(assignments.opendoor_marker);
    CHECK(assignments.count == 1U);
    CHECK(od_assignments_find(&assignments, "USER_HOST_PORT")->port == 3000U);
    CHECK(od_assignments_render(&assignments, ".opendoor/project.toml", &rendered, &rendered_length, &error) == OD_OK);
    CHECK(rendered_length == strlen(rendered));
    CHECK(strstr(rendered, "PORTS_CONFIGURED=1") != NULL);
    CHECK(strstr(rendered, "OPENDOOR_CONFIGURED=1") != NULL);
    CHECK(strstr(rendered, "USER_HOST_PORT=3000") != NULL);
    free(rendered);
    od_assignments_free(&assignments);

    CHECK(od_assignments_parse(foreign, strlen(foreign), &assignments, &error) == OD_ERROR_FOREIGN);
}

static OdService make_service(const char *id, const char *variable, uint16_t preferred) {
    OdService service = {0};
    service.id = (char *)id;
    service.name = (char *)id;
    service.group = "test";
    service.variable = (char *)variable;
    service.preferred_port = preferred;
    service.protocols = OD_PROTOCOL_TCP;
    service.managed = true;
    return service;
}

static void test_allocation(void) {
    OdProfile profile;
    OdError error;
    od_profile_init(&profile);
    profile.project_name = strdup("allocation");
    profile.assignment_file = strdup(".ports.env");
    profile.port_min = 3000U;
    profile.port_max = 3005U;
    OdService first = make_service("first", "FIRST_PORT", 3000U);
    OdService second = make_service("second", "SECOND_PORT", 3001U);
    CHECK(od_profile_add_service(&profile, &first, &error) == OD_OK);
    CHECK(od_profile_add_service(&profile, &second, &error) == OD_OK);

    OdOccupiedPort occupied[] = {{3000U, OD_PROTOCOL_UDP}};
    OdAssignments saved;
    od_assignments_init(&saved);
    saved.items = calloc(1U, sizeof(*saved.items));
    saved.count = 1U;
    saved.items[0].variable = strdup("SECOND_PORT");
    saved.items[0].port = 3004U;
    OdAllocationPlan plan = {0};

    OdStatus status = od_allocate(&profile, occupied, 1U, &saved, &plan, &error);
    CHECK(status == OD_OK);
    if (status != OD_OK) {
        od_assignments_free(&saved);
        od_profile_free(&profile);
        return;
    }
    CHECK(plan.count == 2U);
    CHECK(plan.items[0].new_port == 3002U);
    CHECK(plan.items[0].reason == OD_ALLOC_REASSIGNED);
    CHECK(plan.items[1].new_port == 3004U);
    CHECK(plan.items[1].reason == OD_ALLOC_PRESERVED);
    od_allocation_plan_free(&plan);
    od_assignments_free(&saved);
    od_profile_free(&profile);
}

static void test_conflicting_saved_port_is_a_reassignment(void) {
    OdProfile profile;
    OdError error;
    od_profile_init(&profile);
    profile.project_name = strdup("saved conflict");
    profile.assignment_file = strdup(".ports.env");
    profile.port_min = 3000U;
    profile.port_max = 3010U;
    OdService service = make_service("api", "API_PORT", 3000U);
    CHECK(od_profile_add_service(&profile, &service, &error) == OD_OK);

    OdAssignments saved;
    od_assignments_init(&saved);
    saved.items = calloc(1U, sizeof(*saved.items));
    saved.count = 1U;
    saved.items[0].variable = strdup("API_PORT");
    saved.items[0].port = 3001U;
    OdOccupiedPort occupied = {3001U, OD_PROTOCOL_TCP};
    OdAllocationPlan plan = {0};
    CHECK(od_allocate(&profile, &occupied, 1U, &saved, &plan, &error) == OD_OK);
    CHECK(plan.count == 1U);
    CHECK(plan.items[0].old_port == 3001U);
    CHECK(plan.items[0].new_port == 3000U);
    CHECK(plan.items[0].reason == OD_ALLOC_REASSIGNED);

    od_allocation_plan_free(&plan);
    od_assignments_free(&saved);
    od_profile_free(&profile);
}

static void test_allocation_exhaustion(void) {
    OdProfile profile;
    OdError error;
    od_profile_init(&profile);
    profile.project_name = strdup("full");
    profile.assignment_file = strdup(".ports.env");
    profile.port_min = 4000U;
    profile.port_max = 4000U;
    OdService service = make_service("only", "ONLY_PORT", 4000U);
    CHECK(od_profile_add_service(&profile, &service, &error) == OD_OK);
    OdOccupiedPort occupied = {4000U, OD_PROTOCOL_TCP};
    OdAllocationPlan plan = {0};
    CHECK(od_allocate(&profile, &occupied, 1U, NULL, &plan, &error) == OD_ERROR_EXHAUSTED);
    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
}

static void test_allocation_preferred_reserved_duplicate_and_stale(void) {
    OdProfile profile;
    OdError error;
    od_profile_init(&profile);
    profile.project_name = strdup("matrix");
    profile.assignment_file = strdup(".ports.env");
    profile.port_min = 5000U;
    profile.port_max = 5005U;
    OdService first = make_service("first", "FIRST_PORT", 5000U);
    OdService second = make_service("second", "SECOND_PORT", 5000U);
    CHECK(od_profile_add_service(&profile, &first, &error) == OD_OK);
    CHECK(od_profile_add_service(&profile, &second, &error) == OD_OK);

    OdAssignments saved;
    od_assignments_init(&saved);
    saved.items = calloc(3U, sizeof(*saved.items));
    saved.count = 3U;
    saved.items[0] = (OdAssignment){strdup("FIRST_PORT"), 4999U};
    saved.items[1] = (OdAssignment){strdup("SECOND_PORT"), 5001U};
    saved.items[2] = (OdAssignment){strdup("LEGACY_PORT"), 5002U};
    OdOccupiedPort occupied = {5001U, OD_PROTOCOL_TCP | OD_PROTOCOL_UDP};
    OdAllocationPlan plan = {0};

    CHECK(od_allocate(&profile, &occupied, 1U, &saved, &plan, &error) == OD_OK);
    CHECK(plan.count == 2U);
    CHECK(plan.items[0].old_port == 4999U);
    CHECK(plan.items[0].new_port == 5000U);
    CHECK(plan.items[0].reason == OD_ALLOC_REASSIGNED);
    CHECK(plan.items[1].old_port == 5001U);
    CHECK(plan.items[1].new_port == 5003U);
    CHECK(plan.items[1].reason == OD_ALLOC_REASSIGNED);

    od_allocation_plan_free(&plan);
    od_assignments_free(&saved);
    od_profile_free(&profile);
}

static void test_large_congested_allocation_stays_interactive(void) {
    enum { service_count = 1024 };
    OdProfile profile;
    OdError error;
    od_profile_init(&profile);
    profile.project_name = strdup("large allocation");
    profile.assignment_file = strdup(".ports.env");
    profile.port_min = 10000U;
    profile.port_max = (uint16_t)(10000U + service_count * 2U - 1U);
    OdOccupiedPort *occupied = calloc(service_count, sizeof(*occupied));
    CHECK(occupied != NULL);
    if (occupied == NULL) {
        od_profile_free(&profile);
        return;
    }
    for (size_t index = 0U; index < service_count; ++index) {
        char id[48];
        char variable[48];
        (void)snprintf(id, sizeof(id), "service-%04zu", index);
        (void)snprintf(variable, sizeof(variable), "SERVICE_%04zu_PORT", index);
        OdService service = make_service(id, variable,
                                         (uint16_t)(10000U + index));
        CHECK(od_profile_add_service(&profile, &service, &error) == OD_OK);
        occupied[index] = (OdOccupiedPort){(uint16_t)(10000U + index),
                                           OD_PROTOCOL_TCP};
    }

    struct timespec started;
    struct timespec finished;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    OdAllocationPlan plan = {0};
    CHECK(od_allocate(&profile, occupied, service_count, NULL, &plan, &error) == OD_OK);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
    double elapsed = (double)(finished.tv_sec - started.tv_sec) +
                     (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    CHECK(elapsed < 2.0);
    CHECK(plan.count == service_count);
    CHECK(plan.items[0].new_port == 11024U);
    CHECK(plan.items[service_count - 1U].new_port == 12047U);

    od_allocation_plan_free(&plan);
    free(occupied);
    od_profile_free(&profile);
}

int main(void) {
    test_profile_parsing();
    test_profile_rejects_invalid_documents();
    test_settings();
    test_dotenv_contract();
    test_allocation();
    test_conflicting_saved_port_is_a_reassignment();
    test_allocation_exhaustion();
    test_allocation_preferred_reserved_duplicate_and_stale();
    test_large_congested_allocation_stays_interactive();
    if (failures != 0) {
        fprintf(stderr, "%d core checks failed\n", failures);
        return 1;
    }
    puts("core checks passed");
    return 0;
}
