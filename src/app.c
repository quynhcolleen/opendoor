#include "opendoor/app.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool has_value(int index, int argc) {
    return index + 1 < argc;
}

void opendoor_print_help(void) {
    puts("Usage: opendoor [OPTIONS]");
    puts("");
    puts("Interactive project port discovery and conflict resolution.");
    puts("");
    puts("Options:");
    puts("  --project PATH       Use PATH as the project root");
    puts("  --profile PATH       Load an explicit project profile");
    puts("  --ascii              Use ASCII borders and symbols");
    puts("  --no-color           Disable terminal colors");
    puts("  --reduced-motion     Use static progress indicators");
    puts("  --help               Show this help");
    puts("  --version            Show version information");
}

void opendoor_print_version(void) {
    printf("OpenDoor %s\n", OPENDOOR_VERSION);
}

int opendoor_parse_args(int argc, char **argv, OpendoorOptions *options) {
    if (options == NULL) {
        return 2;
    }
    *options = (OpendoorOptions){0};

    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--help") == 0) {
            opendoor_print_help();
            return 1;
        }
        if (strcmp(argument, "--version") == 0) {
            opendoor_print_version();
            return 1;
        }
        if (strcmp(argument, "--project") == 0 || strcmp(argument, "--profile") == 0) {
            if (!has_value(index, argc)) {
                fprintf(stderr, "opendoor: %s requires a path\n", argument);
                return 2;
            }
            ++index;
            if (strcmp(argument, "--project") == 0) {
                options->project_path = argv[index];
            } else {
                options->profile_path = argv[index];
            }
            continue;
        }
        if (strcmp(argument, "--ascii") == 0) {
            options->force_ascii = true;
            continue;
        }
        if (strcmp(argument, "--no-color") == 0) {
            options->no_color = true;
            continue;
        }
        if (strcmp(argument, "--reduced-motion") == 0) {
            options->reduced_motion = true;
            continue;
        }
        fprintf(stderr, "opendoor: unknown option: %s\n", argument);
        return 2;
    }
    return 0;
}

int opendoor_run(const OpendoorOptions *options) {
    (void)options;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("opendoor: an interactive terminal is required\n", stderr);
        return 4;
    }
    return 0;
}

