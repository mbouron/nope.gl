/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_SPSC_H
#define NAE_SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A lock-free queue of fixed-size elements, for one producer and one consumer thread */
struct spsc {
    _Atomic size_t head;  /* consumer */
    /* Separate the counters without requiring an over-aligned allocation. */
    unsigned char padding[64 - sizeof(_Atomic size_t)];
    _Atomic size_t tail;  /* producer */
    size_t mask;
    size_t element_size;
    unsigned char *data;
};

/* Room for at least `capacity` elements, rounded up to a power of two */
static inline int spsc_init(struct spsc *q, size_t capacity, size_t element_size)
{
    if (!capacity || !element_size)
        return -1;
    size_t size = 1;
    while (size < capacity) {
        if (size > SIZE_MAX / 2)
            return -1;
        size <<= 1;
    }
    if (size > SIZE_MAX / element_size)
        return -1;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    if (!atomic_is_lock_free(&q->head) || !atomic_is_lock_free(&q->tail))
        return -1;
    q->data = calloc(size, element_size);
    if (!q->data)
        return -1;
    q->mask = size - 1;
    q->element_size = element_size;
    return 0;
}

static inline void spsc_uninit(struct spsc *q)
{
    free(q->data);
    q->data = NULL;
}

static inline size_t spsc_capacity(const struct spsc *q)
{
    return q->mask + 1;
}

/* Producer: copies an element in, false if the queue is full */
static inline bool spsc_push(struct spsc *q, const void *element)
{
    const size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail - head > q->mask)
        return false;
    memcpy(q->data + (tail & q->mask) * q->element_size, element, q->element_size);
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

/* Consumer: copies the next element out, false if the queue is empty */
static inline bool spsc_pop(struct spsc *q, void *element)
{
    const size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    const size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail)
        return false;
    memcpy(element, q->data + (head & q->mask) * q->element_size, q->element_size);
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

#endif
