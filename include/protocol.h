#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Robust, blocking I/O helpers for TCP (return 0 on success, -1 on error). */
int writen(int fd, const void *buf, size_t n);   // write exactly n bytes
int readn(int fd, void *buf, size_t n);          // read exactly n bytes

/* 16-bit length-prefixed framing. */
int send_msg(int fd, const char *s);             // send strlen(s) bytes + 16-bit length
int recv_msg(int fd, char **out);                // mallocs *out (NUL-terminated); caller frees

#endif
