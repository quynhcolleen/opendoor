#ifndef OPENDOOR_UI_H
#define OPENDOOR_UI_H

#include "opendoor/common.h"
#include "opendoor/theme.h"

#include <stdbool.h>
#include <stddef.h>

#define OD_CELL_BYTES 16U

typedef struct {
    char glyph[OD_CELL_BYTES];
    OdThemeRole role;
    unsigned attributes;
} OdCell;

typedef struct {
    size_t width;
    size_t height;
    OdCell *cells;
} OdCanvas;

OdStatus od_canvas_init(OdCanvas *canvas, size_t width, size_t height, OdError *error);
void od_canvas_free(OdCanvas *canvas);
void od_canvas_clear(OdCanvas *canvas, OdThemeRole role);
void od_canvas_put(OdCanvas *canvas, int x, int y, const char *glyph,
                   OdThemeRole role, unsigned attributes);
void od_canvas_write(OdCanvas *canvas, int x, int y, const char *text,
                     size_t maximum_columns, OdThemeRole role, unsigned attributes);
void od_canvas_write_slice(OdCanvas *canvas, int x, int y, const char *text,
                           size_t first_column, size_t maximum_columns,
                           OdThemeRole role, unsigned attributes);
void od_canvas_write_centered(OdCanvas *canvas, int y, const char *text,
                              OdThemeRole role, unsigned attributes);
size_t od_text_columns(const char *text);
size_t od_page_target(size_t selected, size_t count, size_t page_size, int pages);
void od_canvas_box(OdCanvas *canvas, int x, int y, int width, int height,
                   bool ascii, OdThemeRole role);
char *od_canvas_to_text(const OdCanvas *canvas, OdError *error);

#endif
