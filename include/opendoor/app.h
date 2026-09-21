#ifndef OPENDOOR_APP_H
#define OPENDOOR_APP_H

#include <stdbool.h>

typedef struct {
    const char *project_path;
    const char *profile_path;
    bool force_ascii;
    bool no_color;
    bool reduced_motion;
} OpendoorOptions;

int opendoor_parse_args(int argc, char **argv, OpendoorOptions *options);
int opendoor_run(const OpendoorOptions *options);
void opendoor_print_help(void);
void opendoor_print_version(void);

#endif

