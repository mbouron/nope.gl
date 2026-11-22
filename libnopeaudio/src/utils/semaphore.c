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

#define _GNU_SOURCE  /* sem_timedwait() and clock_gettime() with glibc */

#if defined(_WIN32)
#include <limits.h>
#elif !defined(__APPLE__)
#include <errno.h>
#include <time.h>
#endif

#include "libnopeutil/time.h"
#include "nopeaudio/nopeaudio.h"
#include "semaphore.h"

#if defined(_WIN32)

int nau_sem_init(nau_sem *sem)
{
    *sem = CreateSemaphoreA(NULL, 0, LONG_MAX, NULL);
    return *sem ? NAU_OK : NAU_ERROR_MEMORY;
}

void nau_sem_destroy(nau_sem *sem)
{
    CloseHandle(*sem);
}

void nau_sem_post(nau_sem *sem)
{
    ReleaseSemaphore(*sem, 1, NULL);
}

void nau_sem_wait(nau_sem *sem)
{
    WaitForSingleObject(*sem, INFINITE);
}

bool nau_sem_timedwait(nau_sem *sem, int64_t timeout)
{
    /* In milliseconds, rounded up; INFINITE is the largest value */
    const int64_t ms = (timeout + 999999) / 1000000;
    return WaitForSingleObject(*sem, ms < INFINITE ? (DWORD)ms : INFINITE - 1) == WAIT_OBJECT_0;
}

#elif defined(__APPLE__)

int nau_sem_init(nau_sem *sem)
{
    *sem = dispatch_semaphore_create(0);
    return *sem ? NAU_OK : NAU_ERROR_MEMORY;
}

void nau_sem_destroy(nau_sem *sem)
{
    dispatch_release(*sem);
}

void nau_sem_post(nau_sem *sem)
{
    dispatch_semaphore_signal(*sem);
}

void nau_sem_wait(nau_sem *sem)
{
    dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER);
}

bool nau_sem_timedwait(nau_sem *sem, int64_t timeout)
{
    return dispatch_semaphore_wait(*sem, dispatch_time(DISPATCH_TIME_NOW, timeout)) == 0;
}

#else

int nau_sem_init(nau_sem *sem)
{
    int ret = sem_init(sem, 0, 0);
    return ret < 0 ? NAU_ERROR_MEMORY : NAU_OK;
}

void nau_sem_destroy(nau_sem *sem)
{
    sem_destroy(sem);
}

void nau_sem_post(nau_sem *sem)
{
    sem_post(sem);
}

void nau_sem_wait(nau_sem *sem)
{
    int ret;
    do {
        ret = sem_wait(sem);
    } while (ret < 0 && errno == EINTR);
}

bool nau_sem_timedwait(nau_sem *sem, int64_t timeout)
{
    /* sem_timedwait() requires an absolute CLOCK_REALTIME deadline. */
    struct timespec date;
    clock_gettime(CLOCK_REALTIME, &date);
    const int64_t ns = date.tv_nsec + timeout % NPRIV_NS_PER_SEC;
    date.tv_sec += (time_t)(timeout / NPRIV_NS_PER_SEC + ns / NPRIV_NS_PER_SEC);
    date.tv_nsec = (long)(ns % NPRIV_NS_PER_SEC);
    int ret;
    do {
        ret = sem_timedwait(sem, &date);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

#endif
