#ifndef OPENDOOR_HELP_H
#define OPENDOOR_HELP_H

#include "opendoor/common.h"

#include <stddef.h>

typedef struct {
    size_t *visible;
    size_t visible_count;
    size_t selected;
    size_t page_start;
    size_t page_size;
    char query[96];
} OdHelp;

OdStatus od_help_init(OdHelp *help, size_t page_size, OdError *error);
void od_help_free(OdHelp *help);
OdStatus od_help_search(OdHelp *help, const char *query, OdError *error);
void od_help_set_page_size(OdHelp *help, size_t rows);
void od_help_move(OdHelp *help, int rows);
void od_help_move_page(OdHelp *help, int pages);
void od_help_home(OdHelp *help);
void od_help_end(OdHelp *help);
const char *od_help_key(const OdHelp *help, size_t visible_index);
const char *od_help_description(const OdHelp *help, size_t visible_index);

#endif
