#include "opendoor/update.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool regular_file(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISREG(information.st_mode);
}

static bool executable_file(const char *path) {
    return regular_file(path) && access(path, X_OK) == 0;
}

static int run_command(char *const arguments[]) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "opendoor: unable to start %s: %s\n",
                arguments[0], strerror(errno));
        return -1;
    }
    if (child == 0) {
        execvp(arguments[0], arguments);
        fprintf(stderr, "opendoor: unable to run %s: %s\n",
                arguments[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "opendoor: unable to wait for %s: %s\n",
                arguments[0], strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "opendoor: %s was interrupted\n", arguments[0]);
        return -1;
    }
    return WEXITSTATUS(status);
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat information;
    if (stat(path, &information) != 0) return false;
    if (S_ISDIR(information.st_mode)) return true;
    errno = ENOTDIR;
    return false;
}

static char *joined_path(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    bool separator = left_length == 0U || left[left_length - 1U] != '/';
    size_t overhead = separator ? 2U : 1U;
    if (right_length > SIZE_MAX - overhead ||
        left_length > SIZE_MAX - right_length - overhead) {
        return NULL;
    }
    size_t capacity = left_length + right_length + overhead;
    char *path = malloc(capacity);
    if (path == NULL) return NULL;
    (void)snprintf(path, capacity, separator ? "%s/%s" : "%s%s", left, right);
    return path;
}

static bool source_checkout(const char *root) {
    char *cmake = joined_path(root, "CMakeLists.txt");
    char *main_source = joined_path(root, "src/main.c");
    char *app_header = joined_path(root, "include/opendoor/app.h");
    bool valid = cmake != NULL && main_source != NULL && app_header != NULL &&
                 regular_file(cmake) && regular_file(main_source) &&
                 regular_file(app_header);
    free(cmake);
    free(main_source);
    free(app_header);
    return valid;
}

static char *locate_source_checkout(const char *home) {
    char *current = getcwd(NULL, 0U);
    if (current != NULL && source_checkout(current)) return current;
    free(current);

    const char *configured = getenv("OPENDOOR_SOURCE_DIR");
    if (configured != NULL && configured[0] != '\0' &&
        source_checkout(configured)) {
        return strdup(configured);
    }
    char *fallback = joined_path(home, "opendoor");
    if (fallback != NULL && source_checkout(fallback)) return fallback;
    free(fallback);
    return NULL;
}

static bool write_all(int descriptor, const char *buffer, size_t length) {
    size_t written = 0U;
    while (written < length) {
        ssize_t count = write(descriptor, buffer + written, length - written);
        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool install_binary(const char *source, const char *destination_directory) {
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        fprintf(stderr, "opendoor: unable to open built executable: %s\n",
                strerror(errno));
        return false;
    }
    char *temporary = joined_path(destination_directory, ".opendoor-update-XXXXXX");
    char *destination = joined_path(destination_directory, "opendoor");
    if (temporary == NULL || destination == NULL) {
        fputs("opendoor: unable to allocate installation path\n", stderr);
        close(input);
        free(temporary);
        free(destination);
        return false;
    }
    int output = mkstemp(temporary);
    bool success = output >= 0;
    int failure_errno = success ? 0 : errno;
    if (!success) {
        fprintf(stderr, "opendoor: unable to create temporary executable: %s\n",
                strerror(failure_errno));
    }
    char buffer[65536];
    while (success) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            success = false;
            failure_errno = errno;
            break;
        }
        if (!write_all(output, buffer, (size_t)count)) {
            success = false;
            failure_errno = errno;
        }
    }
    if (success && fchmod(output, 0755) != 0) {
        success = false;
        failure_errno = errno;
    }
    if (success && fsync(output) != 0) {
        success = false;
        failure_errno = errno;
    }
    if (close(input) != 0 && success) {
        success = false;
        failure_errno = errno;
    }
    if (output >= 0 && close(output) != 0 && success) {
        success = false;
        failure_errno = errno;
    }
    if (success && rename(temporary, destination) != 0) {
        success = false;
        failure_errno = errno;
    }
    if (!success) {
        (void)unlink(temporary);
        fprintf(stderr, "opendoor: unable to install executable: %s\n",
                strerror(failure_errno == 0 ? EIO : failure_errno));
    } else {
        int directory = open(destination_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            if (fsync(directory) != 0) {
                fprintf(stderr, "opendoor: warning: unable to sync install directory: %s\n",
                        strerror(errno));
            }
            (void)close(directory);
        } else {
            fprintf(stderr, "opendoor: warning: unable to open install directory: %s\n",
                    strerror(errno));
        }
    }
    free(temporary);
    free(destination);
    return success;
}

static bool remove_previous_artifact(const char *path) {
    if (unlink(path) == 0 || errno == ENOENT) return true;
    fprintf(stderr, "opendoor: unable to remove stale build artifact %s: %s\n",
            path, strerror(errno));
    return false;
}

int od_update_current_checkout(void) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        fputs("opendoor: HOME is unavailable; cannot select an install directory\n",
              stderr);
        return 5;
    }
    char *source = locate_source_checkout(home);
    if (source == NULL) {
        fputs("opendoor: unable to locate the OpenDoor source checkout; "
              "run this command from the OpenDoor source checkout or set "
              "OPENDOOR_SOURCE_DIR\n", stderr);
        return 5;
    }
    char *local = joined_path(home, ".local");
    char *bin = local == NULL ? NULL : joined_path(local, "bin");
    char *build_directory = joined_path(source, "build-local");
    char *built_binary = build_directory == NULL ? NULL :
        joined_path(build_directory, "opendoor");
    char *release_directory = build_directory == NULL ? NULL :
        joined_path(build_directory, "Release");
    char *release_binary = release_directory == NULL ? NULL :
        joined_path(release_directory, "opendoor");
    if (local == NULL || bin == NULL || build_directory == NULL ||
        built_binary == NULL || release_directory == NULL ||
        release_binary == NULL) {
        fputs("opendoor: unable to allocate installation directory\n", stderr);
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }

    printf("Configuring OpenDoor from %s...\n", source);
    char *configure[] = {
        "cmake", "-S", source, "-B", build_directory,
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", NULL
    };
    char *build[] = {
        "cmake", "--build", build_directory, "--config", "Release",
        "--target", "opendoor", "--parallel", NULL
    };
    int status = run_command(configure);
    if (status == 0 &&
        (!remove_previous_artifact(built_binary) ||
         !remove_previous_artifact(release_binary))) {
        status = -1;
    }
    if (status == 0) status = run_command(build);
    if (status != 0) {
        fprintf(stderr, "opendoor: update build failed%s\n",
                status > 0 ? "" : " to start");
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }
    const char *new_binary = executable_file(built_binary) ? built_binary :
        (executable_file(release_binary) ? release_binary : NULL);
    if (new_binary == NULL) {
        fputs("opendoor: build completed without producing an executable\n", stderr);
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }
    if (!ensure_directory(local)) {
        fprintf(stderr, "opendoor: unable to create %s: %s\n",
                local, strerror(errno));
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }
    if (!ensure_directory(bin)) {
        fprintf(stderr, "opendoor: unable to create %s: %s\n",
                bin, strerror(errno));
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }
    if (!install_binary(new_binary, bin)) {
        free(source);
        free(local);
        free(bin);
        free(build_directory);
        free(built_binary);
        free(release_directory);
        free(release_binary);
        return 5;
    }
    printf("Update complete: %s/opendoor\n", bin);
    free(source);
    free(local);
    free(bin);
    free(build_directory);
    free(built_binary);
    free(release_directory);
    free(release_binary);
    return 0;
}
