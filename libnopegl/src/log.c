/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if !defined(TARGET_IPHONE) && !defined(TARGET_ANDROID) && !defined(TARGET_WINDOWS)
#include <unistd.h>
#endif

#include "log.h"
#include <ngpu/ngpu.h>
#include "utils/error.h"
#include "utils/memory.h"

ngli_printf_format(6, 0)
static void default_callback(void *arg, enum ngl_log_level level, const char *filename, int ln,
                             const char *fn, const char *fmt, va_list vl)
{
    char logline[128];
    char *logbuf = NULL;
    const char *logp = logline;
    static const char * const log_strs[] = {
        [NGL_LOG_DEBUG]   = "DEBUG",
        [NGL_LOG_VERBOSE] = "VERBOSE",
        [NGL_LOG_INFO]    = "INFO",
        [NGL_LOG_WARNING] = "WARNING",
        [NGL_LOG_ERROR]   = "ERROR",
    };

    /* we need a copy because it may be re-used a 2nd time */
    va_list vl_copy;
    va_copy(vl_copy, vl);

    int len = vsnprintf(logline, sizeof(logline), fmt, vl);

    /* handle the case where the line doesn't fit the stack buffer */
    if (len >= sizeof(logline)) {
        const size_t logbuf_size = (size_t)len + 1;
        logbuf = ngli_malloc(logbuf_size);
        if (!logbuf) {
            va_end(vl_copy);
            return;
        }
        vsnprintf(logbuf, logbuf_size, fmt, vl_copy);
        logp = logbuf;
    }

    const char *color_start = "", *color_end = "";

#if !defined(TARGET_IPHONE) && !defined(TARGET_ANDROID) && !defined(TARGET_WINDOWS)
    if (isatty(1) && getenv("TERM") && !getenv("NO_COLOR")) {
        static const char * const colors[] = {
            [NGL_LOG_DEBUG]   = "\033[32m",   // green
            [NGL_LOG_VERBOSE] = "\033[92m",   // green (bright)
            [NGL_LOG_INFO]    = "\033[0m",    // no color
            [NGL_LOG_WARNING] = "\033[93m",   // yellow (bright)
            [NGL_LOG_ERROR]   = "\033[31m",   // red
        };
        color_start = colors[level];
        color_end = "\033[0m";
    }
#endif

    printf("%s[%s] %s:%d %s: %s%s\n", color_start,
           log_strs[level], filename, ln, fn, logp,
           color_end);
    ngli_free(logbuf);
    va_end(vl_copy);
    fflush(stdout);
}

static struct {
    void *user_arg;
    ngl_log_callback_type callback;
    enum ngl_log_level min_level;
} log_ctx = {
    .callback  = default_callback,
    .min_level = NGL_LOG_WARNING,
};

ngli_printf_format(6, 0)
static void default_ngpu_callback(void *arg, enum ngpu_log_level level, const char *filename, int ln,
                                  const char *fn, const char *fmt, va_list vl)
{
    static const enum ngl_log_level log_levels[] = {
        [NGPU_LOG_VERBOSE] = NGL_LOG_VERBOSE,
        [NGPU_LOG_DEBUG]   = NGL_LOG_DEBUG,
        [NGPU_LOG_INFO]    = NGL_LOG_INFO,
        [NGPU_LOG_WARNING] = NGL_LOG_WARNING,
        [NGPU_LOG_ERROR]   = NGL_LOG_ERROR,
    };
    if (level >= NGLI_ARRAY_NB(log_levels))
        return;

    if (log_levels[level] < log_ctx.min_level)
        return;

    log_ctx.callback(log_ctx.user_arg, log_levels[level], filename, ln, fn, fmt, vl);
}

void ngli_log_set_callback(void *arg, ngl_log_callback_type callback)
{
    log_ctx.user_arg = arg;
    log_ctx.callback = callback;

    ngpu_log_set_callback(NULL, default_ngpu_callback);
    ngpu_log_set_min_level(NGPU_LOG_VERBOSE);
}

void ngli_log_set_min_level(enum ngl_log_level level)
{
    log_ctx.min_level = level;

    ngpu_log_set_callback(NULL, default_ngpu_callback);
    ngpu_log_set_min_level(NGPU_LOG_VERBOSE);
}

void ngli_log_print(enum ngl_log_level log_level, const char *filename,
                    int ln, const char *fn, const char *fmt, ...)
{
    va_list arg_list;

    if (log_level < log_ctx.min_level)
        return;

    va_start(arg_list, fmt);
    log_ctx.callback(log_ctx.user_arg, log_level, filename, ln, fn, fmt, arg_list);
    va_end(arg_list);
}

char *ngli_log_ret_str(char *buf, size_t buf_size, int ret) {
  snprintf(buf, buf_size, "%s", ngli_error_to_string(ret));
  return buf;
}
