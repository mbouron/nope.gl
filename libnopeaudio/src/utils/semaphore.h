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

#ifndef NAU_SEMAPHORE_H
#define NAU_SEMAPHORE_H

/*
 * The render thread uses semaphores to wake decoding threads without taking
 * the mutex required by a condition variable. Apple platforms use dispatch
 * semaphores because they do not support POSIX unnamed semaphores.
 *
 * nau_sem_timedwait() takes a timeout in nanoseconds and returns true if the
 * semaphore was acquired. On POSIX, the deadline uses CLOCK_REALTIME, so clock
 * adjustments can change the wait duration.
 */

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE nau_sem;
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t nau_sem;
#else
#include <semaphore.h>
typedef sem_t nau_sem;
#endif

int nau_sem_init(nau_sem *sem);
void nau_sem_destroy(nau_sem *sem);
void nau_sem_post(nau_sem *sem);
void nau_sem_wait(nau_sem *sem);
bool nau_sem_timedwait(nau_sem *sem, int64_t timeout);

#endif
