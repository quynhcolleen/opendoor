#include "opendoor/history.h"
#include "opendoor/persistence.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

typedef struct {
    int log;
    off_t original_length;
    uint64_t next_id;
    OdHistoryKind kind;
    uint64_t reverts;
    bool rollback_incomplete;
} HistoryTransaction;

typedef struct {
    char *text;
    size_t length;
} HistoryTailLine;

static OdStatus history_io_error(OdError *error, const char *operation) {
    od_error_set(error, OD_ERROR_IO, "%s: %s", operation, strerror(errno));
    return OD_ERROR_IO;
}

static OdStatus open_history_transaction(const char *project_root,
                                          size_t count,
                                          HistoryTransaction *transaction,
                                          OdError *error) {
    int root = -1;
    OdStatus status = open_root(project_root, &root, error);
    if (status != OD_OK) return status;
    if (mkdirat(root, ".opendoor", 0700) != 0 && errno != EEXIST) {
        status = history_io_error(error, "unable to create history directory");
        (void)close(root);
        return status;
    }
    int directory = openat(root, ".opendoor", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) status = history_io_error(error, "unable to open history directory safely");
    if (status == OD_OK && fsync(root) != 0) status = history_io_error(error, "unable to sync history project root");
    (void)close(root);
    if (status != OD_OK) {
        if (directory >= 0) (void)close(directory);
        return status;
    }
    transaction->log = openat(directory, "history.log",
        O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (transaction->log < 0) status = history_io_error(error, "unable to open history log safely");
    struct stat information;
    if (status == OD_OK &&
        (fstat(transaction->log, &information) != 0 || !S_ISREG(information.st_mode))) {
        od_error_set(error, OD_ERROR_IO, "history log must be a regular file");
        status = OD_ERROR_IO;
    }
    if (status == OD_OK && fsync(directory) != 0) status = history_io_error(error, "unable to sync history directory");
    (void)close(directory);
    if (status != OD_OK) return status;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int locked;
    do { locked = fcntl(transaction->log, F_SETLKW, &lock); } while (locked < 0 && errno == EINTR);
    if (locked < 0) return history_io_error(error, "unable to lock history log");
    if (fstat(transaction->log, &information) != 0 || information.st_size < 0) {
        return history_io_error(error, "unable to size history log");
    }
    transaction->original_length = information.st_size;
    if (information.st_size > 0) {
        char last;
        ssize_t read_count;
        do { read_count = pread(transaction->log, &last, 1U, information.st_size - 1); }
        while (read_count < 0 && errno == EINTR);
        if (read_count != 1) return history_io_error(error, "unable to read history tail");
        if (last != '\n') {
            od_error_set(error, OD_ERROR_CHANGED,
                         "history log has a torn final record; retained without writing sources");
            return OD_ERROR_CHANGED;
        }
    }
    OdHistory existing = {0};
    status = read_log(transaction->log, &existing, error);
    uint64_t maximum = 0U;
    if (status == OD_OK) {
        for (size_t index = 0U; index < existing.count; ++index) {
            if (existing.items[index].id > maximum) maximum = existing.items[index].id;
        }
        if (maximum > UINT64_MAX - count) {
            od_error_set(error, OD_ERROR_INVALID, "history record IDs are exhausted");
            status = OD_ERROR_INVALID;
        } else transaction->next_id = maximum + 1U;
    }
    od_history_free(&existing);
    return status;
}

static OdStatus append_history_bytes(int log, const char *text, size_t length,
                                      OdError *error) {
    size_t used = 0U;
    while (used < length) {
        ssize_t count = write(log, text + used, length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return history_io_error(error, "unable to append history record");
        used += (size_t)count;
    }
    return OD_OK;
}

static OdStatus prepare_history_tail(const OdCommittedEdit *edits, size_t count,
                                      const HistoryTransaction *transaction,
                                      HistoryTailLine *lines, OdError *error) {
    struct timespec now;
    struct tm utc;
    char timestamp[25], seconds[21];
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || gmtime_r(&now.tv_sec, &utc) == NULL ||
        strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc) != 19U) {
        od_error_set(error, OD_ERROR_IO, "unable to timestamp history records");
        return OD_ERROR_IO;
    }
    (void)snprintf(timestamp, sizeof(timestamp), "%.19s.%03uZ", seconds,
                   (unsigned int)(now.tv_nsec / 1000000L) % 1000U);
    for (size_t index = 0U; index < count; ++index) {
        const OdCommittedEdit *edit = &edits[index];
        const OdResolutionItem *item = edit->item;
        OdHistoryRecord record = {
            .version = OD_HISTORY_VERSION, .id = transaction->next_id + index,
            .timestamp = timestamp, .kind = transaction->kind,
            .has_reverts = transaction->kind == OD_HISTORY_REVERT,
            .reverts = transaction->reverts, .relative_path = item->relative_path,
            .source_kind = item->source_kind, .write_kind = item->write_kind,
            .environment_key = item->variable != NULL && item->variable[0] != '\0' ? item->variable : NULL,
            .line = edit->line, .column = edit->column,
            .byte_offset = edit->byte_offset, .byte_length = edit->byte_length,
            .old_port = item->old_port, .new_port = item->new_port
        };
        OdStatus status = od_history_record_render(&record, &lines[index].text,
                                                   &lines[index].length, error);
        if (status != OD_OK) return status;
        /* Renderer reserves a terminating byte; use it for the JSONL newline. */
        lines[index].text[lines[index].length++] = '\n';
    }
    return OD_OK;
}

static OdStatus validate_history_tail(const HistoryTransaction *transaction,
                                       const HistoryTailLine *lines, size_t count,
                                       OdError *error) {
    char *actual = malloc(OD_HISTORY_RECORD_MAX_BYTES + 1U);
    if (actual == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to validate appended history");
        return OD_ERROR_MEMORY;
    }
    off_t position = transaction->original_length;
    OdStatus status = OD_OK;
    for (size_t index = 0U; index < count && status == OD_OK; ++index) {
        size_t used = 0U;
        while (used < lines[index].length) {
            ssize_t received = pread(transaction->log, actual + used,
                                      lines[index].length - used, position);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) {
                od_error_set(error, OD_ERROR_IO, "unable to read appended history record");
                status = OD_ERROR_IO;
                break;
            }
            used += (size_t)received;
            position += received;
        }
        if (status != OD_OK) break;
        if (memcmp(actual, lines[index].text, used) != 0) {
            od_error_set(error, OD_ERROR_CHANGED, "appended history bytes changed during commit");
            status = OD_ERROR_CHANGED;
            break;
        }
        OdHistoryRecord record = {0};
        status = od_history_record_parse(actual, used - 1U, &record, error);
        od_history_record_free(&record);
    }
    free(actual);
    struct stat information;
    if (status == OD_OK &&
        (fstat(transaction->log, &information) != 0 || information.st_size != position)) {
        od_error_set(error, OD_ERROR_CHANGED, "history length changed during commit");
        status = OD_ERROR_CHANGED;
    }
    return status;
}

static OdStatus commit_history(const OdCommittedEdit *edits, size_t count,
                                 void *opaque, OdError *error) {
    HistoryTransaction *transaction = opaque;
    HistoryTailLine *lines = calloc(count, sizeof(*lines));
    if (lines == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to prepare history append");
        return OD_ERROR_MEMORY;
    }
    OdStatus status = prepare_history_tail(edits, count, transaction, lines, error);
    bool append_started = false;
    for (size_t index = 0U; index < count && status == OD_OK; ++index) {
        append_started = true;
        status = append_history_bytes(transaction->log, lines[index].text,
                                        lines[index].length, error);
    }
    if (status == OD_OK && fsync(transaction->log) != 0) {
        status = history_io_error(error, "unable to sync appended history");
    }
    if (status == OD_OK) status = validate_history_tail(transaction, lines, count, error);
    for (size_t index = 0U; index < count; ++index) free(lines[index].text);
    free(lines);
    if (status != OD_OK && append_started) {
        /* Only this uncommitted tail may be removed, never a successful record. */
        bool restored = ftruncate(transaction->log, transaction->original_length) == 0;
        if (fsync(transaction->log) != 0) restored = false;
        struct stat information;
        if (fstat(transaction->log, &information) != 0 ||
            information.st_size != transaction->original_length) restored = false;
        transaction->rollback_incomplete = !restored;
    }
    return status;
}

OdStatus od_history_apply_resolution(const OdResolution *resolution,
                                      OdHistoryKind kind, const uint64_t *reverts,
                                      size_t *updated, OdError *error) {
    if (updated != NULL) *updated = 0U;
    if (resolution == NULL || updated == NULL ||
        (resolution->count > 0U && resolution->items == NULL) ||
        (kind != OD_HISTORY_APPLY && kind != OD_HISTORY_REVERT) ||
        (kind == OD_HISTORY_APPLY && reverts != NULL) ||
        (kind == OD_HISTORY_REVERT && (reverts == NULL || *reverts == 0U))) {
        od_error_set(error, OD_ERROR_INVALID, "invalid recorded resolution or history kind");
        return OD_ERROR_INVALID;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < resolution->count; ++index) {
        if (resolution->items[index].automatic) ++count;
    }
    if (count == 0U) return od_apply_resolution(resolution, updated, error);
    if (resolution->project_root == NULL || (kind == OD_HISTORY_REVERT && count != 1U)) {
        od_error_set(error, OD_ERROR_INVALID, "recorded edits require a project root and reverts require one edit");
        return OD_ERROR_INVALID;
    }
    HistoryTransaction transaction = {.log = -1, .kind = kind,
                                       .reverts = reverts == NULL ? 0U : *reverts};
    OdStatus status = open_history_transaction(resolution->project_root, count, &transaction, error);
    if (status == OD_OK && kind == OD_HISTORY_REVERT && *reverts >= transaction.next_id) {
        od_error_set(error, OD_ERROR_INVALID, "revert target must precede the new history record");
        status = OD_ERROR_INVALID;
    }
    if (status == OD_OK) {
        status = od_apply_resolution_with_commit(resolution, NULL, NULL,
                                                  commit_history, &transaction,
                                                  updated, error);
    }
    if (transaction.rollback_incomplete) {
        bool sources_incomplete = strstr(error->message, "rollback incomplete") != NULL;
        od_error_set(error, status,
                     "history log rollback incomplete; source rollback %s; backups retained; inspect history.log before retrying",
                     sources_incomplete ? "incomplete" : "completed");
    }
    /* The commit hook has already fsynced and validated the log while source
     * rollback was live. Closing releases the advisory lock after persistence. */
    if (transaction.log >= 0) (void)close(transaction.log);
    return status;
}

static OdStatus set_availability(OdHistoryRecord *record, const char *reason,
                                  OdError *error) {
    char *owned_reason = reason == NULL ? NULL : strdup(reason);
    if (reason != NULL && owned_reason == NULL) {
        record->availability = OD_HISTORY_UNCHECKED;
        od_error_set(error, OD_ERROR_MEMORY, "unable to store history availability reason");
        return OD_ERROR_MEMORY;
    }
    free(record->reason);
    record->reason = owned_reason;
    record->availability = reason == NULL ? OD_HISTORY_READY : OD_HISTORY_UNAVAILABLE;
    od_error_clear(error);
    return OD_OK;
}

static bool same_key(const char *left, const char *right) {
    return strcmp(left == NULL ? "" : left, right == NULL ? "" : right) == 0;
}

static bool auto_writable(const OdPortDeclaration *declaration) {
    return declaration->declaration_kind == OD_DECLARATION_LITERAL &&
           ((declaration->source_kind == OD_SOURCE_ENV &&
             declaration->write_kind == OD_WRITE_ENV_LITERAL) ||
            (declaration->source_kind == OD_SOURCE_COMPOSE &&
             declaration->write_kind == OD_WRITE_COMPOSE_LITERAL));
}

static OdStatus classify_record(OdHistoryRecord *record,
                                 const OdProjectDiscovery *discovery,
                                 const OdPortDeclaration **match,
                                 OdError *error) {
    if (match != NULL) *match = NULL;
    if (record == NULL || record->relative_path == NULL || discovery == NULL ||
        (discovery->count > 0U && discovery->items == NULL)) {
        od_error_set(error, OD_ERROR_INVALID, "history record and discovery are required");
        return OD_ERROR_INVALID;
    }
    const OdPortDeclaration *exact = NULL;
    size_t exact_count = 0U;
    bool moved = false, changed_length = false, changed_key = false;
    bool changed_kind = false, manual = false;
    for (size_t index = 0U; index < discovery->count; ++index) {
        const OdPortDeclaration *declaration = &discovery->items[index];
        if (declaration->relative_path == NULL ||
            strcmp(declaration->relative_path, record->relative_path) != 0) continue;
        bool key_matches = same_key(declaration->environment_key, record->environment_key);
        bool kinds_match = declaration->source_kind == record->source_kind &&
                           declaration->write_kind == record->write_kind;
        if (declaration->byte_offset != record->byte_offset) {
            if (key_matches && kinds_match) moved = true;
            continue;
        }
        if (declaration->byte_length != record->byte_length) {
            changed_length = true;
            continue;
        }
        if (declaration->declaration_kind != OD_DECLARATION_LITERAL ||
            declaration->write_kind == OD_WRITE_MANUAL_ONLY) manual = true;
        if (!key_matches) changed_key = true;
        if (!kinds_match) changed_kind = true;
        if (key_matches && kinds_match) {
            exact = declaration;
            ++exact_count;
        }
    }
    if (exact_count > 1U) {
        return set_availability(record, "Ambiguous declaration: multiple matches at the recorded span", error);
    }
    if (exact != NULL) {
        if (!auto_writable(exact)) {
            return set_availability(record, "Declaration is no longer automatically writable", error);
        }
        if (exact->port != record->new_port) {
            char reason[80];
            (void)snprintf(reason, sizeof(reason), "Current port is %u; expected %u",
                           (unsigned int)exact->port, (unsigned int)record->new_port);
            return set_availability(record, reason, error);
        }
        OdStatus status = set_availability(record, NULL, error);
        if (status == OD_OK && match != NULL) *match = exact;
        return status;
    }
    const char *reason = "Declaration is missing or moved from the recorded span";
    if (manual) reason = "Declaration is no longer automatically writable";
    else if (changed_kind) reason = "Declaration source or write kind changed at the recorded span";
    else if (changed_key) reason = "Declaration key changed at the recorded span";
    else if (changed_length) reason = "Declaration byte length changed at the recorded offset";
    else if (moved) reason = "Declaration moved from the recorded byte offset";
    return set_availability(record, reason, error);
}

OdStatus od_history_classify_record(OdHistoryRecord *record,
                                     const OdProjectDiscovery *discovery,
                                     OdError *error) {
    return classify_record(record, discovery, NULL, error);
}

/* Check every path component under the opened root, without following links.
 * A nonblocking final open also makes FIFOs and devices safe to reject. */
static OdStatus source_safety(int root, OdHistoryRecord *record, bool *safe,
                               OdError *error) {
    *safe = false;
    const char *path = record->relative_path;
    if (!valid_relative_path(path) || strncmp(path, "./", 2U) != 0 ||
        path[2] == '\0' || strstr(path, "//") != NULL) {
        return set_availability(record, "Source file path is unsafe", error);
    }
    char *copy = strdup(path + 2U);
    if (copy == NULL) {
        od_error_set(error, OD_ERROR_MEMORY, "unable to inspect history source path");
        return OD_ERROR_MEMORY;
    }
    int current = dup(root);
    if (current < 0) {
        free(copy);
        return history_io_error(error, "unable to inspect history source directory");
    }
    const char *reason = NULL;
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash != NULL) *slash = '\0';
        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            reason = "Source file path is unsafe";
            break;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (slash != NULL) flags |= O_DIRECTORY;
        int next = openat(current, part, flags);
        if (next < 0) {
            reason = errno == ENOENT ? "Source file is missing" :
                                      "Source file is unsafe or unreadable";
            break;
        }
        (void)close(current);
        current = next;
        if (slash == NULL) {
            struct stat information;
            if (fstat(current, &information) != 0 || !S_ISREG(information.st_mode)) {
                reason = "Source file is unsafe or is not a regular file";
            }
            break;
        }
        part = slash + 1U;
    }
    (void)close(current);
    free(copy);
    if (reason != NULL) return set_availability(record, reason, error);
    *safe = true;
    return OD_OK;
}

static OdStatus classify_current_record(int root, OdHistoryRecord *record,
                                         const OdProjectDiscovery *discovery,
                                         const OdPortDeclaration **match,
                                         OdError *error) {
    if (match != NULL) *match = NULL;
    bool safe = false;
    OdStatus status = source_safety(root, record, &safe, error);
    if (status == OD_OK && safe) status = classify_record(record, discovery, match, error);
    return status;
}

OdStatus od_history_refresh(const char *project_root, OdHistory *history,
                             OdError *error) {
    if (project_root == NULL || history == NULL ||
        (history->count > 0U && history->items == NULL)) {
        od_error_set(error, OD_ERROR_INVALID, "history project root and records are required");
        return OD_ERROR_INVALID;
    }
    /* A failed refresh must not leave previously ready rows actionable. */
    for (size_t index = 0U; index < history->count; ++index) {
        history->items[index].availability = OD_HISTORY_UNCHECKED;
    }
    int root = -1;
    OdProjectDiscovery discovery = {0};
    OdStatus status = open_root(project_root, &root, error);
    if (status == OD_OK) status = od_discover_project_ports(project_root, &discovery, error);
    for (size_t index = 0U; status == OD_OK && index < history->count; ++index) {
        status = classify_current_record(root, &history->items[index], &discovery, NULL, error);
    }
    od_project_discovery_free(&discovery);
    if (root >= 0) (void)close(root);
    if (status == OD_OK) od_error_clear(error);
    return status;
}

OdStatus od_history_revert(const char *project_root, uint64_t record_id,
                            size_t *updated, OdError *error) {
    if (updated != NULL) *updated = 0U;
    if (project_root == NULL || record_id == 0U || updated == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "history project root, record ID and update count are required");
        return OD_ERROR_INVALID;
    }
    char *canonical = realpath(project_root, NULL);
    if (canonical == NULL) return history_io_error(error, "unable to resolve history project root");
    OdHistory history = {0};
    OdProjectDiscovery discovery = {0};
    int root = -1;
    OdStatus status = od_history_load(canonical, &history, error);
    OdHistoryRecord *record = NULL;
    for (size_t index = 0U; status == OD_OK && index < history.count; ++index) {
        if (history.items[index].id == record_id) {
            record = &history.items[index];
            break;
        }
    }
    if (status == OD_OK && record == NULL) {
        od_error_set(error, OD_ERROR_CHANGED, "History record %" PRIu64 " was not found", record_id);
        status = OD_ERROR_CHANGED;
    }
    if (status == OD_OK) status = open_root(canonical, &root, error);
    if (status == OD_OK) status = od_discover_project_ports(canonical, &discovery, error);
    const OdPortDeclaration *declaration = NULL;
    if (status == OD_OK) {
        status = classify_current_record(root, record, &discovery, &declaration, error);
    }
    if (status == OD_OK && record->availability != OD_HISTORY_READY) {
        od_error_set(error, OD_ERROR_CHANGED, "%s", record->reason);
        status = OD_ERROR_CHANGED;
    }
    if (status == OD_OK) {
        /* Borrow only fresh discovery metadata. Persistence performs the final
         * file hash/span preflight, source mutation and rollback. */
        OdResolutionItem item = {
            .variable = declaration->environment_key,
            .old_port = declaration->port, .new_port = record->old_port,
            .automatic = true, .source_kind = declaration->source_kind,
            .write_kind = declaration->write_kind,
            .line = declaration->line, .column = declaration->column,
            .byte_offset = declaration->byte_offset, .byte_length = declaration->byte_length,
            .file_size = declaration->file_size, .file_hash = declaration->file_hash,
            .absolute_path = declaration->absolute_path, .relative_path = declaration->relative_path
        };
        OdResolution resolution = {.items = &item, .count = 1U, .automatic_count = 1U,
                                     .project_root = canonical};
        status = od_history_apply_resolution(&resolution, OD_HISTORY_REVERT, &record_id,
                                              updated, error);
    }
    if (root >= 0) (void)close(root);
    od_project_discovery_free(&discovery);
    od_history_free(&history);
    free(canonical);
    return status;
}
