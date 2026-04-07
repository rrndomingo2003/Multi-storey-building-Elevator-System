#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

int writen(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) return -1;
        p += (size_t)k; n -= (size_t)k;
    }
    return 0;
}

int readn(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        if (k == 0) return -1; // EOF
        p += (size_t)k; n -= (size_t)k;
    }
    return 0;
}

int send_msg(int fd, const char *s) {
    size_t len = strlen(s);
    if (len > 0xFFFFu) { errno = EMSGSIZE; return -1; }
    uint16_t n = htons((uint16_t)len);
    if (writen(fd, &n, sizeof n) < 0) return -1;
    return writen(fd, s, len);
}

int recv_msg(int fd, char **out) {
    uint16_t n_net;
    if (readn(fd, &n_net, sizeof n_net) < 0) return -1;
    size_t len = (size_t)ntohs(n_net);
    char *buf = malloc(len + 1u);
    if (!buf) return -1;
    if (readn(fd, buf, len) < 0) { int e = errno; free(buf); errno = e; return -1; }
    buf[len] = '\0';
    *out = buf;
    return 0;
}
