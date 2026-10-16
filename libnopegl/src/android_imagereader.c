/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2020-2022 GoPro Inc.
 * Copyright 2010 The Android Open Source Project
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

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jni.h>
#include <android/native_window.h>
#include <media/NdkImageReader.h>

#include "android_imagereader.h"
#include "jni_utils.h"
#include "log.h"
#include "utils/darray.h"
#include "utils/memory.h"
#include "utils/pthread_compat.h"

/*
 * Each image holds a reference to its `reader`, as deleting the reader also
 * closes its images.
 */
struct android_image {
    struct android_ctx *android_ctx;
    struct android_imagereader *reader;
    AImage *image;
    struct android_image *next;
};

struct android_imagereader {
    atomic_int refcount;
    struct android_ctx *android_ctx;
    AImageReader *reader;
    jobject window;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    int buffer_available;
    NGLI_DARRAY(AHardwareBuffer *) removed_buffers;
    bool removed_overflow; // some removed buffers could not be recorded
};

void ngli_android_get_crop_matrix(float *matrix, const AHardwareBuffer_Desc *desc, const AImageCropRect *crop_rect, int filtering)
{
    float sx = 1.0f, sy = 1.0f, tx = 0.0f, ty = 0.0f;
    const int32_t width = crop_rect->right - crop_rect->left;
    const int32_t height = crop_rect->bottom - crop_rect->top;
    if (width > 0 && height > 0) {
        float shrink = 0.0f;
        if (filtering) {
            /*
             * In order to prevent bilinear sampling beyond the edge of the
             * crop rectangle we shrink a certain amount of texels on each side
             * depending on the buffer format. This logic matches what is done
             * internally in SurfaceTexture.getTransformatMatrix().
             */
            switch (desc->format) {
            case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
            case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
            case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
                /* No subsampling, shrink by half a texel on each side */
                shrink = 0.5f;
                break;
            default:
                 /* Assume YUV420P, shrink by one texel on each side */
                shrink = 1.0f;
            }
        }
        if (width < desc->width) {
            tx = ((float)crop_rect->left + shrink) / (float)desc->width;
            sx = ((float)width - 2.0f * shrink) / (float)desc->width;
        }
        if (height < desc->height) {
            ty = ((float)crop_rect->top + shrink) / (float)desc->height;
            sy = ((float)height - 2.0f * shrink) / (float)desc->height;
        }
    }
    struct ngli_mat4 crop_matrix = {.m = {
        sx,   0.0f, 0.0f, 0.0f,
        0.0f, sy,   0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        tx,   ty,   0.0f, 1.0f,
    }};
    memcpy(matrix, crop_matrix.m, sizeof(crop_matrix.m));
}

AHardwareBuffer *ngli_android_image_get_hardware_buffer(struct android_image *s)
{
    struct android_ctx *android_ctx = s->android_ctx;

    AHardwareBuffer *hardware_buffer;
    media_status_t status = android_ctx->AImage_getHardwareBuffer(s->image, &hardware_buffer);
    if (status != AMEDIA_OK)
        return NULL;
    return hardware_buffer;
}

int ngli_android_image_get_crop_rect(struct android_image *s, AImageCropRect *rect)
{
    struct android_ctx *android_ctx = s->android_ctx;

    media_status_t status = android_ctx->AImage_getCropRect(s->image, rect);
    if (status != AMEDIA_OK)
        return NGL_ERROR_EXTERNAL;
    return 0;
}

static void imagereader_unref(struct android_imagereader *s);

void ngli_android_image_freep(struct android_image **sp)
{
    if (!*sp)
        return;

    struct android_image *s = *sp;
    struct android_ctx *android_ctx = s->android_ctx;
    struct android_imagereader *reader = s->reader;
    android_ctx->AImage_delete(s->image);

    ngli_freep(sp);
    imagereader_unref(reader);
}

void ngli_android_image_releasep(struct android_image **sp, int release_fence_fd)
{
    if (!*sp) {
        if (release_fence_fd >= 0)
            close(release_fence_fd);
        return;
    }

    struct android_image *s = *sp;
    struct android_ctx *android_ctx = s->android_ctx;
    struct android_imagereader *reader = s->reader;
    android_ctx->AImage_deleteAsync(s->image, release_fence_fd);

    ngli_freep(sp);
    imagereader_unref(reader);
}

void ngli_android_image_list_push(struct android_image **listp, struct android_image **imagep)
{
    struct android_image *s = *imagep;
    if (!s)
        return;

    s->next = *listp;
    *listp = s;
    *imagep = NULL;
}

void ngli_android_image_list_releasep(struct android_image **listp, int release_fence_fd)
{
    struct android_image *s = *listp;
    *listp = NULL;

    if (!s && release_fence_fd >= 0)
        close(release_fence_fd);

    while (s) {
        struct android_image *next = s->next;
        int fd = release_fence_fd;
        if (release_fence_fd >= 0 && next) {
            fd = dup(release_fence_fd);
            if (fd < 0) {
                LOG(ERROR, "could not duplicate the release fence");
                struct pollfd pfd = {.fd = release_fence_fd, .events = POLLIN};
                while (poll(&pfd, 1, -1) < 0 && errno == EINTR)
                    ;
                close(release_fence_fd);
                release_fence_fd = -1;
            }
        }
        ngli_android_image_releasep(&s, fd);
        s = next;
    }
}

static void on_buffer_available(void *context, AImageReader *reader)
{
    struct android_imagereader *s = context;
    pthread_mutex_lock(&s->lock);
    s->buffer_available = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void on_buffer_removed(void *context, AImageReader *reader, AHardwareBuffer *buffer)
{
    struct android_imagereader *s = context;
    pthread_mutex_lock(&s->lock);
    if (ngli_darray_try_push(&s->removed_buffers, buffer) < 0)
        s->removed_overflow = true;
    pthread_mutex_unlock(&s->lock);
}

struct android_imagereader *ngli_android_imagereader_create(struct android_ctx *android_ctx, int32_t width, int32_t height, int format, int max_images)
{
    struct android_imagereader *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    atomic_init(&s->refcount, 1);
    s->android_ctx = android_ctx;
    if (pthread_mutex_init(&s->lock, NULL)) {
        ngli_freep(&s);
        return NULL;
    }

    if (pthread_cond_init(&s->cond, NULL)) {
        pthread_mutex_destroy(&s->lock);
        ngli_freep(&s);
        return NULL;
    }

    const uint32_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    media_status_t status = android_ctx->AImageReader_newWithUsage(width, height, format, usage, max_images, &s->reader);
    if (status != AMEDIA_OK) {
        LOG(ERROR, "failed to allocate AImageReader");
        goto fail;
    }

    AImageReader_ImageListener listener = {
        .context = s,
        .onImageAvailable = on_buffer_available,
    };

    status = android_ctx->AImageReader_setImageListener(s->reader, &listener);
    if (status != AMEDIA_OK) {
        LOG(ERROR, "failed to set image listener");
        goto fail;
    }

    AImageReader_BufferRemovedListener removed_listener = {
        .context = s,
        .onBufferRemoved = on_buffer_removed,
    };

    status = android_ctx->AImageReader_setBufferRemovedListener(s->reader, &removed_listener);
    if (status != AMEDIA_OK) {
        LOG(ERROR, "failed to set buffer removed listener");
        goto fail;
    }

    return s;
fail:

    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    android_ctx->AImageReader_delete(s->reader);
    ngli_freep(&s);

    return NULL;
}

int ngli_android_imagereader_get_window(struct android_imagereader *s, void **window)
{
    struct android_ctx *android_ctx = s->android_ctx;

    if (s->window) {
        *window = s->window;
        return 0;
    }

    ANativeWindow *native_window;
    media_status_t status = android_ctx->AImageReader_getWindow(s->reader, &native_window);
    if (status != AMEDIA_OK)
        return NGL_ERROR_EXTERNAL;

    JNIEnv *env = ngli_jni_get_env();
    if (!env)
        return NGL_ERROR_EXTERNAL;

    jobject object = android_ctx->ANativeWindow_toSurface(env, native_window);
    s->window = (*env)->NewGlobalRef(env, object);
    (*env)->DeleteLocalRef(env, object);

    *window = s->window;
    return *window ? 0 : NGL_ERROR_EXTERNAL;
}

int ngli_android_imagereader_acquire_next_image(struct android_imagereader *s, struct android_image **imagep)
{
    struct android_ctx *android_ctx = s->android_ctx;

    AImage *android_image;
    media_status_t status;
    int nb_retries = 0;

    pthread_mutex_lock(&s->lock);
    for (;;) {
        status = android_ctx->AImageReader_acquireNextImage(s->reader, &android_image);
        if (!nb_retries++ && status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&s->cond, &s->lock, &ts);
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&s->lock);

    if (status != AMEDIA_OK)
        return NGL_ERROR_EXTERNAL;

    struct android_image *image = ngli_try_calloc(1, sizeof(*image));
    if (!image) {
        android_ctx->AImage_delete(android_image);
        return NGL_ERROR_MEMORY;
    }
    atomic_fetch_add_explicit(&s->refcount, 1, memory_order_relaxed);
    image->android_ctx = android_ctx;
    image->reader = s;
    image->image = android_image;
    *imagep = image;

    return 0;
}

bool ngli_android_imagereader_flush_removed_buffers(struct android_imagereader *s,
                                                    void (*remove)(void *arg, const AHardwareBuffer *buffer),
                                                    void *arg)
{
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->removed_buffers.count; i++)
        remove(arg, s->removed_buffers.data[i]);
    ngli_darray_clear(&s->removed_buffers);
    const bool overflow = s->removed_overflow;
    s->removed_overflow = false;
    pthread_mutex_unlock(&s->lock);
    return !overflow;
}

static void imagereader_unref(struct android_imagereader *s)
{
    if (atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel) != 1)
        return;

    struct android_ctx *android_ctx = s->android_ctx;
    android_ctx->AImageReader_delete(s->reader);

    ngli_darray_reset(&s->removed_buffers);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);

    JNIEnv *env = ngli_jni_get_env();
    if (env)
        (*env)->DeleteGlobalRef(env, s->window);

    ngli_freep(&s);
}

void ngli_android_imagereader_freep(struct android_imagereader **sp)
{
    if (!*sp)
        return;

    imagereader_unref(*sp);
    *sp = NULL;
}
