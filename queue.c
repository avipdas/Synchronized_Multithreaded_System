#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct queue queue_t;     

struct queue {
    void** data;
    int    capacity, head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
};

// Create q that can hold at most up to size elements 
queue_t* queue_new(int size) {
    if (size < 0) return NULL;
    
    queue_t* q = malloc(sizeof(*q));
    if (!q)  return NULL;

    q->data = malloc(size * sizeof(void*));
    if (!q->data){
        free(q);
        return NULL;
    }

    q->capacity = size;
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&(q->mutex), NULL);
    pthread_cond_init(&(q->not_empty), NULL);
    pthread_cond_init(&(q->not_full), NULL);

    return q;
}

void queue_delete(queue_t** qp) {
    if (!qp || !*qp) return;

    queue_t *q = *qp;

    pthread_mutex_destroy(&(q->mutex));
    pthread_cond_destroy(&(q->not_empty));
    pthread_cond_destroy(&(q->not_full));

    free(q->data);
    free(q);

    *qp = NULL;
}

bool queue_push(queue_t *q, void *elem) {
    if (!q) return false;

    pthread_mutex_lock(&q->mutex);
    while (q->count == q->capacity) {
        pthread_cond_wait(&(q->not_full), &q->mutex);
    }

    q->data[q->tail] = elem;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&(q->not_empty));
    pthread_mutex_unlock(&(q->mutex));

    return true;
}

bool queue_pop(queue_t *q, void** elem){
    if(!q || !elem) return false;

    pthread_mutex_lock(&(q->mutex));
    while (q->count == 0) {
        pthread_cond_wait(&(q->not_empty), &(q->mutex));
    }

    *elem = q->data[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&(q->not_full));
    pthread_mutex_unlock(&(q->mutex));
    return true;
}

