#include "opendoor/allocation.h"
#include "opendoor/config.h"
#include "opendoor/resolution.h"

#include <stdio.h>
#include <string.h>

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

int main(void) {
    test_conflicts_receive_whole_plan_proposals();
    test_duplicate_wanted_ports_are_resolved();
    test_replacement_range_has_a_1024_floor_and_wraps();
    test_no_conflicts_produces_empty_resolution();
    if (failures != 0) {
        fprintf(stderr, "%d allocation/resolution checks failed\n", failures);
        return 1;
    }
    puts("allocation/resolution checks passed");
    return 0;
}
