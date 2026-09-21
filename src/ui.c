#include "opendoor/ui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t utf8_glyph_length(unsigned char first) {
    if (first < 0x80U) return 1U;
    if ((first & 0xe0U) == 0xc0U) return 2U;
    if ((first & 0xf0U) == 0xe0U) return 3U;
    if ((first & 0xf8U) == 0xf0U) return 4U;
    return 1U;
}

static size_t text_columns(const char *text) {
    size_t columns = 0U;
    for (size_t index = 0U; text[index] != '\0' && text[index] != '\n';) {
        size_t length = utf8_glyph_length((unsigned char)text[index]);
        size_t available = strlen(text + index);
        index += length <= available ? length : 1U;
        ++columns;
    }
    return columns;
}

OdStatus od_canvas_init(OdCanvas *canvas, size_t width, size_t height, OdError *error) {
    if (canvas == NULL || width == 0U || height == 0U || width > SIZE_MAX / height) {
        od_error_set(error, OD_ERROR_INVALID, "canvas dimensions are invalid");
        return OD_ERROR_INVALID;
    }
    *canvas = (OdCanvas){0};
    canvas->cells = calloc(width * height, sizeof(*canvas->cells));
    if (canvas->cells == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to allocate canvas");
        return OD_ERROR_MEMORY;
    }
    canvas->width = width;
    canvas->height = height;
    od_canvas_clear(canvas, OD_ROLE_DEFAULT);
    od_error_clear(error);
    return OD_OK;
}

void od_canvas_free(OdCanvas *canvas) {
    if (canvas == NULL) return;
    free(canvas->cells);
    *canvas = (OdCanvas){0};
}

void od_canvas_clear(OdCanvas *canvas, OdThemeRole role) {
    if (canvas == NULL) return;
    for (size_t index = 0U; index < canvas->width * canvas->height; ++index) {
        (void)strcpy(canvas->cells[index].glyph, " ");
        canvas->cells[index].role = role;
        canvas->cells[index].attributes = 0U;
    }
}

void od_canvas_put(OdCanvas *canvas,
                   int x,
                   int y,
                   const char *glyph,
                   OdThemeRole role,
                   unsigned attributes) {
    if (canvas == NULL || glyph == NULL || x < 0 || y < 0 ||
        (size_t)x >= canvas->width || (size_t)y >= canvas->height) return;
    OdCell *cell = &canvas->cells[(size_t)y * canvas->width + (size_t)x];
    size_t length = utf8_glyph_length((unsigned char)glyph[0]);
    size_t available = strlen(glyph);
    if (length > available || length >= sizeof(cell->glyph)) length = 1U;
    memcpy(cell->glyph, glyph, length);
    cell->glyph[length] = '\0';
    cell->role = role;
    cell->attributes = attributes;
}

void od_canvas_write(OdCanvas *canvas,
                     int x,
                     int y,
                     const char *text,
                     size_t maximum_columns,
                     OdThemeRole role,
                     unsigned attributes) {
    if (canvas == NULL || text == NULL || y < 0 || (size_t)y >= canvas->height ||
        maximum_columns == 0U) return;
    size_t column = 0U;
    size_t index = 0U;
    while (text[index] != '\0' && text[index] != '\n' && column < maximum_columns) {
        int destination_x = x + (int)column;
        if (destination_x >= (int)canvas->width) break;
        size_t length = utf8_glyph_length((unsigned char)text[index]);
        size_t available = strlen(text + index);
        if (length > available) length = 1U;
        if (destination_x >= 0) {
            char glyph[OD_CELL_BYTES] = {0};
            if (length >= sizeof(glyph)) length = 1U;
            memcpy(glyph, text + index, length);
            od_canvas_put(canvas, destination_x, y, glyph, role, attributes);
        }
        index += length;
        ++column;
    }
}

void od_canvas_write_centered(OdCanvas *canvas,
                              int y,
                              const char *text,
                              OdThemeRole role,
                              unsigned attributes) {
    if (canvas == NULL || text == NULL) return;
    size_t columns = text_columns(text);
    int x = columns >= canvas->width ? 0 : (int)((canvas->width - columns) / 2U);
    od_canvas_write(canvas, x, y, text, canvas->width, role, attributes);
}

void od_canvas_box(OdCanvas *canvas,
                   int x,
                   int y,
                   int width,
                   int height,
                   bool ascii,
                   OdThemeRole role) {
    if (canvas == NULL || width < 2 || height < 2) return;
    const char *top_left = ascii ? "+" : "┌";
    const char *top_right = ascii ? "+" : "┐";
    const char *bottom_left = ascii ? "+" : "└";
    const char *bottom_right = ascii ? "+" : "┘";
    const char *horizontal = ascii ? "-" : "─";
    const char *vertical = ascii ? "|" : "│";
    od_canvas_put(canvas, x, y, top_left, role, 0U);
    od_canvas_put(canvas, x + width - 1, y, top_right, role, 0U);
    od_canvas_put(canvas, x, y + height - 1, bottom_left, role, 0U);
    od_canvas_put(canvas, x + width - 1, y + height - 1, bottom_right, role, 0U);
    for (int column = 1; column < width - 1; ++column) {
        od_canvas_put(canvas, x + column, y, horizontal, role, 0U);
        od_canvas_put(canvas, x + column, y + height - 1, horizontal, role, 0U);
    }
    for (int row = 1; row < height - 1; ++row) {
        od_canvas_put(canvas, x, y + row, vertical, role, 0U);
        od_canvas_put(canvas, x + width - 1, y + row, vertical, role, 0U);
    }
}

char *od_canvas_to_text(const OdCanvas *canvas, OdError *error) {
    if (canvas == NULL || canvas->cells == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "canvas is required");
        return NULL;
    }
    if (canvas->width > (SIZE_MAX - 1U) / (OD_CELL_BYTES * canvas->height + 1U)) {
        od_error_set(error, OD_ERROR_MEMORY, "canvas is too large to serialize");
        return NULL;
    }
    size_t capacity = canvas->width * canvas->height * (OD_CELL_BYTES - 1U) +
                      canvas->height + 1U;
    char *text = malloc(capacity);
    if (text == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to serialize canvas");
        return NULL;
    }
    size_t output = 0U;
    for (size_t y = 0U; y < canvas->height; ++y) {
        for (size_t x = 0U; x < canvas->width; ++x) {
            const char *glyph = canvas->cells[y * canvas->width + x].glyph;
            size_t length = strlen(glyph);
            memcpy(text + output, glyph, length);
            output += length;
        }
        text[output++] = '\n';
    }
    text[output] = '\0';
    od_error_clear(error);
    return text;
}

