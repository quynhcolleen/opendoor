#include "opendoor/common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void od_error_clear(OdError *error) {
    if (error != NULL) {
        error->code = OD_OK;
        error->message[0] = '\0';
    }
}

void od_error_set(OdError *error, OdStatus code, const char *format, ...) {
    if (error == NULL) {
        return;
    }
    error->code = code;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

