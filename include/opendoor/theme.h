#ifndef OPENDOOR_THEME_H
#define OPENDOOR_THEME_H

#include <stddef.h>

typedef enum {
    OD_ROLE_DEFAULT = 0,
    OD_ROLE_PRIMARY,
    OD_ROLE_SUCCESS,
    OD_ROLE_WARNING,
    OD_ROLE_DANGER,
    OD_ROLE_MUTED,
    OD_ROLE_SELECTED,
    OD_ROLE_FOCUSED_BORDER,
    OD_ROLE_INACTIVE_BORDER,
    OD_ROLE_COUNT
} OdThemeRole;

typedef struct {
    const char *name;
    short foreground[OD_ROLE_COUNT];
    short background[OD_ROLE_COUNT];
    unsigned attributes[OD_ROLE_COUNT];
} OdTheme;

const OdTheme *od_theme_by_name(const char *name);
const OdTheme *od_theme_at(size_t index);
size_t od_theme_count(void);

#endif

