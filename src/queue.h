/* queue.h — the play queue. */
#ifndef MIKA_QUEUE_H
#define MIKA_QUEUE_H

#include <pthread.h>

#define Q_PATH_MAX 2048
#define Q_NAME_MAX 512

typedef struct {
    char title[512];
    char artist[512];
    char album[512];
    int  duration_ms;
    int  track;      /* track number, 0 if unknown */
    int  disc;       /* disc number, 0 if unknown */
    int  have_meta;
} Meta;

typedef struct {
    char path[Q_PATH_MAX];
    char name[Q_NAME_MAX];
    long long size;
    Meta meta;
    int scrobbled;   /* scrobble already submitted for this listen */
    int np_sent;     /* now-playing already sent */
} QItem;

typedef struct {
    QItem *items;
    int n, cap;
    int cur;         /* index of current track, -1 = none */
    int shuffle;     /* 0/1 */
    int repeat;      /* 0 off, 1 all, 2 one */
    pthread_mutex_t mutex;
} Queue;

void q_init(Queue *q, int shuffle, int repeat);
void q_free(Queue *q);
void q_lock(Queue *q);
void q_unlock(Queue *q);
void q_clear(Queue *q);
int  q_find(Queue *q, const char *path);                       /* index or -1 */
int  q_add(Queue *q, const char *path, const char *name, long long size); /* returns index */
void q_remove(Queue *q, int idx);
/* index to play after current (respects shuffle/repeat); -1 = stop.
 * does NOT move q->cur. */
int  q_next(Queue *q);
/* "previous": random when shuffling, else cur-1 (wraps if repeat all) */
int  q_prev(Queue *q);

#endif
