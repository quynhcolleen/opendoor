#include "opendoor/ui.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool continuation(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

static size_t valid_utf8_length(const char *text, size_t available) {
    if (available == 0U) return 0U;
    const unsigned char *bytes = (const unsigned char *)text;
    if (bytes[0] < 0x80U) return 1U;
    if (bytes[0] >= 0xc2U && bytes[0] <= 0xdfU && available >= 2U &&
        continuation(bytes[1])) return 2U;
    if (bytes[0] == 0xe0U && available >= 3U && bytes[1] >= 0xa0U &&
        bytes[1] <= 0xbfU && continuation(bytes[2])) return 3U;
    if (((bytes[0] >= 0xe1U && bytes[0] <= 0xecU) ||
         (bytes[0] >= 0xeeU && bytes[0] <= 0xefU)) && available >= 3U &&
        continuation(bytes[1]) && continuation(bytes[2])) return 3U;
    if (bytes[0] == 0xedU && available >= 3U && bytes[1] >= 0x80U &&
        bytes[1] <= 0x9fU && continuation(bytes[2])) return 3U;
    if (bytes[0] == 0xf0U && available >= 4U && bytes[1] >= 0x90U &&
        bytes[1] <= 0xbfU && continuation(bytes[2]) && continuation(bytes[3])) return 4U;
    if (bytes[0] >= 0xf1U && bytes[0] <= 0xf3U && available >= 4U &&
        continuation(bytes[1]) && continuation(bytes[2]) && continuation(bytes[3])) return 4U;
    if (bytes[0] == 0xf4U && available >= 4U && bytes[1] >= 0x80U &&
        bytes[1] <= 0x8fU && continuation(bytes[2]) && continuation(bytes[3])) return 4U;
    return 0U;
}

static uint32_t utf8_codepoint(const char *text, size_t length) {
    const unsigned char *bytes = (const unsigned char *)text;
    if (length == 1U) return bytes[0];
    if (length == 2U) return ((uint32_t)(bytes[0] & 0x1fU) << 6U) |
                              (uint32_t)(bytes[1] & 0x3fU);
    if (length == 3U) return ((uint32_t)(bytes[0] & 0x0fU) << 12U) |
                              ((uint32_t)(bytes[1] & 0x3fU) << 6U) |
                              (uint32_t)(bytes[2] & 0x3fU);
    return ((uint32_t)(bytes[0] & 0x07U) << 18U) |
           ((uint32_t)(bytes[1] & 0x3fU) << 12U) |
           ((uint32_t)(bytes[2] & 0x3fU) << 6U) |
           (uint32_t)(bytes[3] & 0x3fU);
}

static size_t codepoint_columns(uint32_t codepoint) {
    if ((codepoint >= 0x0300U && codepoint <= 0x036fU) ||
        (codepoint >= 0x1ab0U && codepoint <= 0x1affU) ||
        (codepoint >= 0x1dc0U && codepoint <= 0x1dffU) ||
        (codepoint >= 0x20d0U && codepoint <= 0x20ffU) ||
        (codepoint >= 0xfe00U && codepoint <= 0xfe0fU) ||
        (codepoint >= 0xfe20U && codepoint <= 0xfe2fU) ||
        (codepoint >= 0xe0100U && codepoint <= 0xe01efU) || codepoint == 0x200dU) {
        return 0U;
    }
    if (codepoint >= 0x1100U &&
        (codepoint <= 0x115fU || codepoint == 0x2329U || codepoint == 0x232aU ||
         (codepoint >= 0x2e80U && codepoint <= 0xa4cfU && codepoint != 0x303fU) ||
         (codepoint >= 0xac00U && codepoint <= 0xd7a3U) ||
         (codepoint >= 0xf900U && codepoint <= 0xfaffU) ||
         (codepoint >= 0xfe10U && codepoint <= 0xfe19U) ||
         (codepoint >= 0xfe30U && codepoint <= 0xfe6fU) ||
         (codepoint >= 0xff00U && codepoint <= 0xff60U) ||
         (codepoint >= 0xffe0U && codepoint <= 0xffe6U) ||
         (codepoint >= 0x1f300U && codepoint <= 0x1faffU) ||
         (codepoint >= 0x20000U && codepoint <= 0x3fffdU))) return 2U;
    return 1U;
}

static size_t text_columns(const char *text) {
    size_t columns = 0U;
    size_t total = strlen(text);
    for (size_t index = 0U; index < total && text[index] != '\n';) {
        size_t length = valid_utf8_length(text + index, total - index);
        if (length == 0U) {
            ++index;
            ++columns;
        } else {
            columns += codepoint_columns(utf8_codepoint(text + index, length));
            index += length;
        }
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
    size_t available = strlen(glyph);
    size_t length = valid_utf8_length(glyph, available);
    if (length == 0U || length >= sizeof(cell->glyph)) {
        (void)strcpy(cell->glyph, "?");
    } else if (length == 1U &&
               ((unsigned char)glyph[0] < 0x20U || (unsigned char)glyph[0] == 0x7fU)) {
        (void)strcpy(cell->glyph, " ");
    } else {
        memcpy(cell->glyph, glyph, length);
        cell->glyph[length] = '\0';
    }
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
    size_t total = strlen(text);
    while (index < total && text[index] != '\n' && column < maximum_columns) {
        size_t length = valid_utf8_length(text + index, total - index);
        size_t width = 1U;
        if (length == 0U) {
            length = 1U;
        } else {
            width = codepoint_columns(utf8_codepoint(text + index, length));
        }
        if (width == 0U) {
            if (column > 0U) {
                int previous_x = x + (int)column - 1;
                while (previous_x >= x) {
                    OdCell *previous = &canvas->cells[(size_t)y * canvas->width +
                                                     (size_t)previous_x];
                    size_t used = strlen(previous->glyph);
                    if (used > 0U) {
                        if (used + length < sizeof(previous->glyph)) {
                            memcpy(previous->glyph + used, text + index, length);
                            previous->glyph[used + length] = '\0';
                        }
                        break;
                    }
                    --previous_x;
                }
            }
            index += length;
            continue;
        }
        int destination_x = x + (int)column;
        if (destination_x >= (int)canvas->width ||
            width > maximum_columns - column ||
            (width == 2U && destination_x + 1 >= (int)canvas->width)) break;
        if (destination_x >= 0) {
            char glyph[OD_CELL_BYTES] = {0};
            if (length >= sizeof(glyph)) length = 1U;
            memcpy(glyph, text + index, length);
            od_canvas_put(canvas, destination_x, y, glyph, role, attributes);
            if (width == 2U) {
                OdCell *continuation_cell =
                    &canvas->cells[(size_t)y * canvas->width + (size_t)(destination_x + 1)];
                continuation_cell->glyph[0] = '\0';
                continuation_cell->role = role;
                continuation_cell->attributes = attributes;
            }
        }
        index += length;
        column += width;
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

size_t od_page_target(size_t selected, size_t count, size_t page_size, int pages) {
    if (count == 0U) return 0U;
    if (selected >= count) selected = count - 1U;
    if (page_size == 0U || pages == 0) return selected;
    unsigned magnitude = pages < 0 ? (unsigned)(-(long long)pages) : (unsigned)pages;
    size_t distance = page_size;
    if (magnitude > 1U) {
        distance = distance > SIZE_MAX / magnitude ? SIZE_MAX : distance * magnitude;
    }
    if (pages < 0) return distance > selected ? 0U : selected - distance;
    size_t remaining = count - 1U - selected;
    return selected + (distance > remaining ? remaining : distance);
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
