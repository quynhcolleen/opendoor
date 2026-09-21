#include "opendoor/help.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *key;
    const char *description;
} HelpEntry;

static const HelpEntry entries[] = {
    {"Up / k", "Move to the previous row or menu item"},
    {"Down / j", "Move to the next row or menu item"},
    {"Left / h", "Focus the previous dashboard widget"},
    {"Right / l / Tab", "Focus the next dashboard widget"},
    {"PgUp / PgDn", "Move one visible page inside the focused widget"},
    {"Home / End", "Move to the first or last row"},
    {"Enter", "Open, accept, or save the selected action"},
    {"Space", "Toggle the selected onboarding candidate"},
    {"e", "Edit a value or expand the focused widget"},
    {"d", "Open the selected row details"},
    {"/", "Search the active table or this help screen"},
    {"s / S", "Cycle the sort column or reverse sort direction"},
    {"r", "Refresh listeners, processes, and Docker mappings"},
    {"?", "Open keyboard and workflow help"},
    {"Esc / q", "Close the current screen without saving"},
    {"Mouse click", "Focus widgets and select rows when mouse input is on"},
    {"Mouse wheel", "Move rows inside the focused widget"},
    {"Discover", "Review every candidate before creating a profile"},
    {"Resolve", "Review each conflict and the full assignment diff"},
    {"Save", "Rescan and bind-probe every selected port before writing"},
    {"Reset", "Back up and remove only the generated assignment file"},
    {"Profile", "Stored per project in .opendoor/project.toml"},
    {"Assignments", "Written to the profile's project-relative dotenv file"},
    {"Docker", "Optional; host-only discovery continues when unavailable"},
    {"Small terminal", "Resize to at least 60 columns by 18 rows"}
};

static bool contains_case_insensitive(const char *text, const char *query) {
    if (query[0] == '\0') return true;
    size_t query_length = strlen(query);
    for (size_t start = 0U; text[start] != '\0'; ++start) {
        size_t matched = 0U;
        while (matched < query_length && text[start + matched] != '\0' &&
               tolower((unsigned char)text[start + matched]) ==
                   tolower((unsigned char)query[matched])) {
            ++matched;
        }
        if (matched == query_length) return true;
    }
    return false;
}

static void ensure_page(OdHelp *help) {
    if (help->visible_count == 0U) {
        help->selected = 0U;
        help->page_start = 0U;
        return;
    }
    if (help->selected >= help->visible_count) help->selected = help->visible_count - 1U;
    size_t rows = help->page_size == 0U ? 1U : help->page_size;
    if (help->selected < help->page_start ||
        help->selected >= help->page_start + rows) {
        help->page_start = (help->selected / rows) * rows;
    }
}

OdStatus od_help_init(OdHelp *help, size_t page_size, OdError *error) {
    if (help == NULL || page_size == 0U) {
        od_error_set(error, OD_ERROR_INVALID, "help page size must be positive");
        return OD_ERROR_INVALID;
    }
    *help = (OdHelp){0};
    help->visible = calloc(sizeof(entries) / sizeof(entries[0]), sizeof(*help->visible));
    if (help->visible == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare help index");
        return OD_ERROR_MEMORY;
    }
    help->page_size = page_size;
    help->visible_count = sizeof(entries) / sizeof(entries[0]);
    for (size_t index = 0U; index < help->visible_count; ++index) help->visible[index] = index;
    od_error_clear(error);
    return OD_OK;
}

void od_help_free(OdHelp *help) {
    if (help == NULL) return;
    free(help->visible);
    *help = (OdHelp){0};
}

OdStatus od_help_search(OdHelp *help, const char *query, OdError *error) {
    if (help == NULL || query == NULL || strlen(query) >= sizeof(help->query)) {
        od_error_set(error, OD_ERROR_INVALID, "help search is too long");
        return OD_ERROR_INVALID;
    }
    (void)strcpy(help->query, query);
    help->visible_count = 0U;
    for (size_t index = 0U; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        if (contains_case_insensitive(entries[index].key, query) ||
            contains_case_insensitive(entries[index].description, query)) {
            help->visible[help->visible_count++] = index;
        }
    }
    help->selected = 0U;
    help->page_start = 0U;
    ensure_page(help);
    od_error_clear(error);
    return OD_OK;
}

void od_help_set_page_size(OdHelp *help, size_t rows) {
    if (help == NULL) return;
    help->page_size = rows == 0U ? 1U : rows;
    ensure_page(help);
}

void od_help_move(OdHelp *help, int rows) {
    if (help == NULL || help->visible_count == 0U) return;
    long long target = (long long)help->selected + (long long)rows;
    if (target < 0LL) target = 0LL;
    if ((unsigned long long)target >= help->visible_count) {
        target = (long long)(help->visible_count - 1U);
    }
    help->selected = (size_t)target;
    ensure_page(help);
}

void od_help_move_page(OdHelp *help, int pages) {
    if (help == NULL) return;
    long long distance = (long long)pages *
                         (long long)(help->page_size == 0U ? 1U : help->page_size);
    if (distance > INT_MAX) distance = INT_MAX;
    if (distance < INT_MIN) distance = INT_MIN;
    od_help_move(help, (int)distance);
}

void od_help_home(OdHelp *help) {
    if (help == NULL) return;
    help->selected = 0U;
    help->page_start = 0U;
}

void od_help_end(OdHelp *help) {
    if (help == NULL || help->visible_count == 0U) return;
    help->selected = help->visible_count - 1U;
    ensure_page(help);
}

const char *od_help_key(const OdHelp *help, size_t visible_index) {
    if (help == NULL || visible_index >= help->visible_count) return "";
    return entries[help->visible[visible_index]].key;
}

const char *od_help_description(const OdHelp *help, size_t visible_index) {
    if (help == NULL || visible_index >= help->visible_count) return "";
    return entries[help->visible[visible_index]].description;
}
