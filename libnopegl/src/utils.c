/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#define _GNU_SOURCE

#ifdef _WIN32
#define POW10_9 1000000000
#include <Windows.h>
#else
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bstr.h"
#include "log.h"
#include "memory.h"
#include "utils.h"
#include "pthread_compat.h"

struct ngli_rc *ngli_rc_ref(struct ngli_rc *s)
{
    s->count++;
    return s;
}

void ngli_rc_unrefp(struct ngli_rc **sp)
{
    struct ngli_rc *s = *sp;
    if (!s)
        return;

    if (s->count-- == 1)
        s->freep((void **)sp);

    *sp = NULL;
}

char *ngli_strdup(const char *s)
{
    if (!s)
        return NULL;
    const size_t len = strlen(s);
    char *r = ngli_malloc(len + 1);
    if (!r)
        return NULL;
    memcpy(r, s, len);
    r[len] = 0;
    return r;
}

int64_t ngli_gettime_relative(void)
{
#ifdef _WIN32
    // reference: https://github.com/mirror/mingw-w64/blob/master/mingw-w64-libraries/winpthreads/src/clock.c
    LARGE_INTEGER pf, pc;
    QueryPerformanceFrequency(&pf);
    QueryPerformanceCounter(&pc);
    int64_t tv_sec = pc.QuadPart / pf.QuadPart;
    int64_t tv_nsec = (int)(((pc.QuadPart % pf.QuadPart) * POW10_9 + (pf.QuadPart >> 1)) / pf.QuadPart);
    if (tv_nsec >= POW10_9) {
        tv_sec++;
        tv_nsec -= POW10_9;
    }
    return tv_sec * 1000000 + tv_nsec / 1000;
#else
    struct timespec ts;
    int ret = clock_gettime(CLOCK_MONOTONIC, &ts);
    return ret != 0 ? 0 : ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

char *ngli_asprintf(const char *fmt, ...)
{
    char *p = NULL;
    va_list va;
    int len;

    va_start(va, fmt);
    len = vsnprintf(NULL, 0, fmt, va);
    va_end(va);
    if (len < 0)
        goto end;

    p = ngli_malloc(len + 1);
    if (!p)
        goto end;

    va_start(va, fmt);
    len = vsnprintf(p, len + 1, fmt, va);
    va_end(va);
    if (len < 0)
        ngli_freep(&p);

end:
    return p;
}

/*
 * The following crc_table is generated with:
 *
 *     uint32_t crc_table[256];
 *     for (int i = 0; i <= 0xff; i++) {
 *         uint32_t crc = i;
 *         for (int j = 0; j < 8; j++)
 *             crc = (crc >> 1) ^ (-(crc & 1) & 0xEDB88320);
 *         crc_table[i] = crc;
 *     }
 *
 *  See: https://en.wikipedia.org/wiki/Cyclic_redundancy_check#CRC-32_algorithm
 */
static const uint32_t crc_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

uint32_t ngli_crc32(const char *s)
{
    uint32_t crc = ~0U;
    for (size_t i = 0; s[i]; i++)
        crc = (crc >> 8) ^ crc_table[(crc & 0xff) ^ (uint8_t)s[i]];
    return ~crc;
}

uint32_t ngli_crc32_mem(const uint8_t *buf, size_t size)
{
    uint32_t crc = ~0U;
    for (size_t i = 0; i < size; i++)
        crc = (crc >> 8) ^ crc_table[(crc & 0xff) ^ buf[i]];
    return ~crc;
}

void ngli_thread_set_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif ((defined(__linux__) && defined(__GLIBC__)) || defined(__ANDROID__))
    pthread_setname_np(pthread_self(), name);
#endif
}

int ngli_get_filesize(const char *filename, int64_t *size)
{
#ifdef _WIN32
    HANDLE file_handle = CreateFile(TEXT(filename), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file_handle == INVALID_HANDLE_VALUE)
        return NGL_ERROR_IO;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle, &file_size)) {
        CloseHandle(file_handle);
        return NGL_ERROR_IO;
    }
    *size = file_size.QuadPart;
    CloseHandle(file_handle);
#else
    struct stat st;
    int ret = stat(filename, &st);
    if (ret == -1) {
        LOG(ERROR, "could not stat '%s': %s", filename, strerror(errno));
        return NGL_ERROR_IO;
    }
    *size = st.st_size;
#endif
    return 0;
}

static int count_lines(const char *s)
{
    int count = 0;
    while (*s) {
        const size_t len = strcspn(s, "\n");
        count++;
        if (!s[len])
            break;
        s += len + 1;
    }
    return count;
}

static int count_digits(int x)
{
    int n = 1;
    while (x /= 10)
        n++;
    return n;
}

char *ngli_numbered_lines(const char *s)
{
    struct bstr *b = ngli_bstr_create();
    if (!b)
        return NULL;

    const int nb_lines = count_lines(s);
    const int nb_digits = count_digits(nb_lines);
    int line = 1;
    while (*s) {
        const size_t len = strcspn(s, "\n");
        ngli_bstr_printf(b, "%*d %.*s\n", nb_digits, line++, (int)len, s);
        if (!s[len])
            break;
        s += len + 1;
    }

    char *ret = ngli_bstr_strdup(b);
    ngli_bstr_freep(&b);
    if (!ret)
        return NULL;

    size_t len = strlen(ret);
    while (len && ret[len - 1] == '\n')
        ret[--len] = 0;

    return ret;
}

int ngli_config_set_debug_defaults(struct ngl_config *config)
{
    switch (config->backend) {
        case NGL_BACKEND_OPENGL:
        case NGL_BACKEND_OPENGLES:
            config->debug |= DEBUG_GL;
            break;
        case NGL_BACKEND_VULKAN:
            config->debug |= DEBUG_VK;
            break;
        default:
            return NGL_ERROR_UNSUPPORTED;
    }
    return config->debug;
}

int ngli_config_copy(struct ngl_config *dst, const struct ngl_config *src)
{
    struct ngl_config tmp = *src;

    if (src->hud_export_filename) {
        tmp.hud_export_filename = ngli_strdup(src->hud_export_filename);
        if (!tmp.hud_export_filename)
            return NGL_ERROR_MEMORY;
    }

    if (src->backend_config) {
        if (src->backend == NGL_BACKEND_OPENGL ||
            src->backend == NGL_BACKEND_OPENGLES) {
            const size_t size = sizeof(struct ngl_config_gl);
            tmp.backend_config = ngli_memdup(src->backend_config, size);
            if (!tmp.backend_config) {
                ngli_freep(&tmp.hud_export_filename);
                return NGL_ERROR_MEMORY;
            }
        } else {
            ngli_freep(&tmp.hud_export_filename);
            LOG(ERROR, "backend_config %p is not supported by backend %d",
                src->backend_config, src->backend);
            return NGL_ERROR_UNSUPPORTED;
        }
    }

    *dst = tmp;

    return 0;
}

void ngli_config_reset(struct ngl_config *config)
{
    ngli_freep(&config->backend_config);
    ngli_freep(&config->hud_export_filename);
    memset(config, 0, sizeof(*config));
}
