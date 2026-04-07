#ifndef CAR_SHARED_MEM_H
#define CAR_SHARED_MEM_H
#include <pthread.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    char current_floor[4];
    char destination_floor[4];
    char status[8]; 

    uint8_t open_button;
    uint8_t close_button;
    uint8_t door_obstruction;
    uint8_t overload;
    uint8_t emergency_stop;
    uint8_t individual_service_mode;
    uint8_t emergency_mode;
} car_shm_t;

#endif
