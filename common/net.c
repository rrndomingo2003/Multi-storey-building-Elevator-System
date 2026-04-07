#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

/* Connect to IPv4 TCP; returns fd or -1 (errno set). */
int tcp_connect_ipv4(const char *ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { int e=errno; close(fd); errno = e?e:EINVAL; return -1; }
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0) { int e=errno; close(fd); errno = e; return -1; }
    return fd;
}
