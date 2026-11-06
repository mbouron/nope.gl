/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_THREAD_H
#define NAE_THREAD_H

/*
 * Threads, and semaphores a real-time thread can post to without waiting:
 * POSIX unnamed semaphores are not available on Apple platforms, which have
 * dispatch semaphores instead.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>

typedef HANDLE nae_thread;
typedef HANDLE nae_sem;

struct nae_thread_start {
    void *(*function)(void *);
    void *arg;
};

static unsigned __stdcall nae_thread_run(void *arg)
{
    struct nae_thread_start start = *(struct nae_thread_start *)arg;
    free(arg);
    start.function(start.arg);
    return 0;
}

static inline int nae_thread_create(nae_thread *thread, void *(*function)(void *), void *arg)
{
    struct nae_thread_start *start = malloc(sizeof(*start));
    if (!start)
        return -1;
    *start = (struct nae_thread_start){function, arg};
    *thread = (HANDLE)_beginthreadex(NULL, 0, nae_thread_run, start, 0, NULL);
    if (!*thread) {
        free(start);
        return -1;
    }
    return 0;
}

static inline void nae_thread_join(nae_thread thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

static inline void nae_thread_set_name(const char *name) { (void)name; }

typedef SRWLOCK nae_mutex;
static inline void nae_mutex_init(nae_mutex *mutex) { InitializeSRWLock(mutex); }
static inline void nae_mutex_destroy(nae_mutex *mutex) { (void)mutex; }
static inline void nae_mutex_lock(nae_mutex *mutex) { AcquireSRWLockExclusive(mutex); }
static inline void nae_mutex_unlock(nae_mutex *mutex) { ReleaseSRWLockExclusive(mutex); }
static inline bool nae_mutex_trylock(nae_mutex *mutex) { return TryAcquireSRWLockExclusive(mutex); }

static inline int nae_sem_init(nae_sem *sem)
{
    *sem = CreateSemaphoreA(NULL, 0, LONG_MAX, NULL);
    return *sem ? 0 : -1;
}

static inline void nae_sem_destroy(nae_sem *sem) { CloseHandle(*sem); }
static inline void nae_sem_post(nae_sem *sem) { ReleaseSemaphore(*sem, 1, NULL); }
static inline void nae_sem_wait(nae_sem *sem) { WaitForSingleObject(*sem, INFINITE); }

#else
#include <pthread.h>

typedef pthread_t nae_thread;

static inline int nae_thread_create(nae_thread *thread, void *(*function)(void *), void *arg)
{
    return pthread_create(thread, NULL, function, arg) ? -1 : 0;
}

static inline void nae_thread_join(nae_thread thread)
{
    pthread_join(thread, NULL);
}

typedef pthread_mutex_t nae_mutex;
static inline void nae_mutex_init(nae_mutex *mutex) { pthread_mutex_init(mutex, NULL); }
static inline void nae_mutex_destroy(nae_mutex *mutex) { pthread_mutex_destroy(mutex); }
static inline void nae_mutex_lock(nae_mutex *mutex) { pthread_mutex_lock(mutex); }
static inline void nae_mutex_unlock(nae_mutex *mutex) { pthread_mutex_unlock(mutex); }
static inline bool nae_mutex_trylock(nae_mutex *mutex) { return pthread_mutex_trylock(mutex) == 0; }

static inline void nae_thread_set_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__ANDROID__) || (defined(__linux__) && defined(_GNU_SOURCE))
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

#if defined(__APPLE__)
#include <dispatch/dispatch.h>

typedef dispatch_semaphore_t nae_sem;

static inline int nae_sem_init(nae_sem *sem)
{
    *sem = dispatch_semaphore_create(0);
    return *sem ? 0 : -1;
}

static inline void nae_sem_destroy(nae_sem *sem) { dispatch_release(*sem); }
static inline void nae_sem_post(nae_sem *sem) { dispatch_semaphore_signal(*sem); }
static inline void nae_sem_wait(nae_sem *sem) { dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER); }

#else
#include <errno.h>
#include <semaphore.h>

typedef sem_t nae_sem;

static inline int nae_sem_init(nae_sem *sem) { return sem_init(sem, 0, 0); }
static inline void nae_sem_destroy(nae_sem *sem) { sem_destroy(sem); }
static inline void nae_sem_post(nae_sem *sem) { sem_post(sem); }

static inline void nae_sem_wait(nae_sem *sem)
{
    while (sem_wait(sem) && errno == EINTR) {
    }
}
#endif
#endif

#endif
