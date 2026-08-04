/* queue.c — dynamic array queue with shuffle/repeat. */
#include "queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void q_init(Queue *q, int shuffle, int repeat)
{
    memset(q, 0, sizeof(*q));
    q->cur = -1;
    q->shuffle = shuffle;
    q->repeat = repeat;
    pthread_mutex_init(&q->mutex, NULL);
}

void q_free(Queue *q)
{
    q_clear(q);
    free(q->items);
    q->items = NULL;
    q->cap = 0;
    pthread_mutex_destroy(&q->mutex);
}

void q_lock(Queue *q)   { pthread_mutex_lock(&q->mutex); }
void q_unlock(Queue *q) { pthread_mutex_unlock(&q->mutex); }

void q_clear(Queue *q)
{
    q->n = 0;
    q->cur = -1;
}

int q_find(Queue *q, const char *path)
{
    int i;
    for (i = 0; i < q->n; i++)
        if (!strcmp(q->items[i].path, path)) return i;
    return -1;
}

int q_add(Queue *q, const char *path, const char *name, long long size)
{
    QItem *it;
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 32;
        q->items = (QItem *)realloc(q->items, q->cap * sizeof(QItem));
    }
    it = &q->items[q->n];
    memset(it, 0, sizeof(*it));
    snprintf(it->path, sizeof it->path, "%s", path);
    snprintf(it->name, sizeof it->name, "%s", name);
    it->size = size;
    return q->n++;
}

void q_remove(Queue *q, int idx)
{
    if (idx < 0 || idx >= q->n) return;
    if (idx == q->cur) q->cur = -1;
    else if (idx < q->cur) q->cur--;
    memmove(&q->items[idx], &q->items[idx + 1], (q->n - idx - 1) * sizeof(QItem));
    q->n--;
}

int q_next(Queue *q)
{
    int i;
    if (q->n == 0) return -1;
    if (q->repeat == 2 && q->cur >= 0) return q->cur;      /* repeat one */
    if (q->shuffle && q->n > 1) {
        /* pick a random different index */
        int r = rand() % (q->n - 1);
        int target = q->cur >= 0 && r >= q->cur ? r + 1 : r;
        if (q->cur >= 0 && target == q->cur) target = (target + 1) % q->n;
        return target;
    }
    i = q->cur + 1;
    if (i >= q->n) {
        if (q->repeat == 1) i = 0;
        else return -1;
    }
    return i;
}

int q_prev(Queue *q)
{
    int i;
    if (q->n == 0) return -1;
    if (q->repeat == 2 && q->cur >= 0) return q->cur;
    if (q->shuffle && q->n > 1) {
        int r = rand() % (q->n - 1);
        int target = q->cur >= 0 && r >= q->cur ? r + 1 : r;
        if (q->cur >= 0 && target == q->cur) target = (target + 1) % q->n;
        return target;
    }
    i = q->cur - 1;
    if (i < 0) {
        if (q->repeat == 1) i = q->n - 1;
        else return -1;
    }
    return i;
}
