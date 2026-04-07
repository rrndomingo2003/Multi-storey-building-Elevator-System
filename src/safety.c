#define _POSIX_C_SOURCE 200809L

/* Safety – MISRA-ish guarded elevator safety monitor
 *
 * Deviations recorded:
 *  - Uses <stdio.h> only for snprintf; all output via write() (Rule family 21).
 *  - Infinite loop for real-time monitoring (documented).
 *  - Uses pthread APIs and process-shared sync (project requirement).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>   /* snprintf only (bounded) */

/* ===== Local mirror of tester's shared-memory layout =====
 * Keep field order/types EXACT. Do NOT insert extra fields.
 * Floors are short strings (e.g., "B4", "12"); status is a string.
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    char current_floor[4];
    char destination_floor[4];
    char status[8]; /* "Opening"|"Open"|"Closing"|"Closed"|"Between" */

    uint8_t open_button;
    uint8_t close_button;
    uint8_t door_obstruction;
    uint8_t overload;
    uint8_t emergency_stop;
    uint8_t individual_service_mode;
    uint8_t emergency_mode;
} car_shared_mem;

/* ===== Constants (no magic numbers) ===== */
enum { STATUS_LEN = 8, FLO_LEN = 4 };
#define U8_MAX_FLAG 1u

/* ===== Small, bounded I/O helpers (MISRA-friendly-ish) ===== */
/* Write the whole buffer, handling EINTR/short writes. Returns 0 on success, -1 on error. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(fd, buf + off, len - off);
        if (k < 0) {
            if (errno == EINTR) { continue; }
            return -1;
        }
        off += (size_t)k;
    }
    return 0;
}

static void wputs(const char *s) {
    if (s != NULL) {
        (void)write_all(STDOUT_FILENO, s, strlen(s));
    }
}

static void wperr(const char *s) {
    if (s != NULL) {
        (void)write_all(STDERR_FILENO, s, strlen(s));
    }
}


/* ===== Validation helpers ===== */

static int is_valid_flag(uint8_t v) { return (v == 0u) || (v == 1u); }

/* Floors: "", "1".."999" or 'B' + digits (length <=3). */
static int is_valid_floor(const char *f) {
    if (f == NULL) { return 0; }
    const size_t len = strlen(f);
    if (len == 0u || len > 3u) { return 0; }

    /* First char may be 'B' or digit */
    const unsigned char c0 = (unsigned char)f[0];
    if (!isdigit(c0) && (c0 != (unsigned char)'B')) { return 0; }

    /* Rest must be digits (if any) */
    for (size_t i = 1u; i < len; i++) {
        const unsigned char ci = (unsigned char)f[i];
        if (!isdigit(ci)) { return 0; }
    }
    return 1;
}

static int is_valid_status(const char *s) {
    /* exact tokens expected by testers */
    static const char * const ok[] = { "Opening", "Open", "Closing", "Closed", "Between" };
    if (s == NULL) { return 0; }
    for (size_t i = 0u; i < (sizeof ok / sizeof ok[0]); ++i) {
        if (strcmp(s, ok[i]) == 0) { return 1; }
    }
    return 0;
}

/* ===== Rule enforcements (all called with mutex held) ===== */

static void set_status(car_shared_mem *m, const char *new_status) {
    /* write bounded, always NUL-terminated */
    (void)snprintf(m->status, (size_t)STATUS_LEN, "%s", new_status);
}

/* Consistency check; returns 1 if consistent, 0 if not */
static int check_consistency(const car_shared_mem *m) {
    if (m == NULL) { return 0; }

    /* Flags must be 0/1 */
    if (!is_valid_flag(m->open_button) ||
        !is_valid_flag(m->close_button) ||
        !is_valid_flag(m->door_obstruction) ||
        !is_valid_flag(m->overload) ||
        !is_valid_flag(m->emergency_stop) ||
        !is_valid_flag(m->individual_service_mode) ||
        !is_valid_flag(m->emergency_mode)) {
        return 0;
    }

    /* Floors & status must be valid strings */
    if (!is_valid_floor(m->current_floor) ||
        !is_valid_floor(m->destination_floor) ||
        !is_valid_status(m->status)) {
        return 0;
    }

    /* Example invariant: "Between" cannot coincide with equal floors */
    if ((strcmp(m->status, "Between") == 0) &&
        (strcmp(m->current_floor, m->destination_floor) == 0)) {
        return 0;
    }

    return 1;
}

/* ===== Global state for cleanup ===== */
static int shm_fd = -1;
static car_shared_mem *g_mem = NULL;
static char shm_name[128];

/* Clean up (idempotent) */
static void cleanup_and_exit(int code) {
    if (g_mem != NULL) {
        /* Do NOT destroy process-shared mutex/cond here; other procs may be attached. */
        (void)munmap((void*)g_mem, sizeof *g_mem);
        g_mem = NULL;
    }
    if (shm_fd >= 0) { (void)close(shm_fd); shm_fd = -1; }
    _exit(code);
}

static void sigint_handler(int sig) {
    (void)sig;
    cleanup_and_exit(0);
}

/* ===== Main ===== */
int main(int argc, char **argv) {
    if (argc != 2) {
        wputs("Usage: safety {car_name}\n");
        return 1;
    }

    /* Build shm name: "/car" + argv[1] */
    shm_name[0] = '\0';
    if (snprintf(shm_name, sizeof shm_name, "/car%s", argv[1]) < 0) {
        wperr("safety: name build failed\n");
        return 1;
    }

    /* Attach to car shared memory */
    shm_fd = shm_open(shm_name, O_RDWR, 0600);
    if (shm_fd < 0) {
        wperr("safety: shm_open failed\n");
        return 1;
    }

    g_mem = (car_shared_mem *)mmap(NULL, sizeof *g_mem, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_mem == MAP_FAILED) {
        wperr("safety: mmap failed\n");
        return 1;
    }

    /* Be robust to SIGINT so we detach cleanly */
    (void)signal(SIGINT, sigint_handler);
    (void)signal(SIGTERM, sigint_handler);

    for (;;) { /* Continuous monitoring loop (justified for real-time systems) */
        int r = pthread_mutex_lock(&g_mem->mutex);
        if (r != 0) { wperr("safety: mutex_lock\n"); break; }

        /* Wait for any change signaled by the car/internal components */
        r = pthread_cond_wait(&g_mem->cond, &g_mem->mutex);
        if (r != 0) { wperr("safety: cond_wait\n"); (void)pthread_mutex_unlock(&g_mem->mutex); break; }

        /* --- Rule 1: obstruction while closing => Opening --- */
        if ((g_mem->door_obstruction == 1u) && (strcmp(g_mem->status, "Closing") == 0)) {
            set_status(g_mem, "Opening");
            (void)pthread_cond_broadcast(&g_mem->cond);
        }

        /* --- Rule 2: emergency stop / overload trigger emergency mode --- */
        if ((g_mem->emergency_stop == 1u) || (g_mem->overload == 1u)) {
            if (g_mem->emergency_mode == 0u) {
                g_mem->emergency_mode = 1u;
                (void)pthread_cond_broadcast(&g_mem->cond);
            }
        }

        /* --- Rule 3: basic data consistency --- */
        if (check_consistency(g_mem) == 0) {
            if (g_mem->emergency_mode == 0u) {
                g_mem->emergency_mode = 1u;
                (void)pthread_cond_broadcast(&g_mem->cond);
            }
        }

        /* --- Optional gentle fixup: if Between but at dest, close --- */
        if ((strcmp(g_mem->status, "Between") == 0) &&
            (strcmp(g_mem->current_floor, g_mem->destination_floor) == 0)) {
            set_status(g_mem, "Closed");
            (void)pthread_cond_broadcast(&g_mem->cond);
        }

        r = pthread_mutex_unlock(&g_mem->mutex);
        if (r != 0) { wperr("safety: mutex_unlock\n"); break; }
    }

    cleanup_and_exit(1);
    return 1; /* not reached */
}
