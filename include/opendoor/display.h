#ifndef OPENDOOR_DISPLAY_H
#define OPENDOOR_DISPLAY_H

#include "opendoor/discovery.h"

#include <stdbool.h>
#include <stddef.h>

/* Shared display vocabulary for declaration locations and sources. */
bool od_display_relative_folder(const char *relative_path,
                                char *destination,
                                size_t capacity);
bool od_display_source(OdPortSourceKind source_kind,
                       const char *environment_key,
                       const char *contributing_paths,
                       char *destination,
                       size_t capacity);

/* History timestamps are persisted as RFC 3339 UTC with milliseconds, but
 * displayed at whole-second precision as YYYY-MM-DD HH:MM:SS. */
bool od_display_history_when(const char *timestamp,
                             char *destination,
                             size_t capacity);

#endif
