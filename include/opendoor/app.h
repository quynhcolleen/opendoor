#ifndef OPENDOOR_APP_H
#define OPENDOOR_APP_H

#include <stdbool.h>

typedef struct {
    const char *project_path;
    bool force_ascii;
    bool update_requested;
} OpendoorOptions;

int opendoor_parse_args(int argc, char **argv, OpendoorOptions *options);
int opendoor_run(const OpendoorOptions *options);
void opendoor_print_version(void);

#endif
