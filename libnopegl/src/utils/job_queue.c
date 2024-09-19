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

#include <string.h>
#include <time.h>

#include "nopegl/nopegl.h"

#include "job_queue.h"
#include "memory.h"
#include "thread.h"
#include "time.h"
#include "utils.h"

void ngli_fence_init(struct ngli_fence *fence)
{
    memset(fence, 0, sizeof(*fence));
    pthread_mutex_init(&fence->mutex, NULL);
    pthread_cond_init(&fence->cond, NULL);
    fence->signalled = 1;
}

void ngli_fence_destroy(struct ngli_fence *fence)
{

    pthread_mutex_lock(&fence->mutex);
    ngli_assert(fence->signalled);
    pthread_mutex_unlock(&fence->mutex);

    pthread_cond_destroy(&fence->cond);
    pthread_mutex_destroy(&fence->mutex);

    memset(fence, 0, sizeof(*fence));
}

void ngli_fence_reset(struct ngli_fence *fence)
{
    pthread_mutex_lock(&fence->mutex);
    ngli_assert(fence->signalled);
    fence->signalled = 0;
    pthread_mutex_unlock(&fence->mutex);
}

int ngli_fence_is_signalled(struct ngli_fence *fence)
{
    int signaled = 0;

    pthread_mutex_lock(&fence->mutex);
    signaled = fence->signalled;
    pthread_mutex_unlock(&fence->mutex);

    return signaled;
}

void ngli_fence_signal(struct ngli_fence *fence)
{
    pthread_mutex_lock(&fence->mutex);
    fence->signalled = 1;
    pthread_cond_broadcast(&fence->cond);
    pthread_mutex_unlock(&fence->mutex);
}

void ngli_fence_wait(struct ngli_fence *fence)
{
    pthread_mutex_lock(&fence->mutex);
    while (!fence->signalled)
        pthread_cond_wait(&fence->cond, &fence->mutex);
    pthread_mutex_unlock(&fence->mutex);
}

int ngli_fence_wait_timeout(struct ngli_fence *fence, int64_t timeout)
{
    if (ngli_fence_is_signalled(fence))
        return 1;

    if (timeout == INT64_MAX) {
        ngli_fence_wait(fence);
        return 1;
    }

    struct timespec ts;
#if defined(TARGET_ANDROID)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif

    ts.tv_sec += timeout / NGLI_NS_PER_SEC;
    ts.tv_nsec += timeout % NGLI_NS_PER_SEC;
    if (ts.tv_nsec >= NGLI_NS_PER_SEC) {
        ts.tv_sec++;
        ts.tv_nsec -= NGLI_NS_PER_SEC;
    }

    int signaled = 0;
    pthread_mutex_lock(&fence->mutex);
    while (!fence->signalled) {
        if (pthread_cond_timedwait(&fence->cond, &fence->mutex, &ts) != 0)
            break;
    }
    signaled = fence->signalled;
    pthread_mutex_unlock(&fence->mutex);

    return signaled;
}

struct ngli_barrier {
    uint32_t count;
    uint32_t waiters;
    uint64_t sequence;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

static void ngli_barrier_init(struct ngli_barrier *barrier, uint32_t count)
{
    barrier->count    = count;
    barrier->waiters  = 0;
    barrier->sequence = 0;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
}

static void ngli_barrier_destroy(struct ngli_barrier *barrier)
{
    ngli_assert(barrier->waiters == 0);
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
}

static int ngli_barrier_wait(struct ngli_barrier *barrier)
{
    int is_serial = 0;

    pthread_mutex_lock(&barrier->mutex);

    ngli_assert(barrier->waiters < barrier->count);
    barrier->waiters++;

    if (barrier->waiters < barrier->count) {
        uint64_t sequence = barrier->sequence;
        do {
            pthread_cond_wait(&barrier->cond, &barrier->mutex);
        } while (sequence == barrier->sequence);
    } else {
        barrier->waiters = 0;
        barrier->sequence++;
        pthread_cond_broadcast(&barrier->cond);
        is_serial = 1;
    }

    pthread_mutex_unlock(&barrier->mutex);

    return is_serial;
}

struct ngli_queue_job {
    void *data;
    void *shared_data;
    struct ngli_fence *fence;
    ngli_job_func execute;
    ngli_job_func cleanup;
};

struct thread_arg {
    struct ngli_queue *queue;
    uint32_t thread_index;
};

static void *queue_thread(void *arg)
{
    struct thread_arg *thread_arg = arg;

    struct ngli_queue *queue = thread_arg->queue;
    uint32_t thread_index = thread_arg->thread_index;

    ngli_freep(&arg);
    thread_arg = NULL;

    if (strlen(queue->name) > 0) {
        char name[16];
        snprintf(name, sizeof(name), "%s-%u", queue->name, thread_index);
        ngli_thread_set_name(name);
    }

    while (1) {
        pthread_mutex_lock(&queue->lock);
        ngli_assert(queue->nb_jobs <= queue->max_jobs);

        while (!queue->stopped && queue->nb_jobs == 0)
            pthread_cond_wait(&queue->has_queued_cond, &queue->lock);

        if (queue->stopped) {
            pthread_mutex_unlock(&queue->lock);
            break;
        }

        uint32_t read_idx = queue->read_idx;
        struct ngli_queue_job job = queue->jobs[read_idx];
        memset(&queue->jobs[read_idx], 0, sizeof(struct ngli_queue_job));
        queue->read_idx = (read_idx + 1) % queue->max_jobs;
        queue->nb_jobs--;
        pthread_cond_signal(&queue->has_space_cond);
        pthread_mutex_unlock(&queue->lock);

        if (job.data) {
            job.execute(job.data, job.shared_data, thread_index);
            if (job.fence)
                ngli_fence_signal(job.fence);
            if (job.cleanup)
                job.cleanup(job.data, job.shared_data, thread_index);
        }
    }

    pthread_mutex_lock(&queue->lock);
    if (queue->stopped) {
        for (uint32_t i = queue->read_idx; i != queue->write_idx; i = (i + 1) % queue->max_jobs) {
            if (queue->jobs[i].data) {
                if (queue->jobs[i].fence)
                    ngli_fence_signal(queue->jobs[i].fence);
                queue->jobs[i].data = NULL;
            }
        }
        queue->read_idx = queue->write_idx;
        queue->nb_jobs  = 0;
    }
    pthread_mutex_unlock(&queue->lock);

    return NULL;
}

static int ngli_queue_create_thread(struct ngli_queue *queue, uint32_t index)
{
    struct thread_arg *arg = ngli_calloc(1, sizeof(*arg));
    if (!arg)
        return NGL_ERROR_MEMORY;

    arg->queue        = queue;
    arg->thread_index = index;

    int ret = pthread_create(&queue->threads[index], NULL, queue_thread, arg);
    if (ret != 0) {
        ngli_freep(&arg);
        return NGL_ERROR_MEMORY;
    }

    return 0;
}

int ngli_queue_init(struct ngli_queue *queue, const char *name, uint32_t max_jobs, uint32_t nb_threads, void *shared_data)
{
    memset(queue, 0, sizeof(*queue));

    if (nb_threads < 1 || nb_threads > NGLI_QUEUE_MAX_THREAD)
        return NGL_ERROR_UNSUPPORTED;

    snprintf(queue->name, sizeof(queue->name), "%s", name);

    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->has_queued_cond, NULL);
    pthread_cond_init(&queue->has_space_cond, NULL);

    queue->jobs = ngli_calloc(max_jobs, sizeof(*queue->jobs));
    if (!queue->jobs)
        goto fail;
    queue->max_jobs = max_jobs;

    for (uint32_t i = 0; i < nb_threads; i++) {
        if (ngli_queue_create_thread(queue, i) < 0)
            goto fail;
        queue->nb_threads++;
    }

    queue->shared_data = shared_data;

    return 0;

fail:
    ngli_queue_destroy(queue);
    return NGL_ERROR_MEMORY;
}

void ngli_queue_destroy(struct ngli_queue *queue)
{
    if (!queue->jobs)
        return;

    pthread_mutex_lock(&queue->lock);
    queue->stopped = 1;
    pthread_cond_signal(&queue->has_queued_cond);
    pthread_mutex_unlock(&queue->lock);

    for (uint32_t i = 0; i < queue->nb_threads; i++) {
        pthread_join(queue->threads[i], NULL);
    }

    pthread_cond_destroy(&queue->has_space_cond);
    pthread_cond_destroy(&queue->has_queued_cond);
    pthread_mutex_destroy(&queue->lock);
    ngli_free(queue->jobs);
    memset(queue, 0, sizeof(*queue));
}

static void ngli_queue_add_job_unsafe(struct ngli_queue *queue, void *data, struct ngli_fence *fence, ngli_job_func execute, ngli_job_func cleanup)
{
    if (queue->stopped)
        return;

    if (fence)
        ngli_fence_reset(fence);

    ngli_assert(queue->nb_jobs <= queue->max_jobs);

    if (queue->nb_jobs >= queue->max_jobs) {
        while (queue->nb_jobs >= queue->max_jobs)
            pthread_cond_wait(&queue->has_space_cond, &queue->lock);
    }

    struct ngli_queue_job *queue_job = &queue->jobs[queue->write_idx];
    ngli_assert(queue_job->data == NULL);

    queue_job->data        = data;
    queue_job->shared_data = queue->shared_data;
    queue_job->fence       = fence;
    queue_job->execute     = execute;
    queue_job->cleanup     = cleanup;

    queue->write_idx = (queue->write_idx + 1) % queue->max_jobs;
    queue->nb_jobs++;
    pthread_cond_signal(&queue->has_queued_cond);
}

static void ngli_queue_add_job_safe(struct ngli_queue *queue, void *data, struct ngli_fence *fence, ngli_job_func execute, ngli_job_func cleanup)
{
    pthread_mutex_lock(&queue->lock);
    ngli_queue_add_job_unsafe(queue, data, fence, execute, cleanup);
    pthread_mutex_unlock(&queue->lock);
}

void ngli_queue_add_job(struct ngli_queue *queue, void *data, struct ngli_fence *fence, ngli_job_func execute, ngli_job_func cleanup)
{
    ngli_queue_add_job_safe(queue, data, fence, execute, cleanup);
}

void ngli_queue_drop_job(struct ngli_queue *queue, struct ngli_fence *fence)
{
    int removed = 0;

    if (!fence)
        return;

    if (ngli_fence_is_signalled(fence))
        return;

    pthread_mutex_lock(&queue->lock);
    for (uint32_t i = queue->read_idx; i != queue->write_idx; i = (i + 1) % queue->max_jobs) {
        struct ngli_queue_job *job = &queue->jobs[i];
        if (job->fence == fence) {
            if (job->cleanup)
                job->cleanup(job->data, queue->shared_data, UINT32_MAX);

            memset(&queue->jobs[i], 0, sizeof(queue->jobs[i]));
            removed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&queue->lock);

    if (removed)
        ngli_fence_signal(fence);
    else
        ngli_fence_wait(fence);
}

static void wait_barrier(void *data, void *shared_data, uint32_t thread_index)
{
    struct ngli_barrier *barrier = data;

    if (ngli_barrier_wait(barrier))
        ngli_barrier_destroy(barrier);
}

void ngli_queue_wait(struct ngli_queue *queue)
{
    pthread_mutex_lock(&queue->lock);

    struct ngli_barrier barrier;
    ngli_barrier_init(&barrier, queue->nb_threads);

    struct ngli_fence fences[NGLI_QUEUE_MAX_THREAD] = {0};
    for (uint32_t i = 0; i < queue->nb_threads; ++i) {
        struct ngli_fence *fence = &fences[i];
        ngli_fence_init(fence);
        ngli_queue_add_job_unsafe(queue, &barrier, fence, wait_barrier, NULL);
    }

    pthread_mutex_unlock(&queue->lock);

    for (uint32_t i = 0; i < queue->nb_threads; i++) {
        ngli_fence_wait(&fences[i]);
        ngli_fence_destroy(&fences[i]);
    }
}
