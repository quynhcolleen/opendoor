#ifndef OPENDOOR_UI_H
#define OPENDOOR_UI_H

#include "opendoor/common.h"

#include <stdbool.h>
#include <stddef.h>

#define OD_CELL_BYTES 16U

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
} OdStyleRole;

typedef struct {
    char glyph[OD_CELL_BYTES];
    OdStyleRole role;
    unsigned attributes;
} OdCell;

typedef struct {
    size_t width;
    size_t height;
    OdCell *cells;
} OdCanvas;

OdStatus od_canvas_init(OdCanvas *canvas, size_t width, size_t height, OdError *error);
void od_canvas_free(OdCanvas *canvas);
void od_canvas_clear(OdCanvas *canvas, OdStyleRole role);
void od_canvas_put(OdCanvas *canvas, int x, int y, const char *glyph,
                   OdStyleRole role, unsigned attributes);
void od_canvas_write(OdCanvas *canvas, int x, int y, const char *text,
                     size_t maximum_columns, OdStyleRole role, unsigned attributes);
void od_canvas_write_slice(OdCanvas *canvas, int x, int y, const char *text,
                           size_t first_column, size_t maximum_columns,
                           OdStyleRole role, unsigned attributes);
void od_canvas_write_centered(OdCanvas *canvas, int y, const char *text,
                              OdStyleRole role, unsigned attributes);
size_t od_text_columns(const char *text);
size_t od_page_target(size_t selected, size_t count, size_t page_size, int pages);
void od_canvas_box(OdCanvas *canvas, int x, int y, int width, int height,
                   bool ascii, OdStyleRole role);
char *od_canvas_to_text(const OdCanvas *canvas, OdError *error);

#endif
