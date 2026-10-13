#include "opendoor/scan.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <fcntl.h>
#include <unistd.h>

static int failures;
static int fault;
static int diagnostic_fd = -1;
static unsigned sequence;
static bool sent_bad_frame;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
int __real_socket(int domain, int type, int protocol);
FILE *__real_fopen(const char *path, const char *mode);
ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);

int __wrap_socket(int domain, int type, int protocol) {
    if (domain == AF_NETLINK) {
        if (fault >= 3 && fault != 6) { diagnostic_fd = open("/dev/null", O_RDONLY); return diagnostic_fd; }
        errno = EACCES; return -1;
    }
    return __real_socket(domain, type, protocol);
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags) {
    if (fd != diagnostic_fd) return __real_send(fd, buffer, length, flags);
    sequence = ((const struct nlmsghdr *)buffer)->nlmsg_seq;
    sent_bad_frame = false;
    return (ssize_t)length;
}

ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags) {
    if (fd != diagnostic_fd) return __real_recv(fd, buffer, length, flags);
    if (fault == 5 && sequence == 2U) { errno = EIO; return -1; }
    struct nlmsghdr done = {.nlmsg_len = NLMSG_LENGTH(sizeof(int)), .nlmsg_type = NLMSG_DONE,
        .nlmsg_seq = sequence, .nlmsg_flags = fault == 3 || fault == 5 ? NLM_F_DUMP_INTR : 0U};
    if (fault == 7 || fault == 10) {
        int dump_error = -ENOBUFS;
        done.nlmsg_len = NLMSG_LENGTH(fault == 7 ? sizeof(dump_error) : 1U);
        memcpy(buffer, &done, sizeof(done));
        memcpy((char *)buffer + sizeof(done), &dump_error, fault == 7 ? sizeof(dump_error) : 1U);
        return (ssize_t)done.nlmsg_len;
    }
    if ((fault == 8 || fault == 9 || fault == 11) && !sent_bad_frame) {
        sent_bad_frame = true;
        struct nlmsghdr bad = {.nlmsg_len = fault == 9 ? 8U : sizeof(bad),
            .nlmsg_type = fault == 11 ? NLMSG_OVERRUN : SOCK_DIAG_BY_FAMILY, .nlmsg_seq = sequence};
        memcpy(buffer, &bad, sizeof(bad));
        return (ssize_t)sizeof(bad);
    }
    if (fault == 12) done.nlmsg_len = sizeof(done);
    memcpy(buffer, &done, sizeof(done));
    int success = 0;
    memcpy((char *)buffer + sizeof(done), &success, sizeof(success));
    return fault == 4 ? (ssize_t)length + 1 : (ssize_t)done.nlmsg_len;
}

FILE *__wrap_fopen(const char *path, const char *mode) {
    if (strncmp(path, "/proc/net/", 10U) != 0) return __real_fopen(path, mode);
    if (fault == 1 && strcmp(path, "/proc/net/tcp6") == 0) { errno = EACCES; return NULL; }
    FILE *file = tmpfile();
    if (file != NULL) {
        if (fault != 6) fputs("  sl  local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n", file);
        if (fault == 2 && strcmp(path, "/proc/net/tcp") == 0) fputs("malformed socket row\n", file);
        rewind(file);
    }
    return file;
}

int main(void) {
    OdError error;
    OdScanSnapshot snapshot = {0};
    CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
    CHECK(snapshot.report.total_count == 1U);
    CHECK(snapshot.report.completeness == OD_SCAN_INCOMPLETE);
    CHECK(!od_scan_report_blocks_apply(&snapshot.report));
    od_scan_snapshot_free(&snapshot);
    fault = 6;
    CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
    CHECK(od_scan_report_blocks_apply(&snapshot.report));
    CHECK(snapshot.endpoint_count == 0U);
    od_scan_snapshot_free(&snapshot);
    fault = 1;
    CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
    CHECK(snapshot.report.total_count == 2U && snapshot.report.blocking_count == 1U);
    CHECK(od_scan_report_blocks_apply(&snapshot.report));
    if (snapshot.report.count == 2U) CHECK(strstr(snapshot.report.items[1].text, "IPv6 TCP") != NULL);
    od_scan_snapshot_free(&snapshot);
    fault = 2;
    CHECK(od_scan_sockets(&snapshot, &error) != OD_OK);
    CHECK(od_scan_report_blocks_apply(&snapshot.report));
    od_scan_snapshot_free(&snapshot);
    for (fault = 3; fault <= 4; ++fault) {
        CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
        CHECK(snapshot.report.blocking_count > 0U);
        od_scan_snapshot_free(&snapshot);
    }
    /* A complete fallback replaces the partial kernel result. Stale warnings
     * from the discarded result must not block verification of the new one. */
    fault = 5;
    CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
    CHECK(snapshot.report.total_count == 1U);
    CHECK(!od_scan_report_blocks_apply(&snapshot.report));
    od_scan_snapshot_free(&snapshot);
    for (fault = 7; fault <= 12; ++fault) {
        CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
        CHECK(snapshot.report.completeness == OD_SCAN_INCOMPLETE);
        CHECK(od_scan_report_blocks_apply(&snapshot.report));
        od_scan_snapshot_free(&snapshot);
    }
    fault = 13;
    CHECK(od_scan_sockets(&snapshot, &error) == OD_OK);
    CHECK(snapshot.report.completeness == OD_SCAN_COMPLETE);
    CHECK(snapshot.report.total_count == 0U);
    od_scan_snapshot_free(&snapshot);
    return failures == 0 ? 0 : 1;
}
