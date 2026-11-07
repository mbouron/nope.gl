/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_EXAMPLE_WAIT_H
#define NAE_EXAMPLE_WAIT_H
#ifdef _WIN32
#include <windows.h>
static void wait_ms(unsigned ms)
{
    Sleep(ms);
}
#else
#include <errno.h>
#include <time.h>
static void wait_ms(unsigned ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
#endif
#endif
