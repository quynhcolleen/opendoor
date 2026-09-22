#include "opendoor/app.h"
#include "opendoor/config.h"
#include "opendoor/tui.h"
#include "opendoor/update.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
    puts("  --update             Rebuild and install this source checkout");
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
        if (strcmp(argv[index], "--update") == 0 && argc != 2) {
            fputs("opendoor: --update must be used alone\n", stderr);
            return 2;
        }
    }

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
        if (strcmp(argument, "--update") == 0) {
            options->update_requested = true;
            continue;
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
    if (options->update_requested) return od_update_current_checkout();
    const char *project = options->project_path == NULL ? "." : options->project_path;
    struct stat project_status;
    if (stat(project, &project_status) != 0 || !S_ISDIR(project_status.st_mode)) {
        fprintf(stderr, "opendoor: invalid project directory: %s\n", project);
        return 3;
    }
    char default_profile[4096];
    const char *profile_path = options->profile_path;
    if (profile_path == NULL) {
        int count = snprintf(default_profile, sizeof(default_profile), "%s%s.opendoor/project.toml",
                             project, project[0] != '\0' &&
                             project[strlen(project) - 1U] == '/' ? "" : "/");
        if (count < 0 || (size_t)count >= sizeof(default_profile)) {
            fputs("opendoor: project profile path is too long\n", stderr);
            return 3;
        }
        profile_path = default_profile;
    }
    struct stat profile_status;
    if (lstat(profile_path, &profile_status) == 0 || options->profile_path != NULL) {
        OdProfile profile;
        OdError error;
        if (od_profile_load(profile_path, &profile, &error) != OD_OK) {
            fprintf(stderr, "opendoor: %s\n", error.message);
            return 3;
        }
        od_profile_free(&profile);
    } else if (errno != ENOENT) {
        fprintf(stderr, "opendoor: unable to inspect profile: %s\n", strerror(errno));
        return 3;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("opendoor: an interactive terminal is required\n", stderr);
        return 4;
    }
    return od_tui_run(options);
}
