#include "opendoor/discovery.h"
#include "opendoor/onboarding.h"
#include "opendoor/screens.h"
#include "opendoor/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                                  \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static void make_candidates(OdCandidateList *candidates) {
    const char *dotenv =
        "API_PORT=3000\nAUTH_PORT=3001\nCACHE_PORT=3002\nDB_PORT=3003\n"
        "MAIL_PORT=3004\nMETRICS_PORT=3005\nSEARCH_PORT=3006\nWORKER_PORT=3007\n";
    const char *package =
        "{\"name\":\"web\",\"scripts\":{\"dev\":\"vite --port 5173\",\"preview\":\"vite --port 4173\"}}";
    OdCandidateList package_candidates;
    OdError error;
    od_candidate_list_init(candidates);
    od_candidate_list_init(&package_candidates);
    CHECK(od_discover_dotenv_text(".env", dotenv, candidates, &error) == OD_OK);
    CHECK(od_discover_package_json("package.json", package, &package_candidates, &error) == OD_OK);
    CHECK(od_candidates_merge(candidates, &package_candidates, &error) == OD_OK);
    od_candidate_list_free(&package_candidates);
}

static void test_review_pagination_and_profile(void) {
    OdCandidateList candidates;
    make_candidates(&candidates);
    OdOnboarding onboarding;
    OdError error;
    CHECK(od_onboarding_init(&onboarding, &candidates, 4U, 1024U, 65535U, &error) == OD_OK);
    CHECK(onboarding.candidates.count == 10U);
    CHECK(od_onboarding_page_count(&onboarding) == 3U);
    CHECK(od_onboarding_page(&onboarding) == 0U);
    od_onboarding_move_page(&onboarding, 1);
    CHECK(onboarding.selected == 4U);
    CHECK(od_onboarding_page_start(&onboarding) == 4U);
    od_onboarding_move(&onboarding, 3);
    CHECK(onboarding.selected == 7U);
    od_onboarding_move(&onboarding, 99);
    CHECK(onboarding.selected == 9U);
    CHECK(od_onboarding_page(&onboarding) == 2U);
    od_onboarding_move(&onboarding, -99);
    CHECK(onboarding.selected == 0U);

    bool initially_selected = onboarding.candidates.items[0].selected;
    od_onboarding_toggle_selected(&onboarding);
    CHECK(onboarding.candidates.items[0].selected != initially_selected);
    OdProfile profile;
    CHECK(od_onboarding_build_profile(&onboarding, "Demo", ".ports.env", &profile, &error) == OD_ERROR_CONFLICT);
    for (size_t index = 0U; index < onboarding.candidates.count; ++index) {
        onboarding.selected = index;
        od_onboarding_review_selected(&onboarding);
    }
    CHECK(od_onboarding_all_reviewed(&onboarding));
    OdStatus build_status = od_onboarding_build_profile(&onboarding, "Demo", ".ports.env", &profile, &error);
    CHECK(build_status == OD_OK);
    if (build_status != OD_OK) {
        od_onboarding_free(&onboarding);
        od_candidate_list_free(&candidates);
        return;
    }
    CHECK(profile.service_count == 7U);
    CHECK(profile.port_min == 1024U);
    CHECK(profile.port_max == 65535U);
    od_profile_free(&profile);
    od_onboarding_free(&onboarding);
    od_candidate_list_free(&candidates);
}

static void test_edit_and_manual_validation(void) {
    OdCandidateList candidates;
    make_candidates(&candidates);
    OdOnboarding onboarding;
    OdError error;
    CHECK(od_onboarding_init(&onboarding, &candidates, 5U, 2000U, 9000U, &error) == OD_OK);
    CHECK(od_onboarding_edit_selected(&onboarding, "API service", "backend",
                                      "API_HTTP_PORT", 8080U, &error) == OD_OK);
    CHECK(strcmp(onboarding.candidates.items[0].variable, "API_HTTP_PORT") == 0);
    CHECK(onboarding.reviewed[0]);
    OdStatus manual_status = od_onboarding_add_manual(&onboarding, "Admin", "backend", "ADMIN_PORT",
                                                      8081U, OD_PROTOCOL_TCP, &error);
    CHECK(manual_status == OD_OK);
    CHECK(onboarding.candidates.count == 11U);
    CHECK(onboarding.selected == 10U);
    if (manual_status == OD_OK) CHECK(!onboarding.reviewed[10]);
    CHECK(od_onboarding_add_manual(&onboarding, "Duplicate", "backend", "ADMIN_PORT",
                                   8082U, OD_PROTOCOL_TCP, &error) == OD_ERROR_INVALID);
    CHECK(od_onboarding_add_manual(&onboarding, "Bad", "backend", "bad-var",
                                   8082U, OD_PROTOCOL_TCP, &error) == OD_ERROR_INVALID);
    CHECK(od_onboarding_add_manual(&onboarding, "Range", "backend", "RANGE_PORT",
                                   9999U, OD_PROTOCOL_TCP, &error) == OD_ERROR_INVALID);
    od_onboarding_free(&onboarding);
    od_candidate_list_free(&candidates);
}

static void test_onboarding_snapshot_stays_in_viewport(void) {
    OdCandidateList candidates;
    make_candidates(&candidates);
    OdOnboarding onboarding;
    OdError error;
    CHECK(od_onboarding_init(&onboarding, &candidates, 5U, 1024U, 65535U, &error) == OD_OK);
    OdCanvas canvas;
    CHECK(od_canvas_init(&canvas, 90U, 22U, &error) == OD_OK);
    od_render_onboarding(&canvas, &onboarding, true,
                         "Review every candidate before saving.");
    char *text = od_canvas_to_text(&canvas, &error);
    CHECK(text != NULL);
    if (text != NULL) {
        size_t newlines = 0U;
        for (size_t index = 0U; text[index] != '\0'; ++index) {
            if (text[index] == '\n') ++newlines;
        }
        CHECK(newlines == 22U);
        CHECK(strstr(text, "Discover this project") != NULL);
        CHECK(strstr(text, "Confirmed") != NULL);
        CHECK(strstr(text, "Page 1/2") != NULL);
        CHECK(strstr(text, "PgUp/PgDn Page") != NULL);
        CHECK(strstr(text, "Review every candidate") != NULL);
        free(text);
    }
    od_canvas_free(&canvas);
    od_onboarding_free(&onboarding);
    od_candidate_list_free(&candidates);
}

int main(void) {
    test_review_pagination_and_profile();
    test_edit_and_manual_validation();
    test_onboarding_snapshot_stays_in_viewport();
    if (failures != 0) {
        fprintf(stderr, "%d onboarding checks failed\n", failures);
        return 1;
    }
    puts("onboarding checks passed");
    return 0;
}
