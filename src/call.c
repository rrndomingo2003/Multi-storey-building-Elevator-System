#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "protocol.h"   // send_msg / recv_msg (16-bit)
#include "floor.h"      // parse_floor()
int tcp_connect_ipv4(const char *ip, unsigned short port); // from common/net.c

static int is_valid_floor_token(const char *s) {
    // Mirror the testers: 1..3 chars, either digits or 'B'+digits.
    if (!s) return 0;
    size_t n = strlen(s);
    if (n == 0 || n > 3) return 0;

    unsigned char c0 = (unsigned char)s[0];
    if (c0 == 'B') {
        if (n == 1) return 0; // "B" alone is invalid
        for (size_t i = 1; i < n; ++i) {
            if (!isdigit((unsigned char)s[i])) return 0;
        }
        return 1;
    }
    // First char must be digit if not 'B'
    if (!isdigit(c0)) return 0;
    for (size_t i = 1; i < n; ++i) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: {source floor} {destination floor}\n");
        return EXIT_FAILURE;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    if (!is_valid_floor_token(src) || !is_valid_floor_token(dst)) {
        printf("Invalid floor(s) specified.\n");
        return EXIT_FAILURE;
    }
    if (strcmp(src, dst) == 0) {
        printf("You are already on that floor!\n");
        return EXIT_FAILURE;
    }

    // Connect to controller
    int fd = tcp_connect_ipv4("127.0.0.1", 3000);
    if (fd < 0) {
        printf("Unable to connect to elevator system.\n");
        return EXIT_FAILURE;
    }

    // Send CALL (16-bit framed)
    char line[32];
    // src/dst are max 3 chars each
    snprintf(line, sizeof line, "CALL %s %s", src, dst);
    if (send_msg(fd, line) < 0) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    // Receive response
    char *resp = NULL;
    if (recv_msg(fd, &resp) < 0 || !resp) {
        printf("Unable to connect to elevator system.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    // Interpret response
    if (strncmp(resp, "UNAVAILABLE", 11) == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else if (strncmp(resp, "CAR ", 4) == 0) {
        // resp = "CAR <name>"
        const char *name = resp + 4;
        printf("Car %s is arriving.\n", name);
    } else {}

    free(resp);
    close(fd);
    return EXIT_SUCCESS;
}
