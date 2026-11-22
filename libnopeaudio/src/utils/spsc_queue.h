/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef NAU_SPSC_QUEUE_H
#define NAU_SPSC_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libnopeutil/utils.h"

/*
 * Lock-free queue for one producer thread and one consumer thread.
 *
 * The elements live in a ring whose capacity is a power of two. The producer
 * writes at the tail, the consumer reads at the head. Each index is written
 * by one thread. The unsigned counters wrap modulo SIZE_MAX + 1; tail - head
 * gives the queued count, including across that wrap. Masking an index selects
 * its slot in the ring.
 *
 *                 head (consumer)           tail (producer)
 *                   v                        v
 *    +----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+
 *    |    |    |    | ## | ## | ## | ## | ## |    |    |    |    |    |    |    |    |
 *    +----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+
 *                   '------- queued ---------'
 *
 * To push an element, the producer loads tail (relaxed) and head (acquire), then
 * checks tail - head < capacity to ensure the tail slot is free. Only the
 * producer updates tail, so its load can be relaxed and no other thread can
 * claim the slot before it writes the element. The consumer advances head
 * (release) only after copying an element; the producer's acquire ensures that
 * this copy is complete before it overwrites a reused slot.
 *
 * After writing the element, the producer advances tail (release). The consumer
 * loads head (relaxed) and tail (acquire), then checks head != tail before
 * reading, ensuring it sees the completed write. Only the consumer updates
 * head, so its load can also be relaxed.
 *
 * Usage:
 *
 *    NAU_SPSC_QUEUE(struct block) queue = {0};
 *    nau_spsc_queue_init(&queue, 32);
 *    nau_spsc_queue_push(&queue, block);
 *    nau_spsc_queue_pop(&queue, &block);
 *    nau_spsc_queue_uninit(&queue);
 */
struct spsc_queue {
    _Atomic size_t head;
    /* Separate the counters to reduce false sharing on CPUs with 64-byte cache lines. */
    uint8_t padding[64 - sizeof(_Atomic size_t)];
    _Atomic size_t tail;
    size_t mask;
};

#define NAU_SPSC_QUEUE_FIELDS_(T) \
    struct spsc_queue state;      \
    T *data

#define NAU_SPSC_QUEUE(T) struct { NAU_SPSC_QUEUE_FIELDS_(T); }
#define NAU_DECLARE_SPSC_QUEUE_WITH_NAME(name, T) struct name { NAU_SPSC_QUEUE_FIELDS_(T); }

typedef void *npriv_may_alias nau_spsc_queue_data_alias;

int nau_spsc_queue_init_(struct spsc_queue *q, nau_spsc_queue_data_alias *data,
                         size_t capacity, size_t element_size, size_t alignment);
void nau_spsc_queue_uninit_(struct spsc_queue *q, nau_spsc_queue_data_alias *data, size_t alignment);

#define nau_spsc_queue_init(q, capacity)                                         \
    __extension__ ({                                                             \
        __typeof__(q) nau_sq_init_q_ = (q);                                      \
        nau_spsc_queue_init_(&nau_sq_init_q_->state,                             \
                             (nau_spsc_queue_data_alias *)&nau_sq_init_q_->data, \
                             (capacity), sizeof(*nau_sq_init_q_->data),          \
                             _Alignof(__typeof__(*nau_sq_init_q_->data)));       \
    })

#define nau_spsc_queue_uninit(q) do {                                           \
    __typeof__(q) nau_sq_uninit_q_ = (q);                                       \
    nau_spsc_queue_uninit_(&nau_sq_uninit_q_->state,                            \
                          (nau_spsc_queue_data_alias *)&nau_sq_uninit_q_->data, \
                          _Alignof(__typeof__(*nau_sq_uninit_q_->data)));       \
} while (0)

#define nau_spsc_queue_capacity(q)                                         \
    __extension__ ({                                                       \
        __typeof__(q) nau_sq_capacity_q_ = (q);                            \
        nau_sq_capacity_q_->data ? nau_sq_capacity_q_->state.mask + 1 : 0; \
    })

static inline bool nau_spsc_queue_push_index_(const struct spsc_queue *q, size_t *tail)
{
    *tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    return *tail - head <= q->mask;
}

static inline bool nau_spsc_queue_pop_index_(const struct spsc_queue *q, size_t *head)
{
    *head = atomic_load_explicit(&q->head, memory_order_relaxed);
    const size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return *head != tail;
}

#define nau_spsc_queue_push(q, ...)                                                                        \
    __extension__ ({                                                                                       \
        __typeof__(q) nau_sq_push_q_ = (q);                                                                \
        struct spsc_queue *nau_sq_push_state_ = &nau_sq_push_q_->state;                                    \
        size_t nau_sq_push_tail_;                                                                          \
        const bool nau_sq_push_success_ = nau_sq_push_q_->data &&                                          \
            nau_spsc_queue_push_index_(nau_sq_push_state_, &nau_sq_push_tail_);                            \
        if (nau_sq_push_success_) {                                                                        \
            nau_sq_push_q_->data[nau_sq_push_tail_ & nau_sq_push_state_->mask] = (__VA_ARGS__);            \
            atomic_store_explicit(&nau_sq_push_state_->tail, nau_sq_push_tail_ + 1, memory_order_release); \
        }                                                                                                  \
        nau_sq_push_success_;                                                                              \
    })

#define nau_spsc_queue_pop(q, element)                                                                   \
    __extension__ ({                                                                                     \
        __typeof__(q) nau_sq_pop_q_ = (q);                                                               \
        struct spsc_queue *nau_sq_pop_state_ = &nau_sq_pop_q_->state;                                    \
        size_t nau_sq_pop_head_;                                                                         \
        const bool nau_sq_pop_success_ = nau_sq_pop_q_->data &&                                          \
            nau_spsc_queue_pop_index_(nau_sq_pop_state_, &nau_sq_pop_head_);                             \
        if (nau_sq_pop_success_) {                                                                       \
            __typeof__(nau_sq_pop_q_->data) nau_sq_pop_element_ = (element);                             \
            *nau_sq_pop_element_ = nau_sq_pop_q_->data[nau_sq_pop_head_ & nau_sq_pop_state_->mask];      \
            atomic_store_explicit(&nau_sq_pop_state_->head, nau_sq_pop_head_ + 1, memory_order_release); \
        }                                                                                                \
        nau_sq_pop_success_;                                                                             \
    })

#endif
