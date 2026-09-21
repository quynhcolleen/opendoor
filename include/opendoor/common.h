#ifndef OPENDOOR_COMMON_H
#define OPENDOOR_COMMON_H

#include <stddef.h>

#define OD_ERROR_MESSAGE_CAP 256U

typedef enum {
    OD_OK = 0,
    OD_ERROR_INVALID,
    OD_ERROR_IO,
    OD_ERROR_MEMORY,
    OD_ERROR_FOREIGN,
    OD_ERROR_UNSUPPORTED,
    OD_ERROR_CONFLICT,
    OD_ERROR_EXHAUSTED,
    OD_ERROR_CHANGED,
    OD_ERROR_TIMEOUT,
    OD_ERROR_CANCELLED
} OdStatus;

typedef struct {
    OdStatus code;
    char message[OD_ERROR_MESSAGE_CAP];
} OdError;

void od_error_clear(OdError *error);
void od_error_set(OdError *error, OdStatus code, const char *format, ...);

#endif

