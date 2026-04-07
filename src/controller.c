#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"   // send_msg / recv_msg (16-bit framing)
#include "floor.h"      // parse_floor / floor_in_range

// ---------------- Floor helpers (ordering + direction) ----------------

static bool floor_to_int(const char *s, int *out) {
    bool is_b = false; int n = 0;
    if (!parse_floor(s, &is_b, &n)) return false;
    *out = is_b ? -n : n;           // B3 < B2 < B1 < 1 < 2 < ...
    return true;
}

static char get_direction(const char *a, const char *b) {
    int ia, ib;
    if (!floor_to_int(a, &ia) || !floor_to_int(b, &ib)) return '?';
    if (ia == ib) return 'E';       // equal
    return (ib > ia) ? 'U' : 'D';
}

// ---------------- Data structures ----------------

typedef struct {
    int   fd;                       // socket to the car
    char  name[64];
    char  lowest[8], highest[8];

    // live status from STATUS messages
    char  current[8];
    char  dest[8];
    char  door[16];                 // "Opening|Open|Closing|Closed|Between"
} car_t;

typedef struct stop_node {
    int   car_fd;                   // assigned car
    char  floor[8];
    struct stop_node *next;
} stop_node;

// Global lists + locks
static car_t    *cars = NULL;       // simple dynamic array
static size_t    cars_sz = 0, cars_cap = 0;
static pthread_mutex_t cars_mx = PTHREAD_MUTEX_INITIALIZER;

static stop_node *stops_head = NULL;
static pthread_mutex_t stops_mx = PTHREAD_MUTEX_INITIALIZER;

// ---------------- Car registry helpers ----------------

static car_t *find_car_by_fd_nolock(int fd) {
    for (size_t i = 0; i < cars_sz; ++i)
        if (cars[i].fd == fd) return &cars[i];
    return NULL;
}


static void remove_car_by_fd(int fd) {
    pthread_mutex_lock(&cars_mx);
    for (size_t i = 0; i < cars_sz; ++i) {
        if (cars[i].fd == fd) {
            cars[i] = cars[cars_sz - 1];
            cars_sz--;
            break;
        }
    }
    pthread_mutex_unlock(&cars_mx);
}

static void add_car(car_t *in) {
    pthread_mutex_lock(&cars_mx);
    if (cars_sz == cars_cap) {
        size_t nc = cars_cap ? cars_cap * 2 : 8;
        car_t *np = realloc(cars, nc * sizeof *np);
        if (!np) { pthread_mutex_unlock(&cars_mx); perror("realloc cars"); exit(1); }
        cars = np; cars_cap = nc;
    }
    cars[cars_sz++] = *in;
    pthread_mutex_unlock(&cars_mx);
}

// Range check: does car support both floors?
static bool car_covers(const car_t *c, const char *src, const char *dst) {
    int il, ih, is, id;
    if (!floor_to_int(c->lowest, &il))  return false;
    if (!floor_to_int(c->highest, &ih)) return false;
    if (!floor_to_int(src, &is))        return false;
    if (!floor_to_int(dst, &id))        return false;
    if (ih < il) { int t = ih; ih = il; il = t; } // safety
    return (is >= il && is <= ih && id >= il && id <= ih);
}

// First-fit chooser
static car_t *choose_car(const char *src, const char *dst) {
    pthread_mutex_lock(&cars_mx);
    car_t *best = NULL;
    for (size_t i = 0; i < cars_sz; ++i) {
        if (car_covers(&cars[i], src, dst)) { best = &cars[i]; break; }
    }
    pthread_mutex_unlock(&cars_mx);
    return best;
}

// ---------------- Stop queue helpers ----------------

static void enqueue_stop(int car_fd, const char *floor) {
    stop_node *n = malloc(sizeof *n);
    if (!n) return;
    n->car_fd = car_fd;
    snprintf(n->floor, sizeof n->floor, "%s", floor);
    n->next = NULL;

    pthread_mutex_lock(&stops_mx);
    if (!stops_head) {
        stops_head = n;
    } else {
        stop_node *p = stops_head;
        while (p->next) p = p->next;
        p->next = n;
    }
    pthread_mutex_unlock(&stops_mx);
}

static bool dequeue_first_for_car(int car_fd, char out[8]) {
    pthread_mutex_lock(&stops_mx);
    stop_node *p = stops_head, *prev = NULL;
    while (p) {
        if (p->car_fd == car_fd) {
            if (prev) prev->next = p->next; else stops_head = p->next;
            snprintf(out, 8, "%s", p->floor);
            free(p);
            pthread_mutex_unlock(&stops_mx);
            return true;
        }
        prev = p; p = p->next;
    }
    pthread_mutex_unlock(&stops_mx);
    return false;
}

// ---------------- Networking helpers ----------------

static int listen_v4_any(uint16_t port) {
    int lf = socket(AF_INET, SOCK_STREAM, 0);
    if (lf < 0) { perror("socket"); exit(1); }
    int yes = 1; setsockopt(lf, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(lf, (struct sockaddr*)&sa, sizeof sa) < 0) { perror("bind"); exit(1); }
    if (listen(lf, 64) < 0) { perror("listen"); exit(1); }
    return lf;
}

// ---------------- Car thread ----------------

static void *car_thread(void *arg) {
    int cfd = *(int*)arg; free(arg);
    // Keep a local pointer (valid while car remains in array)
    car_t *car = NULL;

    for (;;) {
        char *line = NULL;
        if (recv_msg(cfd, &line) < 0) break;

        if (strncmp(line, "STATUS ", 7) == 0) {
            // STATUS <door> <current> <dest>
            char door[16], cur[8], dst[8];
            if (sscanf(line, "STATUS %15s %7s %7s", door, cur, dst) == 3) {
                pthread_mutex_lock(&cars_mx);
                car = find_car_by_fd_nolock(cfd);
                if (car) {
                    snprintf(car->door,    sizeof car->door,    "%s", door);
                    snprintf(car->current, sizeof car->current, "%s", cur);
                    snprintf(car->dest,    sizeof car->dest,    "%s", dst);
                }
                pthread_mutex_unlock(&cars_mx);

                // If car is idle/opening/at-dest, dispatch next queued stop (if any)
                if (car && (strcmp(car->door, "Closed") == 0 ||
                            strcmp(car->door, "Opening") == 0 ||
                            strcmp(car->current, car->dest) == 0)) {
                    char next[8];
                    if (dequeue_first_for_car(cfd, next)) {
                        char msg[32];
                        snprintf(msg, sizeof msg, "FLOOR %s", next);
                        (void)send_msg(cfd, msg);
                    }
                }
            }
        } else if (strncmp(line, "EMERGENCY", 9) == 0 ||
                   strncmp(line, "INDIVIDUAL", 10) == 0) {
            // For now: stop dispatching; could clear queue etc.
        } else {
            // Unknown line; ignore
        }

        free(line);
    }

    shutdown(cfd, SHUT_RDWR);
    close(cfd);
    remove_car_by_fd(cfd);
    return NULL;
}

// ---------------- Main accept loop ----------------

int main(void) {
    int lf = listen_v4_any(3000);

    for (;;) {
        int cfd = accept(lf, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char *line = NULL;
        if (recv_msg(cfd, &line) < 0) { close(cfd); continue; }

        if (strncmp(line, "CAR ", 4) == 0) {
            // Register car
            car_t c = {0};
            c.fd = cfd;
            if (sscanf(line, "CAR %63s %7s %7s", c.name, c.lowest, c.highest) != 3) {
                free(line); close(cfd); continue;
            }
            // Default live fields
            snprintf(c.current, sizeof c.current, "%s", c.lowest);
            snprintf(c.dest,    sizeof c.dest,    "%s", c.lowest);
            snprintf(c.door,    sizeof c.door,    "%s", "Closed");

            add_car(&c);

            // spin a thread to handle STATUS + dispatching
            int *pf = malloc(sizeof *pf);
            if (!pf) { free(line); close(cfd); continue; }
            *pf = cfd;
            pthread_t th;
            if (pthread_create(&th, NULL, car_thread, pf) != 0) {
                free(pf); free(line); close(cfd); continue;
            }
            pthread_detach(th);
        }
        else if (strncmp(line, "CALL ", 5) == 0) {
            // CALL <src> <dst>
            char src[8], dst[8];
            if (sscanf(line, "CALL %7s %7s", src, dst) == 2) {
                car_t *chosen = choose_car(src, dst);
                if (!chosen) {
                    (void)send_msg(cfd, "UNAVAILABLE");
                } else {
                    // queue source then destination for that car
                    char dir = get_direction(src, dst);
                    (void)dir; // reserved for smarter ordering later
                    enqueue_stop(chosen->fd, src);
                    enqueue_stop(chosen->fd, dst);

                    char resp[96];
                    snprintf(resp, sizeof resp, "CAR %s", chosen->name);
                    (void)send_msg(cfd, resp);
                }
            }
        }

        free(line);
        // The client (call) can close; we keep car sockets open via their threads
        // For a CALL client, it's usually one request then close:
        shutdown(cfd, SHUT_RDWR);
        close(cfd);
    }

    close(lf);
    return 0;
}
