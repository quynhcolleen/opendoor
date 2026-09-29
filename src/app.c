#include "opendoor/app.h"
#include "opendoor/tui.h"
#include "opendoor/update.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool has_value(int index, int argc) {
    return index + 1 < argc;
}

void opendoor_print_version(void) {
    printf("OpenDoor %s\n", OPENDOOR_VERSION);
}

int opendoor_parse_args(int argc, char **argv, OpendoorOptions *options) {
    if (options == NULL) return 2;
    *options = (OpendoorOptions){0};
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--update") == 0 && argc != 2) {
            fputs("opendoor: --update must be used alone\n", stderr);
            return 2;
        }
    }
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--version") == 0) {
            opendoor_print_version();
            return 1;
        }
        if (strcmp(argument, "--update") == 0) {
            options->update_requested = true;
            continue;
        }
        if (strcmp(argument, "--project") == 0) {
            if (!has_value(index, argc)) {
                fputs("opendoor: --project requires a path\n", stderr);
                return 2;
            }
            options->project_path = argv[++index];
            continue;
        }
        if (strcmp(argument, "--ascii") == 0) {
            options->force_ascii = true;
            continue;
        }
        fprintf(stderr, "opendoor: unknown option: %s\n", argument);
        return 2;
    }
    return 0;
}

int opendoor_run(const OpendoorOptions *options) {
    if (options == NULL) return 2;
    if (options->update_requested) return od_update_current_checkout();
    const char *project = options->project_path == NULL ? "." : options->project_path;
    struct stat project_status;
    if (stat(project, &project_status) != 0 || !S_ISDIR(project_status.st_mode)) {
        fprintf(stderr, "opendoor: invalid project directory: %s\n", project);
        return 3;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("opendoor: an interactive terminal is required\n", stderr);
        return 4;
    }
    return od_tui_run(options);
}
