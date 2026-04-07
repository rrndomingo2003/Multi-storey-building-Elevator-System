#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

#include "car_shared_mem.h"   // MUST match testers: status is char[8], floors char[4]

// ---- Optional networking (leave disabled until controller is ready) ----
// #define ENABLE_CAR_NET 1
#ifdef ENABLE_CAR_NET
  #include "protocol.h"       // send_msg/recv_msg (16-bit framing)
  int tcp_connect_ipv4(const char *ip, unsigned short port); // from common/net.c
#endif

// ====== Local helpers / globals ======
static int shm_fd = -1;
static car_shm_t *shm = NULL;           // mapped shared memory
static char shm_name[128] = {0};        // e.g., "/carAlpha"
static int running = 1;                  // main loop flag
static int delay_ms = 1000;              // inter-state delay in ms

// Non-shared metadata (local only)
static char car_name_local[64]  = {0};
static char lowest_local[8]     = {0};
static char highest_local[8]    = {0};

// Threads
static pthread_t th_buttons, th_doors, th_motion;
#ifdef ENABLE_CAR_NET
static pthread_t th_net;
#endif

// ---- Small helpers ----
static inline void broadcast(void) { (void)pthread_cond_broadcast(&shm->cond); }

static void set_status(const char *s) {
    // status buffer is 8, longest token is "Between" (7) + NUL
    (void)snprintf(shm->status, sizeof shm->status, "%s", s);
}

// Copy a floor string with strict max length 3 (plus NUL). Returns 0 on OK, -1 on invalid length.
static int copy_floor(char *dst, size_t dstsz, const char *src) {
    size_t n = strlen(src);
    if (dstsz < 2 || n == 0 || n > 3) return -1; // enforce 1..3 chars
    // bound copy: precision ensures no truncation warning, always NUL-terminated
    (void)snprintf(dst, dstsz, "%.*s", (int)(dstsz - 1), src);
    return 0;
}

// Convert milliseconds to absolute timespec and sleep via cond timedwait (while holding shm->mutex)
static void sleep_ms_cond(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    (void)pthread_cond_timedwait(&shm->cond, &shm->mutex, &ts); // ETIMEDOUT is expected
}

static void cleanup_and_exit(int code) {
    running = 0;
    if (shm) { (void)munmap(shm, sizeof *shm); shm = NULL; }
    if (shm_fd >= 0) { (void)close(shm_fd); shm_fd = -1; }
    if (shm_name[0]) { (void)shm_unlink(shm_name); }
    _exit(code);
}

static void sigint_handler(int signum) {
    (void)signum;
    cleanup_and_exit(0);
}

// ====== Threads ======

// Reacts to open/close buttons & individual service mode
static void* button_thread(void *arg) {
    (void)arg;
    while (running) {
        (void)pthread_mutex_lock(&shm->mutex);
        (void)pthread_cond_wait(&shm->cond, &shm->mutex);

        // Open button
        if (shm->open_button == 1u) {
            shm->open_button = 0u;
            if (shm->individual_service_mode == 1u) {
                set_status("Open");
            } else {
                if (!strcmp(shm->status,"Closed") || !strcmp(shm->status,"Closing")) {
                    set_status("Opening");
                }
            }
            broadcast();
        }

        // Close button
        if (shm->close_button == 1u) {
            shm->close_button = 0u;
            if (shm->individual_service_mode == 1u) {
                set_status("Closed");
            } else {
                if (!strcmp(shm->status,"Open")) {
                    set_status("Closing");
                }
            }
            broadcast();
        }

        (void)pthread_mutex_unlock(&shm->mutex);
    }
    return NULL;
}

// Door state machine: Opening -> Open -> Closing -> Closed, respecting obstruction
static void* door_thread(void *arg) {
    (void)arg;
    while (running) {
        (void)pthread_mutex_lock(&shm->mutex);
        (void)pthread_cond_wait(&shm->cond, &shm->mutex);

        if (shm->individual_service_mode == 0u) {
            // Obstruction during Closing => Opening
            if ((shm->door_obstruction == 1u) && !strcmp(shm->status,"Closing")) {
                set_status("Opening");
                broadcast();
            }

            if (!strcmp(shm->status,"Opening")) {
                sleep_ms_cond(delay_ms);
                set_status("Open");
                broadcast();
            }
            if (!strcmp(shm->status,"Open")) {
                sleep_ms_cond(delay_ms);
                set_status("Closing");
                broadcast();
            }
            if (!strcmp(shm->status,"Closing")) {
                sleep_ms_cond(delay_ms);
                if (shm->door_obstruction == 0u) {
                    set_status("Closed");
                    broadcast();
                }
            }
        }

        (void)pthread_mutex_unlock(&shm->mutex);
    }
    return NULL;
}

// If a destination exists and doors are closed, move: Between -> (wait) -> Closed at dest
static void* motion_thread(void *arg) {
    (void)arg;
    while (running) {
        (void)pthread_mutex_lock(&shm->mutex);
        (void)pthread_cond_wait(&shm->cond, &shm->mutex);

        if ((shm->individual_service_mode == 0u) &&
            !strcmp(shm->status,"Closed") &&
            strcmp(shm->current_floor, shm->destination_floor) != 0)
        {
            set_status("Between");
            broadcast();

            sleep_ms_cond(delay_ms);

            // arrive at destination (bounded copy)
            (void)copy_floor(shm->current_floor, sizeof shm->current_floor, shm->destination_floor);
            set_status("Closed");
            broadcast();
        }

        (void)pthread_mutex_unlock(&shm->mutex);
    }
    return NULL;
}

#ifdef ENABLE_CAR_NET
// Minimal networking: announce and accept FLOOR commands
static void* net_thread(void *arg) {
    (void)arg;
    int fd = tcp_connect_ipv4("127.0.0.1", 3000);
    if (fd < 0) return NULL;

    // Identify car: "CAR <name> <lowest> <highest>"
    char msg[128];
    (void)snprintf(msg, sizeof msg, "CAR %s %s %s", car_name_local, lowest_local, highest_local);
    if (send_msg(fd, msg) < 0) { (void)close(fd); return NULL; }

    // Send initial STATUS
    (void)pthread_mutex_lock(&shm->mutex);
    (void)snprintf(msg, sizeof msg, "STATUS %s %s %s", shm->status, shm->current_floor, shm->destination_floor);
    (void)pthread_mutex_unlock(&shm->mutex);
    (void)send_msg(fd, msg);

    for (;;) {
        char *line = NULL;
        if (recv_msg(fd, &line) < 0) break;
        if (strncmp(line, "FLOOR ", 6) == 0) {
            (void)pthread_mutex_lock(&shm->mutex);
            // Replace destination only if not Between (simple policy)
            if (strcmp(shm->status, "Between") != 0) {
                (void)copy_floor(shm->destination_floor, sizeof shm->destination_floor, line + 6);
                broadcast();
            }
            (void)pthread_mutex_unlock(&shm->mutex);
        }
        free(line);
    }
    (void)close(fd);
    return NULL;
}
#endif

// ====== main ======
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {name} {lowest} {highest} {delay_ms}\n", argv[0]);
        return 1;
    }

    // Ignore SIGPIPE so failed socket
    (void)signal(SIGPIPE, SIG_IGN);
    (void)signal(SIGINT,  sigint_handler);
    (void)signal(SIGTERM, sigint_handler);

    // Local metadata (not in shared block)
    (void)snprintf(car_name_local, sizeof car_name_local, "%s", argv[1]);
    (void)snprintf(lowest_local,   sizeof lowest_local,   "%s", argv[2]);
    (void)snprintf(highest_local,  sizeof highest_local,  "%s", argv[3]);

    delay_ms = atoi(argv[4]);
    if (delay_ms <= 0) delay_ms = 1000;

    // Validate lowest/highest are <= 3 chars to prevent bogus inputs
    if (strlen(lowest_local) == 0 || strlen(lowest_local) > 3 ||
        strlen(highest_local) == 0 || strlen(highest_local) > 3) {
        fprintf(stderr, "car: floor args must be 1..3 chars (e.g., B2, 10)\n");
        return 1;
    }

    // Build shm name: "/car<name>"
    if (snprintf(shm_name, sizeof shm_name, "/car%s", car_name_local) < 0) {
        perror("car: shm name");
        return 1;
    }

    // Fresh shared memory
    (void)shm_unlink(shm_name);
    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof *shm) < 0) { perror("ftruncate"); cleanup_and_exit(1); }

    shm = (car_shm_t*)mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); cleanup_and_exit(1); }

    // Init process-shared mutex/cond
    pthread_mutexattr_t ma; (void)pthread_mutexattr_init(&ma);
    (void)pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    (void)pthread_mutex_init(&shm->mutex, &ma);
    (void)pthread_mutexattr_destroy(&ma);

    pthread_condattr_t ca; (void)pthread_condattr_init(&ca);
    (void)pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    (void)pthread_cond_init(&shm->cond, &ca);
    (void)pthread_condattr_destroy(&ca);

    // Init remaining fields under lock
    (void)pthread_mutex_lock(&shm->mutex);

    if (copy_floor(shm->current_floor, sizeof shm->current_floor, lowest_local) != 0 ||
        copy_floor(shm->destination_floor, sizeof shm->destination_floor, lowest_local) != 0) {
        (void)pthread_mutex_unlock(&shm->mutex);
        fprintf(stderr, "car: invalid floor format for lowest\n");
        cleanup_and_exit(1);
    }

    set_status("Closed");

    shm->open_button             = 0u;
    shm->close_button            = 0u;
    shm->door_obstruction        = 0u;
    shm->overload                = 0u;
    shm->emergency_stop          = 0u;
    shm->individual_service_mode = 0u;
    shm->emergency_mode          = 0u;

    broadcast();
    (void)pthread_mutex_unlock(&shm->mutex);

    // Spawn threads
    if (pthread_create(&th_buttons, NULL, button_thread, NULL) != 0) { perror("pthread_create buttons"); cleanup_and_exit(1); }
    if (pthread_create(&th_doors,   NULL, door_thread,   NULL) != 0) { perror("pthread_create doors");   cleanup_and_exit(1); }
    if (pthread_create(&th_motion,  NULL, motion_thread, NULL) != 0) { perror("pthread_create motion");  cleanup_and_exit(1); }

#ifdef ENABLE_CAR_NET
    if (pthread_create(&th_net,     NULL, net_thread,    NULL) != 0) { perror("pthread_create net"); }
#endif

    // Join core threads
    (void)pthread_join(th_buttons, NULL);
    (void)pthread_join(th_doors,   NULL);
    (void)pthread_join(th_motion,  NULL);
#ifdef ENABLE_CAR_NET
    (void)pthread_join(th_net, NULL);
#endif

    cleanup_and_exit(0);
    return 0;
}
