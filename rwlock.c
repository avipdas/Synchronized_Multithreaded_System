#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef enum {READERS, WRITERS, N_WAY} PRIORITY;

typedef struct rwlock rwlock_t;

struct rwlock {
    pthread_mutex_t mutex;
    pthread_cond_t  readers_cv;
    pthread_cond_t  writers_cv;

    int active_readers, waiting_readers, active_writers, waiting_writers;
    PRIORITY priority;
    int n, readers_since_writer;
}

rwlock_t* rwlock_new(PRIORITY P, int n) {
    rwlock_t *rw = malloc(sizeof(*rw));
    if (!rw) {
        return NULL;
    }

    pthread_mutex_init(&rw->mutex, NULL);
    pthread_cond_init(&rw->readers_cv, NULL);
    pthread_cond_init(&rw->writers_cv, NULL);

    rw->active_readers = rw->waiting_readers = 0;
    rw->active_writers = rw->waiting_writers = 0;
    rw->prio = p;
    rw->n    = (p == N_WAY) ? n : 0;
    rw->readers_since_writer = 0;

    return rw;
}