#include "opendoor/history.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const unsigned char *cursor;
    const unsigned char *end;
    bool memory_error;
} JsonReader;

typedef struct {
    char *text;
    size_t length;
    bool oversized;
} JsonWriter;

void od_history_record_free(OdHistoryRecord *record) {
    if (record == NULL) return;
    free(record->timestamp);
    free(record->relative_path);
    free(record->environment_key);
    free(record->reason);
    *record = (OdHistoryRecord){0};
}

void od_history_free(OdHistory *history) {
    if (history == NULL) return;
    for (size_t index = 0U; index < history->count; ++index) {
        od_history_record_free(&history->items[index]);
    }
    free(history->items);
    for (size_t index = 0U; index < history->warning_count; ++index) {
        free(history->warnings[index]);
    }
    free(history->warnings);
    *history = (OdHistory){0};
}

static void skip_space(JsonReader *reader) {
    while (reader->cursor < reader->end &&
           (*reader->cursor == ' ' || *reader->cursor == '\t' ||
            *reader->cursor == '\r' || *reader->cursor == '\n')) {
        ++reader->cursor;
    }
}

static bool consume(JsonReader *reader, unsigned char character) {
    skip_space(reader);
    if (reader->cursor == reader->end || *reader->cursor != character) return false;
    ++reader->cursor;
    return true;
}

static bool consume_word(JsonReader *reader, const char *word) {
    skip_space(reader);
    size_t length = strlen(word);
    if ((size_t)(reader->end - reader->cursor) < length ||
        memcmp(reader->cursor, word, length) != 0) return false;
    reader->cursor += length;
    return true;
}

/* Reject overlong encodings, surrogate code points, and values above U+10FFFF. */
static size_t utf8_width(const unsigned char *text, size_t length) {
    if (length == 0U) return 0U;
    if (text[0] < 0x80U) return 1U;
    size_t width = text[0] >= 0xc2U && text[0] <= 0xdfU ? 2U :
                   text[0] >= 0xe0U && text[0] <= 0xefU ? 3U :
                   text[0] >= 0xf0U && text[0] <= 0xf4U ? 4U : 0U;
    if (width == 0U || width > length) return 0U;
    for (size_t index = 1U; index < width; ++index) {
        if (text[index] < 0x80U || text[index] > 0xbfU) return 0U;
    }
    if ((text[0] == 0xe0U && text[1] < 0xa0U) ||
        (text[0] == 0xedU && text[1] >= 0xa0U) ||
        (text[0] == 0xf0U && text[1] < 0x90U) ||
        (text[0] == 0xf4U && text[1] > 0x8fU)) return 0U;
    return width;
}

static bool hex_quad(JsonReader *reader, unsigned int *value) {
    if ((size_t)(reader->end - reader->cursor) < 4U) return false;
    unsigned int number = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        unsigned char character = *reader->cursor++;
        unsigned int digit;
        if (character >= '0' && character <= '9') digit = (unsigned int)(character - '0');
        else if (character >= 'a' && character <= 'f') digit = (unsigned int)(character - 'a') + 10U;
        else if (character >= 'A' && character <= 'F') digit = (unsigned int)(character - 'A') + 10U;
        else return false;
        number = number * 16U + digit;
    }
    *value = number;
    return true;
}

static size_t encode_utf8(unsigned int value, char *text) {
    if (value < 0x80U) {
        text[0] = (char)value;
        return 1U;
    }
    if (value < 0x800U) {
        text[0] = (char)(0xc0U | (value >> 6U));
        text[1] = (char)(0x80U | (value & 0x3fU));
        return 2U;
    }
    if (value < 0x10000U) {
        text[0] = (char)(0xe0U | (value >> 12U));
        text[1] = (char)(0x80U | ((value >> 6U) & 0x3fU));
        text[2] = (char)(0x80U | (value & 0x3fU));
        return 3U;
    }
    text[0] = (char)(0xf0U | (value >> 18U));
    text[1] = (char)(0x80U | ((value >> 12U) & 0x3fU));
    text[2] = (char)(0x80U | ((value >> 6U) & 0x3fU));
    text[3] = (char)(0x80U | (value & 0x3fU));
    return 4U;
}

static bool read_string(JsonReader *reader, char **output, size_t *length) {
    if (!consume(reader, '"')) return false;
    size_t capacity = (size_t)(reader->end - reader->cursor) + 1U;
    char *text = malloc(capacity);
    if (text == NULL) {
        reader->memory_error = true;
        return false;
    }
    size_t used = 0U;
    while (reader->cursor < reader->end) {
        unsigned char character = *reader->cursor++;
        if (character == '"') {
            text[used] = '\0';
            char *compact = realloc(text, used + 1U);
            if (compact != NULL) text = compact;
            *output = text;
            if (length != NULL) *length = used;
            return true;
        }
        if (character < 0x20U) break;
        if (character == '\\') {
            if (reader->cursor == reader->end) break;
            character = *reader->cursor++;
            if (character == 'u') {
                unsigned int value;
                if (!hex_quad(reader, &value)) break;
                if (value >= 0xd800U && value <= 0xdbffU) {
                    unsigned int low;
                    if ((size_t)(reader->end - reader->cursor) < 6U ||
                        reader->cursor[0] != '\\' || reader->cursor[1] != 'u') break;
                    reader->cursor += 2U;
                    if (!hex_quad(reader, &low) || low < 0xdc00U || low > 0xdfffU) break;
                    value = 0x10000U + ((value - 0xd800U) << 10U) + low - 0xdc00U;
                } else if (value >= 0xdc00U && value <= 0xdfffU) break;
                used += encode_utf8(value, text + used);
                continue;
            }
            switch (character) {
                case '"': case '\\': case '/': break;
                case 'b': character = '\b'; break;
                case 'f': character = '\f'; break;
                case 'n': character = '\n'; break;
                case 'r': character = '\r'; break;
                case 't': character = '\t'; break;
                default: free(text); return false;
            }
        } else if (character >= 0x80U) {
            const unsigned char *start = reader->cursor - 1U;
            size_t width = utf8_width(start, (size_t)(reader->end - start));
            if (width == 0U) break;
            memcpy(text + used, start, width);
            used += width;
            reader->cursor = start + width;
            continue;
        }
        text[used++] = (char)character;
    }
    free(text);
    return false;
}

/* Known strings use C ownership conventions and cannot contain embedded NUL.
 * Unknown JSON strings may contain escaped U+0000 and are simply discarded. */
static bool read_c_string(JsonReader *reader, char **output) {
    char *text = NULL;
    size_t length = 0U;
    if (!read_string(reader, &text, &length)) return false;
    if (memchr(text, '\0', length) != NULL) {
        free(text);
        return false;
    }
    *output = text;
    return true;
}

static bool is_digit(unsigned char character) {
    return character >= '0' && character <= '9';
}

static bool read_unsigned(JsonReader *reader, uint64_t *value) {
    skip_space(reader);
    if (reader->cursor == reader->end || !is_digit(*reader->cursor)) return false;
    bool leading_zero = *reader->cursor == '0';
    uint64_t number = 0U;
    size_t digits = 0U;
    while (reader->cursor < reader->end && is_digit(*reader->cursor)) {
        unsigned int digit = (unsigned int)(*reader->cursor++ - '0');
        if (number > (UINT64_MAX - digit) / 10U) return false;
        number = number * 10U + digit;
        ++digits;
    }
    if (leading_zero && digits != 1U) return false;
    *value = number;
    return true;
}

static bool skip_number(JsonReader *reader) {
    if (reader->cursor < reader->end && *reader->cursor == '-') ++reader->cursor;
    if (reader->cursor == reader->end || !is_digit(*reader->cursor)) return false;
    if (*reader->cursor == '0') ++reader->cursor;
    else while (reader->cursor < reader->end && is_digit(*reader->cursor)) ++reader->cursor;
    if (reader->cursor < reader->end && *reader->cursor == '.') {
        ++reader->cursor;
        if (reader->cursor == reader->end || !is_digit(*reader->cursor)) return false;
        while (reader->cursor < reader->end && is_digit(*reader->cursor)) ++reader->cursor;
    }
    if (reader->cursor < reader->end &&
        (*reader->cursor == 'e' || *reader->cursor == 'E')) {
        ++reader->cursor;
        if (reader->cursor < reader->end &&
            (*reader->cursor == '+' || *reader->cursor == '-')) ++reader->cursor;
        if (reader->cursor == reader->end || !is_digit(*reader->cursor)) return false;
        while (reader->cursor < reader->end && is_digit(*reader->cursor)) ++reader->cursor;
    }
    return true;
}

static bool skip_value(JsonReader *reader) {
    /* One byte of grammar state per open container, bounded by input length.
     * Iteration avoids both a recursive stack risk and an extra nesting cap. */
    size_t capacity = (size_t)(reader->end - reader->cursor) + 1U;
    unsigned char *frames = malloc(capacity);
    if (frames == NULL) { reader->memory_error = true; return false; }
    size_t depth = 0U;
    bool need_value = true;
    bool valid = true;
    while (valid) {
        skip_space(reader);
        if (need_value) {
            if (reader->cursor == reader->end) { valid = false; break; }
            unsigned char character = *reader->cursor;
            if (character == '[' || character == '{') {
                ++reader->cursor;
                /* bit 0: object; bit 1: first item; bit 2: after value */
                frames[depth++] = character == '{' ? 3U : 2U;
            } else if (character == '"') {
                char *text = NULL;
                valid = read_string(reader, &text, NULL);
                free(text);
            } else if (character == 't') valid = consume_word(reader, "true");
            else if (character == 'f') valid = consume_word(reader, "false");
            else if (character == 'n') valid = consume_word(reader, "null");
            else valid = skip_number(reader);
            need_value = false;
            if (depth == 0U) break;
        } else {
            unsigned char frame = frames[depth - 1U];
            unsigned char end = (frame & 1U) != 0U ? '}' : ']';
            if ((frame & 4U) != 0U) {
                if (consume(reader, end)) {
                    if (--depth == 0U) break;
                } else if (consume(reader, ',')) frames[depth - 1U] &= 1U;
                else valid = false;
            } else if ((frame & 2U) != 0U && consume(reader, end)) {
                if (--depth == 0U) break;
            } else {
                if ((frame & 1U) != 0U) {
                    char *key = NULL;
                    valid = read_string(reader, &key, NULL);
                    free(key);
                    if (!valid || !consume(reader, ':')) { valid = false; break; }
                }
                frames[depth - 1U] |= 4U;
                need_value = true;
            }
        }
    }
    free(frames);
    return valid;
}

static int field_index(const char *key) {
    static const char *const fields[] = {
        "v", "id", "timestamp", "kind", "reverts", "file", "source", "write",
        "key", "line", "column", "offset", "length", "old_port", "new_port"
    };
    for (size_t index = 0U; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        if (strcmp(key, fields[index]) == 0) return (int)index;
    }
    return -1;
}

static bool read_field(JsonReader *reader, int field, OdHistoryRecord *record) {
    if (field < 0) return skip_value(reader);
    if (field == 2) return read_c_string(reader, &record->timestamp);
    if (field == 5) return read_c_string(reader, &record->relative_path);
    if (field == 8) {
        if (consume_word(reader, "null")) return true;
        return read_c_string(reader, &record->environment_key);
    }
    if (field == 3 || field == 6 || field == 7) {
        char *text = NULL;
        if (!read_c_string(reader, &text)) return false;
        bool valid = true;
        if (field == 3) {
            if (strcmp(text, "apply") == 0) record->kind = OD_HISTORY_APPLY;
            else if (strcmp(text, "revert") == 0) record->kind = OD_HISTORY_REVERT;
            else valid = false;
        } else if (field == 6) {
            if (strcmp(text, "env") == 0) record->source_kind = OD_SOURCE_ENV;
            else if (strcmp(text, "compose") == 0) record->source_kind = OD_SOURCE_COMPOSE;
            else valid = false;
        } else {
            if (strcmp(text, "env_literal") == 0) record->write_kind = OD_WRITE_ENV_LITERAL;
            else if (strcmp(text, "compose_literal") == 0) record->write_kind = OD_WRITE_COMPOSE_LITERAL;
            else valid = false;
        }
        free(text);
        return valid;
    }
    if (field == 4 && consume_word(reader, "null")) return true;
    uint64_t value;
    if (!read_unsigned(reader, &value)) return false;
    switch (field) {
        case 0:
            if (value > UINT_MAX) return false;
            record->version = (unsigned int)value;
            return true;
        case 1: record->id = value; return true;
        case 4: record->has_reverts = true; record->reverts = value; return true;
        case 13: case 14:
            if (value == 0U || value > UINT16_MAX) return false;
            if (field == 13) record->old_port = (uint16_t)value;
            else record->new_port = (uint16_t)value;
            return true;
        default:
            if (value > SIZE_MAX) return false;
            if (field == 9) record->line = (size_t)value;
            else if (field == 10) record->column = (size_t)value;
            else if (field == 11) record->byte_offset = (size_t)value;
            else if (field == 12) record->byte_length = (size_t)value;
            return true;
    }
}

static unsigned int decimal_digits(const char *text, size_t length) {
    unsigned int result = 0U;
    for (size_t index = 0U; index < length; ++index) {
        result = result * 10U + (unsigned int)(text[index] - '0');
    }
    return result;
}

static bool valid_timestamp(const char *text) {
    if (text == NULL || strlen(text) != 24U) return false;
    for (size_t index = 0U; index < 24U; ++index) {
        char separator = index == 4U || index == 7U ? '-' :
                         index == 10U ? 'T' :
                         index == 13U || index == 16U ? ':' :
                         index == 19U ? '.' : index == 23U ? 'Z' : '\0';
        if (separator != '\0') {
            if (text[index] != separator) return false;
        } else if (!is_digit((unsigned char)text[index])) return false;
    }
    unsigned int year = decimal_digits(text, 4U);
    unsigned int month = decimal_digits(text + 5U, 2U);
    unsigned int day = decimal_digits(text + 8U, 2U);
    static const unsigned int month_days[] = {31U,28U,31U,30U,31U,30U,31U,31U,30U,31U,30U,31U};
    if (month == 0U || month > 12U) return false;
    unsigned int days = month_days[month - 1U];
    if (month == 2U && year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U)) ++days;
    return day > 0U && day <= days && decimal_digits(text + 11U, 2U) < 24U &&
           decimal_digits(text + 14U, 2U) < 60U && decimal_digits(text + 17U, 2U) <= 60U;
}

static bool valid_relative_path(const char *path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/') return false;
    const char *part = path;
    while (*part != '\0') {
        const char *slash = strchr(part, '/');
        size_t length = slash == NULL ? strlen(part) : (size_t)(slash - part);
        if (length == 2U && part[0] == '.' && part[1] == '.') return false;
        if (slash == NULL) return length != 0U;
        part = slash + 1U;
    }
    return false;
}

static bool valid_record(const OdHistoryRecord *record) {
    if (record == NULL || record->id == 0U || !valid_timestamp(record->timestamp) ||
        !valid_relative_path(record->relative_path) || record->line == 0U ||
        record->column == 0U || record->byte_length == 0U ||
        record->byte_offset > SIZE_MAX - record->byte_length ||
        record->old_port == 0U || record->new_port == 0U) return false;
    if (record->kind == OD_HISTORY_APPLY) {
        if (record->has_reverts) return false;
    } else if (record->kind == OD_HISTORY_REVERT) {
        if (!record->has_reverts || record->reverts == 0U || record->reverts >= record->id) return false;
    } else return false;
    return (record->source_kind == OD_SOURCE_ENV && record->write_kind == OD_WRITE_ENV_LITERAL) ||
           (record->source_kind == OD_SOURCE_COMPOSE && record->write_kind == OD_WRITE_COMPOSE_LITERAL);
}

OdStatus od_history_record_parse(const char *text, size_t length,
                                  OdHistoryRecord *record, OdError *error) {
    if (record != NULL) *record = (OdHistoryRecord){0};
    if (text == NULL || record == NULL || length > OD_HISTORY_RECORD_MAX_BYTES ||
        memchr(text, '\0', length) != NULL) {
        od_error_set(error, OD_ERROR_INVALID, "history record is missing, exceeds 64 KiB, or contains NUL data");
        return OD_ERROR_INVALID;
    }
    JsonReader reader = {.cursor = (const unsigned char *)text,
                         .end = (const unsigned char *)text + length};
    unsigned int seen = 0U;
    bool valid = consume(&reader, '{');
    if (valid && !consume(&reader, '}')) {
        do {
            char *key = NULL;
            size_t key_length = 0U;
            if (!read_string(&reader, &key, &key_length)) { valid = false; break; }
            int field = memchr(key, '\0', key_length) != NULL ? -1 : field_index(key);
            free(key);
            unsigned int bit = field < 0 ? 0U : 1U << (unsigned int)field;
            if ((seen & bit) != 0U || !consume(&reader, ':') ||
                !read_field(&reader, field, record)) { valid = false; break; }
            seen |= bit;
            if (consume(&reader, '}')) break;
            if (!consume(&reader, ',')) { valid = false; break; }
        } while (true);
    }
    skip_space(&reader);
    if (!valid || reader.cursor != reader.end || seen != 0x7fffU || !valid_record(record)) {
        od_history_record_free(record);
        OdStatus status = reader.memory_error ? OD_ERROR_MEMORY : OD_ERROR_INVALID;
        od_error_set(error, status, reader.memory_error ? "unable to parse history record" : "malformed history record");
        return status;
    }
    if (record->version != OD_HISTORY_VERSION) {
        od_history_record_free(record);
        od_error_set(error, OD_ERROR_UNSUPPORTED, "unsupported history record version");
        return OD_ERROR_UNSUPPORTED;
    }
    record->reason = strdup("Availability has not been checked");
    if (record->reason == NULL) {
        od_history_record_free(record);
        od_error_set(error, OD_ERROR_MEMORY, "unable to store history availability reason");
        return OD_ERROR_MEMORY;
    }
    record->availability = OD_HISTORY_UNCHECKED;
    od_error_clear(error);
    return OD_OK;
}

static void write_bytes(JsonWriter *writer, const char *text, size_t length) {
    if (writer->oversized) return;
    if (length > OD_HISTORY_RECORD_MAX_BYTES - writer->length) {
        writer->oversized = true;
        return;
    }
    memcpy(writer->text + writer->length, text, length);
    writer->length += length;
    writer->text[writer->length] = '\0';
}

static void write_format(JsonWriter *writer, const char *format, ...) {
    if (writer->oversized) return;
    size_t remaining = OD_HISTORY_RECORD_MAX_BYTES - writer->length + 1U;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(writer->text + writer->length, remaining, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= remaining) writer->oversized = true;
    else writer->length += (size_t)written;
}

static bool write_string(JsonWriter *writer, const char *text) {
    write_bytes(writer, "\"", 1U);
    size_t length = strlen(text);
    for (size_t index = 0U; index < length && !writer->oversized; ++index) {
        unsigned char character = (unsigned char)text[index];
        const char *escape = character == '"' ? "\\\"" : character == '\\' ? "\\\\" :
                             character == '\b' ? "\\b" : character == '\f' ? "\\f" :
                             character == '\n' ? "\\n" : character == '\r' ? "\\r" :
                             character == '\t' ? "\\t" : NULL;
        if (escape != NULL) write_bytes(writer, escape, 2U);
        else if (character < 0x20U) write_format(writer, "\\u%04x", (unsigned int)character);
        else if (character >= 0x80U) {
            size_t width = utf8_width((const unsigned char *)text + index, length - index);
            if (width == 0U) return false;
            write_bytes(writer, text + index, width);
            index += width - 1U;
        } else write_bytes(writer, text + index, 1U);
    }
    write_bytes(writer, "\"", 1U);
    return true;
}

OdStatus od_history_record_render(const OdHistoryRecord *record,
                                   char **text, size_t *length, OdError *error) {
    if (text != NULL) *text = NULL;
    if (length != NULL) *length = 0U;
    if (text == NULL || length == NULL || !valid_record(record)) {
        od_error_set(error, OD_ERROR_INVALID, "invalid history record to render");
        return OD_ERROR_INVALID;
    }
    if (record->version != OD_HISTORY_VERSION) {
        od_error_set(error, OD_ERROR_UNSUPPORTED, "unsupported history record version");
        return OD_ERROR_UNSUPPORTED;
    }
    JsonWriter writer = {.text = malloc(OD_HISTORY_RECORD_MAX_BYTES + 1U)};
    if (writer.text == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to render history record");
        return OD_ERROR_MEMORY;
    }
    write_format(&writer, "{\"v\":%u,\"id\":%" PRIu64 ",\"timestamp\":", record->version, record->id);
    bool valid = write_string(&writer, record->timestamp);
    write_format(&writer, ",\"kind\":\"%s\",\"reverts\":",
                 record->kind == OD_HISTORY_APPLY ? "apply" : "revert");
    if (record->has_reverts) write_format(&writer, "%" PRIu64, record->reverts);
    else write_bytes(&writer, "null", 4U);
    write_bytes(&writer, ",\"file\":", 8U);
    valid = write_string(&writer, record->relative_path) && valid;
    write_format(&writer, ",\"source\":\"%s\",\"write\":\"%s\",\"key\":",
                 record->source_kind == OD_SOURCE_ENV ? "env" : "compose",
                 record->write_kind == OD_WRITE_ENV_LITERAL ? "env_literal" : "compose_literal");
    if (record->environment_key == NULL) write_bytes(&writer, "null", 4U);
    else valid = write_string(&writer, record->environment_key) && valid;
    write_format(&writer, ",\"line\":%zu,\"column\":%zu,\"offset\":%zu,\"length\":%zu,"
                 "\"old_port\":%u,\"new_port\":%u}", record->line, record->column,
                 record->byte_offset, record->byte_length,
                 (unsigned int)record->old_port, (unsigned int)record->new_port);
    if (!valid || writer.oversized) {
        free(writer.text);
        od_error_set(error, OD_ERROR_INVALID, writer.oversized ? "history record exceeds 64 KiB" : "history record contains invalid UTF-8");
        return OD_ERROR_INVALID;
    }
    *text = writer.text;
    *length = writer.length;
    od_error_clear(error);
    return OD_OK;
}

static OdStatus open_root(const char *project_root, int *descriptor, OdError *error) {
    char *canonical = realpath(project_root, NULL);
    if (canonical == NULL) {
        od_error_set(error, OD_ERROR_IO, "unable to resolve history project root: %s", strerror(errno));
        return OD_ERROR_IO;
    }
    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) {
        free(canonical);
        od_error_set(error, OD_ERROR_IO, "unable to open history project root: %s", strerror(errno));
        return OD_ERROR_IO;
    }
    char *save = NULL;
    for (char *part = strtok_r(canonical + 1U, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        int next = openat(current, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            int saved_errno = errno;
            (void)close(current);
            free(canonical);
            od_error_set(error, OD_ERROR_IO, "unable to open history project root: %s", strerror(saved_errno));
            return OD_ERROR_IO;
        }
        (void)close(current);
        current = next;
    }
    free(canonical);
    *descriptor = current;
    return OD_OK;
}

static OdStatus add_warning(OdHistory *history, size_t line,
                             const char *detail, OdError *error) {
    if (history->warning_count >= SIZE_MAX / sizeof(*history->warnings)) {
        od_error_set(error, OD_ERROR_MEMORY, "too many history warnings");
        return OD_ERROR_MEMORY;
    }
    char message[OD_ERROR_MESSAGE_CAP + 64U];
    (void)snprintf(message, sizeof(message), "./.opendoor/history.log:%zu: %s", line, detail);
    char *warning = strdup(message);
    if (warning == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to store history warning");
        return OD_ERROR_MEMORY;
    }
    char **warnings = realloc(history->warnings,
                               (history->warning_count + 1U) * sizeof(*warnings));
    if (warnings == NULL) {
        free(warning);
        od_error_set(error, OD_ERROR_MEMORY, "unable to grow history warnings");
        return OD_ERROR_MEMORY;
    }
    history->warnings = warnings;
    history->warnings[history->warning_count++] = warning;
    return OD_OK;
}

static OdStatus load_line(OdHistory *history, size_t *capacity,
                           const char *text, size_t length, size_t line,
                           OdError *error) {
    OdHistoryRecord record = {0};
    OdError parse_error = {0};
    OdStatus status = od_history_record_parse(text, length, &record, &parse_error);
    if (status == OD_ERROR_MEMORY) {
        od_error_set(error, status, "%s", parse_error.message);
        return status;
    }
    if (status != OD_OK) return add_warning(history, line, parse_error.message, error);
    for (size_t index = 0U; index < history->count; ++index) {
        if (history->items[index].id == record.id) {
            od_history_record_free(&record);
            return add_warning(history, line, "duplicate history record ID", error);
        }
    }
    if (history->count == *capacity) {
        size_t next = *capacity == 0U ? 16U : *capacity * 2U;
        if (next < *capacity || next > SIZE_MAX / sizeof(*history->items)) {
            od_history_record_free(&record);
            od_error_set(error, OD_ERROR_MEMORY, "too many history records");
            return OD_ERROR_MEMORY;
        }
        OdHistoryRecord *items = realloc(history->items, next * sizeof(*items));
        if (items == NULL) {
            od_history_record_free(&record);
            od_error_set(error, OD_ERROR_MEMORY, "unable to grow history records");
            return OD_ERROR_MEMORY;
        }
        history->items = items;
        *capacity = next;
    }
    history->items[history->count++] = record;
    return OD_OK;
}

static int newest_first(const void *left, const void *right) {
    const OdHistoryRecord *first = left;
    const OdHistoryRecord *second = right;
    return first->id > second->id ? -1 : first->id < second->id ? 1 : 0;
}

static OdStatus read_log(int descriptor, OdHistory *history, OdError *error) {
    char *line = malloc(OD_HISTORY_RECORD_MAX_BYTES + 1U);
    if (line == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to read history log");
        return OD_ERROR_MEMORY;
    }
    size_t used = 0U;
    size_t number = 1U;
    size_t capacity = 0U;
    bool oversized = false;
    OdStatus status = OD_OK;
    char chunk[4096];
    while (status == OD_OK) {
        ssize_t count = read(descriptor, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            od_error_set(error, OD_ERROR_IO, "unable to read history log: %s", strerror(errno));
            status = OD_ERROR_IO;
            break;
        }
        if (count == 0) {
            if (used > 0U || oversized) status = add_warning(history, number, "torn final history record skipped", error);
            break;
        }
        for (size_t index = 0U; index < (size_t)count && status == OD_OK; ++index) {
            if (chunk[index] == '\n') {
                if (oversized) status = add_warning(history, number, "history record exceeds 64 KiB", error);
                else status = load_line(history, &capacity, line, used, number, error);
                used = 0U;
                oversized = false;
                ++number;
            } else if (!oversized) {
                if (used == OD_HISTORY_RECORD_MAX_BYTES) oversized = true;
                else line[used++] = chunk[index];
            }
        }
    }
    free(line);
    return status;
}

OdStatus od_history_load(const char *project_root, OdHistory *history,
                          OdError *error) {
    if (history != NULL) *history = (OdHistory){0};
    if (project_root == NULL || history == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "history project root and output are required");
        return OD_ERROR_INVALID;
    }
    int root = -1;
    OdStatus status = open_root(project_root, &root, error);
    if (status != OD_OK) return status;
    int directory = openat(root, ".opendoor", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int saved_errno = errno;
    (void)close(root);
    if (directory < 0) {
        if (saved_errno == ENOENT) { od_error_clear(error); return OD_OK; }
        od_error_set(error, OD_ERROR_IO, "unable to open history directory safely: %s", strerror(saved_errno));
        return OD_ERROR_IO;
    }
    int log = openat(directory, "history.log", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    saved_errno = errno;
    (void)close(directory);
    if (log < 0) {
        if (saved_errno == ENOENT) { od_error_clear(error); return OD_OK; }
        od_error_set(error, OD_ERROR_IO, "unable to open history log safely: %s", strerror(saved_errno));
        return OD_ERROR_IO;
    }
    struct stat information;
    if (fstat(log, &information) != 0 || !S_ISREG(information.st_mode)) {
        (void)close(log);
        od_error_set(error, OD_ERROR_IO, "history log must be a regular file");
        return OD_ERROR_IO;
    }
    status = read_log(log, history, error);
    if (close(log) != 0 && status == OD_OK) {
        od_error_set(error, OD_ERROR_IO, "unable to close history log: %s", strerror(errno));
        status = OD_ERROR_IO;
    }
    if (status != OD_OK) od_history_free(history);
    else {
        if (history->count > 1U) qsort(history->items, history->count, sizeof(*history->items), newest_first);
        od_error_clear(error);
    }
    return status;
}
