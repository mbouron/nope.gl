/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef JOB_QUEUE_H
#define JOB_QUEUE_H

#include <stdint.h>

#include "utils/pthread_compat.h"

struct ngli_fence {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int32_t signalled;
};

void ngli_fence_init(struct ngli_fence *fence);
void ngli_fence_destroy(struct ngli_fence *fence);
void ngli_fence_signal(struct ngli_fence *fence);
void ngli_fence_reset(struct ngli_fence *fence);
int ngli_fence_is_signalled(struct ngli_fence *fence);
void ngli_fence_wait(struct ngli_fence *fence);
int ngli_fence_wait_timeout(struct ngli_fence *fence, int64_t timeout);

typedef void (*ngli_job_func)(void *data, void *shared_data, uint32_t thread_index);

#define NGLI_QUEUE_MAX_THREAD 16

struct ngli_queue {
    char name[14];

    int stopped;
    pthread_mutex_t lock;
    pthread_cond_t has_queued_cond;
    pthread_cond_t has_space_cond;

    struct ngli_queue_job *jobs;
    uint32_t nb_jobs;
    uint32_t max_jobs;
    uint32_t write_idx;
    uint32_t read_idx;

    pthread_t threads[NGLI_QUEUE_MAX_THREAD];
    uint32_t nb_threads;

    void *shared_data;
};

int ngli_queue_init(struct ngli_queue *queue,const char *name, uint32_t max_jobs, uint32_t nb_threads, void *shared_data);
void ngli_queue_destroy(struct ngli_queue *queue);
void ngli_queue_add_job(struct ngli_queue *queue, void *data, struct ngli_fence *fence, ngli_job_func execute, ngli_job_func cleanup);
void ngli_queue_drop_job(struct ngli_queue *queue, struct ngli_fence *fence);
void ngli_queue_wait(struct ngli_queue *queue);

#endif
