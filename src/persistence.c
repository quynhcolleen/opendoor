#include "opendoor/persistence.h"

#include "opendoor/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

OdStatus od_config_write(const char *path,
                         const OdAssignments *assignments,
                         OdError *error) {
    if (path == NULL || assignments == NULL) {
        od_error_set(error, OD_ERROR_INVALID, "wanted-ports path and values are required");
        return OD_ERROR_INVALID;
    }
    char *text = NULL;
    size_t length = 0U;
    OdStatus status = od_config_render(assignments, &text, &length, error);
    if (status != OD_OK) return status;

    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        free(text);
        od_error_set(error, OD_ERROR_IO, "unable to open %s: %s", path, strerror(errno));
        return OD_ERROR_IO;
    }
    size_t offset = 0U;
    while (offset < length) {
        ssize_t written = write(descriptor, text + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            status = OD_ERROR_IO;
            od_error_set(error, status, "unable to write %s: %s", path, strerror(errno));
            break;
        }
        offset += (size_t)written;
    }
    if (close(descriptor) != 0 && status == OD_OK) {
        status = OD_ERROR_IO;
        od_error_set(error, status, "unable to close %s: %s", path, strerror(errno));
    }
    free(text);
    if (status == OD_OK) od_error_clear(error);
    return status;
}
