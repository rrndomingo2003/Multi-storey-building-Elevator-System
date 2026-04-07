#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "car_shared_mem.h"  // must match testers: floors[4], status[8], flags as uint8_t

static car_shm_t *g = NULL;

/* ---- tiny helpers ---- */

static void update_flag(uint8_t *p, uint8_t v) {
    pthread_mutex_lock(&g->mutex);
    *p = v;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
}

/* Floors are 1..3 chars: e.g., "1","12","999","B1","B12","B999" */
static int copy_floor(char dst[4], const char *src) {
    size_t n = strlen(src);
    if (n == 0 || n > 3) return -1;
    /* precision ensures no truncation warning and always NUL */
    (void)snprintf(dst, 4, "%.*s", 3, src);
    return 0;
}

/* Checks if we are allowed to command a floor change (prints tester-expected messages).
   Returns 1 if allowed, 0 otherwise. */
static int is_floor_change_allowed(void) {
    int allowed = 0;

    pthread_mutex_lock(&g->mutex);

    if (g->individual_service_mode == 0u) {
        printf("Operation only allowed in service mode.\n");
    } else if (strcmp(g->status, "Between") == 0) {
        printf("Operation not allowed while elevator is moving.\n");
    } else if (strcmp(g->status, "Closed") != 0) {
        printf("Operation not allowed while doors are open.\n");
    } else {
        allowed = 1;
    }

    pthread_mutex_unlock(&g->mutex);
    return allowed;
}

/* direction: +1 for up, -1 for down */
static void handle_floor_change(int direction) {
    char next[8] = {0};

    pthread_mutex_lock(&g->mutex);

    /* Parse current into signed number: Bk => -k, n => +n */
    int cur;
    if (g->current_floor[0] == 'B') {
        cur = -(atoi(g->current_floor + 1));
    } else {
        cur = atoi(g->current_floor);
    }

    /* Compute next */
    if (direction > 0) { /* up */
        if (cur < 0) {
            /* going up in basements: B2->B1, B1->1 */
            int k = -cur;
            if (k <= 1) cur = 1; else cur = -(k - 1);
        } else {
            cur += 1;
        }
    } else {             /* down */
        if (cur <= 1) {
            /* 1 -> B1, already basement: Bk -> B(k+1) */
            if (cur == 1) cur = -1;
            else          cur -= 1; /* cur==0 shouldn't happen, but keep monotonicity */
        } else {
            cur -= 1;
        }
    }

    /* Cap to magnitude 1..999 to keep within 3 chars */
    if (cur >  999) cur =  999;
    if (cur < -999) cur = -999;

    /* Format next floor (no zero padding; testers show e.g., B32, B1) */
    if (cur < 0) {
        (void)snprintf(next, sizeof next, "B%d", -cur);
    } else {
        (void)snprintf(next, sizeof next, "%d", cur);
    }

    /* Write destination (bounded to 3 chars) */
    (void)copy_floor(g->destination_floor, next);

    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
}

/* ---- main ---- */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: {car name} {operation}\n");
        return EXIT_FAILURE;
    }

    /* Build shm name: "/car" + name */
    char shm_name[128];
    (void)snprintf(shm_name, sizeof shm_name, "/car%s", argv[1]);

    int fd = shm_open(shm_name, O_RDWR, 0660);
    if (fd < 0) {
        printf("Unable to access car %s.\n", argv[1]);
        return EXIT_FAILURE;
    }

    g = (car_shm_t*)mmap(NULL, sizeof *g, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    /* Decide operation */
    const char *op = argv[2];

    if (strcmp(op, "open") == 0) {
        update_flag(&g->open_button, 1u);
    } else if (strcmp(op, "close") == 0) {
        update_flag(&g->close_button, 1u);
    } else if (strcmp(op, "stop") == 0) {
        update_flag(&g->emergency_stop, 1u);
    } else if (strcmp(op, "service_on") == 0) {
        /* Clear emergency, enable individual service */
        pthread_mutex_lock(&g->mutex);
        g->emergency_mode = 0u;
        g->individual_service_mode = 1u;
        pthread_cond_broadcast(&g->cond);
        pthread_mutex_unlock(&g->mutex);
    } else if (strcmp(op, "service_off") == 0) {
        pthread_mutex_lock(&g->mutex);
        g->individual_service_mode = 0u;
        pthread_cond_broadcast(&g->cond);
        pthread_mutex_unlock(&g->mutex);
    } else if (strcmp(op, "up") == 0) {
        if (is_floor_change_allowed() == 1) {
            handle_floor_change(+1);
        }
    } else if (strcmp(op, "down") == 0) {
        if (is_floor_change_allowed() == 1) {
            handle_floor_change(-1);
        }
    } else {
        printf("Invalid operation.\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
