#include "opendoor/allocation.h"
#include "opendoor/model.h"
#include "opendoor/persistence.h"
#include "opendoor/resolution.h"
#include "opendoor/scan.h"
#include "opendoor/screens.h"
#include "opendoor/ui.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static char *duplicate(const char *value) {
    size_t length = strlen(value) + 1U;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

static void make_profile(OdProfile *profile, uint16_t first_port, OdError *error) {
    od_profile_init(profile);
    profile->project_name = duplicate("Persistence test");
    profile->assignment_file = duplicate(".ports.env");
    profile->port_min = 1024U;
    profile->port_max = 65535U;
    OdService first = {
        .id = "api", .name = "API", .group = "backend", .variable = "API_PORT",
        .preferred_port = first_port, .protocols = OD_PROTOCOL_TCP, .managed = true
    };
    OdService second = {
        .id = "web", .name = "Web", .group = "frontend", .variable = "WEB_PORT",
        .preferred_port = 4100U, .protocols = OD_PROTOCOL_TCP, .managed = true
    };
    CHECK(od_profile_add_service(profile, &first, error) == OD_OK);
    CHECK(od_profile_add_service(profile, &second, error) == OD_OK);
}

static void make_plan(OdAllocationPlan *plan, uint16_t first_port) {
    *plan = (OdAllocationPlan){0};
    plan->items = calloc(2U, sizeof(*plan->items));
    plan->count = 2U;
    plan->items[0].service_id = duplicate("api");
    plan->items[0].variable = duplicate("API_PORT");
    plan->items[0].old_port = 4000U;
    plan->items[0].new_port = first_port;
    plan->items[0].reason = OD_ALLOC_REASSIGNED;
    plan->items[1].service_id = duplicate("web");
    plan->items[1].variable = duplicate("WEB_PORT");
    plan->items[1].new_port = 4100U;
    plan->items[1].reason = OD_ALLOC_PREFERRED;
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    CHECK(fseek(file, 0L, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length >= 0L);
    CHECK(fseek(file, 0L, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1U);
    if (text != NULL) {
        CHECK(fread(text, 1U, (size_t)length, file) == (size_t)length);
        text[length] = '\0';
    }
    CHECK(fclose(file) == 0);
    return text;
}

static bool write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool okay = fwrite(text, 1U, strlen(text), file) == strlen(text);
    return fclose(file) == 0 && okay;
}

static void cleanup_tree(const char *root) {
    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/.ports.env.opendoor.bak", root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.ports.env", root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.opendoor/project.toml.opendoor.bak", root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.opendoor/project.toml", root);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.opendoor", root);
    (void)rmdir(path);
    (void)rmdir(root);
}

static void test_resolution_and_snapshot_validation(void) {
    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    OdScanSnapshot snapshot;
    od_scan_snapshot_init(&snapshot, 1U);
    snapshot.endpoints = calloc(1U, sizeof(*snapshot.endpoints));
    snapshot.endpoint_count = 1U;
    snapshot.endpoints[0].local_port = 4000U;
    snapshot.endpoints[0].protocol = OD_PROTOCOL_TCP;
    (void)strcpy(snapshot.endpoints[0].process, "foreign-api");
    snapshot.endpoints[0].pid = 99;

    OdResolution resolution;
    CHECK(od_resolution_init(&resolution, &profile, &snapshot, &plan, &error) == OD_OK);
    CHECK(resolution.count == 1U);
    const OdResolutionItem *item = od_resolution_current(&resolution);
    CHECK(item != NULL && strstr(item->owner, "foreign-api") != NULL);
    CHECK(od_resolution_edit(&resolution, 4000U, &error) == OD_ERROR_CONFLICT);
    CHECK(od_resolution_edit(&resolution, 4002U, &error) == OD_OK);
    CHECK(plan.items[0].new_port == 4002U);
    OdCanvas canvas;
    CHECK(od_canvas_init(&canvas, 70U, 22U, &error) == OD_OK);
    od_render_conflict_resolution(&canvas, &resolution, true, "Awaiting decision");
    char *rendered = od_canvas_to_text(&canvas, &error);
    CHECK(rendered != NULL && strstr(rendered, "Resolve conflict") != NULL);
    CHECK(rendered != NULL && strstr(rendered, "Awaiting decision") != NULL);
    free(rendered);
    od_canvas_free(&canvas);
    od_resolution_accept(&resolution);
    CHECK(od_resolution_done(&resolution));

    CHECK(od_canvas_init(&canvas, 70U, 22U, &error) == OD_OK);
    od_render_change_review(&canvas, &profile, &plan, 0U, true, "Ready to save");
    rendered = od_canvas_to_text(&canvas, &error);
    CHECK(rendered != NULL && strstr(rendered, "Review assignment changes") != NULL);
    CHECK(rendered != NULL && strstr(rendered, "PgUp/PgDn Page") != NULL);
    free(rendered);
    od_canvas_free(&canvas);

    CHECK(od_plan_validate_snapshot(&profile, &plan, &snapshot, &error) == OD_OK);
    snapshot.endpoints[0].local_port = 4002U;
    CHECK(od_plan_validate_snapshot(&profile, &plan, &snapshot, &error) == OD_ERROR_CHANGED);
    od_resolution_free(&resolution);
    od_scan_snapshot_free(&snapshot);
    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
}

static void test_bind_probe_detects_changed_port(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        puts("bind probe check skipped: socket creation is restricted");
        return;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = (socklen_t)sizeof(address);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    uint16_t port = ntohs(address.sin_port);

    OdError error;
    OdProfile profile;
    make_profile(&profile, port, &error);
    OdAllocationPlan plan;
    make_plan(&plan, port);
    CHECK(od_plan_probe_bindings(&profile, &plan, &error) == OD_ERROR_CHANGED);
    CHECK(close(listener) == 0);
    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
}

static void test_atomic_save_backup_foreign_refusal_and_reset(void) {
    char template[] = "/tmp/opendoor-persistence-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;
    char profile_path[1024];
    char assignment_path[1024];
    (void)snprintf(profile_path, sizeof(profile_path), "%s/.opendoor/project.toml", root);
    (void)snprintf(assignment_path, sizeof(assignment_path), "%s/.ports.env", root);

    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) == OD_OK);
    char *assignments = read_text(assignment_path);
    CHECK(assignments != NULL);
    if (assignments != NULL) {
        CHECK(strstr(assignments, "PORTS_CONFIGURED=1") != NULL);
        CHECK(strstr(assignments, "API_PORT=4001") != NULL);
        free(assignments);
    }

    plan.items[0].new_port = 4002U;
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) == OD_OK);
    char backup_path[1024];
    (void)snprintf(backup_path, sizeof(backup_path), "%s/.ports.env.opendoor.bak", root);
    char *backup = read_text(backup_path);
    CHECK(backup != NULL && strstr(backup, "API_PORT=4001") != NULL);
    free(backup);

    CHECK(write_text(assignment_path, "SOME_PORT=5000\n"));
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) == OD_ERROR_FOREIGN);
    char *foreign = read_text(assignment_path);
    CHECK(foreign != NULL && strcmp(foreign, "SOME_PORT=5000\n") == 0);
    free(foreign);

    OdAssignments imported;
    CHECK(od_assignments_import_file(assignment_path, &imported, &error) == OD_OK);
    CHECK(imported.count == 1U);
    od_assignments_free(&imported);
    CHECK(od_project_save_importing_foreign(root, profile_path, &profile, &plan,
                                            &error) == OD_OK);

    CHECK(write_text(assignment_path,
                     "PORTS_CONFIGURED=1\nOPENDOOR_CONFIGURED=1\nAPI_PORT=4002\n"));
    CHECK(od_project_reset_assignments(root, &profile, &error) == OD_OK);
    CHECK(access(assignment_path, F_OK) != 0);
    CHECK(access(backup_path, F_OK) == 0);

    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
    cleanup_tree(root);
}

static void test_save_preflights_every_target_before_writing(void) {
    char template[] = "/tmp/opendoor-preflight-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;

    char profile_path[1024];
    char assignment_path[1024];
    char backup_path[1024];
    char outside_path[1024];
    (void)snprintf(profile_path, sizeof(profile_path), "%s/.opendoor/project.toml", root);
    (void)snprintf(assignment_path, sizeof(assignment_path), "%s/.ports.env", root);
    (void)snprintf(backup_path, sizeof(backup_path), "%s/.ports.env.opendoor.bak", root);
    (void)snprintf(outside_path, sizeof(outside_path), "%s/outside", root);
    CHECK(write_text(assignment_path,
                     "PORTS_CONFIGURED=1\nOPENDOOR_CONFIGURED=1\nAPI_PORT=4000\n"));
    CHECK(write_text(outside_path, "must remain unchanged\n"));
    CHECK(symlink(outside_path, backup_path) == 0);

    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) ==
          OD_ERROR_INVALID);
    CHECK(access(profile_path, F_OK) != 0);
    char *outside = read_text(outside_path);
    CHECK(outside != NULL && strcmp(outside, "must remain unchanged\n") == 0);
    free(outside);

    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
    (void)unlink(backup_path);
    (void)unlink(outside_path);
    (void)unlink(assignment_path);
    (void)rmdir(root);
}

static void test_assignment_parent_cannot_escape_project(void) {
    char root_template[] = "/tmp/opendoor-confined-XXXXXX";
    char outside_template[] = "/tmp/opendoor-outside-XXXXXX";
    char *root = mkdtemp(root_template);
    char *outside = mkdtemp(outside_template);
    CHECK(root != NULL && outside != NULL);
    if (root == NULL || outside == NULL) return;

    char link_path[1024];
    char escaped_path[1024];
    char profile_path[1024];
    (void)snprintf(link_path, sizeof(link_path), "%s/link", root);
    (void)snprintf(escaped_path, sizeof(escaped_path), "%s/.ports.env", outside);
    (void)snprintf(profile_path, sizeof(profile_path), "%s/.opendoor/project.toml", root);
    CHECK(symlink(outside, link_path) == 0);

    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    free(profile.assignment_file);
    profile.assignment_file = strdup("link/.ports.env");
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) ==
          OD_ERROR_INVALID);
    CHECK(access(escaped_path, F_OK) != 0);
    CHECK(access(profile_path, F_OK) != 0);

    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
    (void)unlink(link_path);
    (void)rmdir(outside);
    (void)rmdir(root);
}

static void test_safe_nested_assignment_parent_is_prepared_before_save(void) {
    char template[] = "/tmp/opendoor-nested-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;

    char profile_path[1024];
    char assignment_path[1024];
    (void)snprintf(profile_path, sizeof(profile_path), "%s/.opendoor/project.toml", root);
    (void)snprintf(assignment_path, sizeof(assignment_path), "%s/state/.ports.env", root);
    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    free(profile.assignment_file);
    profile.assignment_file = strdup("state/.ports.env");
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) == OD_OK);
    CHECK(access(profile_path, F_OK) == 0);
    CHECK(access(assignment_path, F_OK) == 0);

    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
    (void)unlink(assignment_path);
    char state_path[1024];
    (void)snprintf(state_path, sizeof(state_path), "%s/state", root);
    (void)rmdir(state_path);
    cleanup_tree(root);
}

static void test_second_replace_failure_rolls_back_first_target(void) {
    if (geteuid() == 0) {
        puts("transaction permission-failure check skipped as root");
        return;
    }
    char template[] = "/tmp/opendoor-rollback-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;
    char profile_dir[1024];
    char profile_path[1024];
    char state_dir[1024];
    char assignment_path[1024];
    (void)snprintf(profile_dir, sizeof(profile_dir), "%s/.opendoor", root);
    (void)snprintf(profile_path, sizeof(profile_path),
                   "%s/.opendoor/project.toml", root);
    (void)snprintf(state_dir, sizeof(state_dir), "%s/state", root);
    (void)snprintf(assignment_path, sizeof(assignment_path),
                   "%s/state/.ports.env", root);
    CHECK(mkdir(profile_dir, 0700) == 0);
    CHECK(mkdir(state_dir, 0500) == 0);
    CHECK(write_text(profile_path, "original profile bytes\n"));

    OdError error;
    OdProfile profile;
    make_profile(&profile, 4000U, &error);
    free(profile.assignment_file);
    profile.assignment_file = strdup("state/.ports.env");
    OdAllocationPlan plan;
    make_plan(&plan, 4001U);
    CHECK(od_project_save(root, profile_path, &profile, &plan, &error) == OD_ERROR_IO);
    char *restored = read_text(profile_path);
    CHECK(restored != NULL && strcmp(restored, "original profile bytes\n") == 0);
    CHECK(access(assignment_path, F_OK) != 0);
    free(restored);

    od_allocation_plan_free(&plan);
    od_profile_free(&profile);
    char backup_path[1024];
    (void)snprintf(backup_path, sizeof(backup_path),
                   "%s/.opendoor/project.toml.opendoor.bak", root);
    (void)unlink(backup_path);
    (void)unlink(profile_path);
    CHECK(chmod(state_dir, 0700) == 0);
    (void)rmdir(state_dir);
    (void)rmdir(profile_dir);
    (void)rmdir(root);
}

static void test_assignment_ignore_detection_is_read_only(void) {
    char template[] = "/tmp/opendoor-ignore-XXXXXX";
    char *root = mkdtemp(template);
    CHECK(root != NULL);
    if (root == NULL) return;
    char ignore_path[1024];
    (void)snprintf(ignore_path, sizeof(ignore_path), "%s/.gitignore", root);
    CHECK(write_text(ignore_path, "*.env\n!important.env\nstate/.ports.local\n"));
    CHECK(od_assignment_appears_ignored(root, ".ports.env"));
    CHECK(!od_assignment_appears_ignored(root, "important.env"));
    CHECK(od_assignment_appears_ignored(root, "state/.ports.local"));
    char *unchanged = read_text(ignore_path);
    CHECK(unchanged != NULL &&
          strcmp(unchanged, "*.env\n!important.env\nstate/.ports.local\n") == 0);
    free(unchanged);
    (void)unlink(ignore_path);
    (void)rmdir(root);
}

int main(void) {
    test_resolution_and_snapshot_validation();
    test_bind_probe_detects_changed_port();
    test_atomic_save_backup_foreign_refusal_and_reset();
    test_save_preflights_every_target_before_writing();
    test_assignment_parent_cannot_escape_project();
    test_safe_nested_assignment_parent_is_prepared_before_save();
    test_second_replace_failure_rolls_back_first_target();
    test_assignment_ignore_detection_is_read_only();
    if (failures != 0) {
        fprintf(stderr, "%d persistence checks failed\n", failures);
        return 1;
    }
    puts("persistence checks passed");
    return 0;
}
