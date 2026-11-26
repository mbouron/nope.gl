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

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <android/hardware_buffer_jni.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkImageReader.h>

/*
 * Unlike android.media.ImageReader, images are acquired without waiting for
 * the producer's GPU work and are handed back with nope.gl's release fence.
 * The producer waits for that fence before drawing into the buffer again.
 *
 * The reader is deleted once closed and every image has been released, as
 * deleting it also deletes the images nope.gl may still hold.
 */
struct canvas_queue {
    AImageReader *reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint64_t frames;
    int outstanding;
    bool closed;
};

static void on_image_available(void *context, AImageReader *reader)
{
    struct canvas_queue *q = context;

    pthread_mutex_lock(&q->lock);
    q->frames++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static void queue_free(struct canvas_queue *q)
{
    if (q->reader) {
        AImageReader_setImageListener(q->reader, NULL);
        AImageReader_delete(q->reader);
    }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->lock);
    free(q);
}

JNIEXPORT jlong JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeCreate(
    JNIEnv *env, jobject thiz, jint width, jint height, jint format,
    jlong usage, jint max_images)
{
    struct canvas_queue *q = calloc(1, sizeof(*q));
    if (!q)
        return 0;

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&q->cond, &attr);
    pthread_condattr_destroy(&attr);
    pthread_mutex_init(&q->lock, NULL);

    media_status_t ret = AImageReader_newWithUsage(
        width, height, format, (uint64_t)usage, max_images, &q->reader);
    if (ret != AMEDIA_OK) {
        q->reader = NULL;
        queue_free(q);
        return 0;
    }

    AImageReader_ImageListener listener = {
        .context = q,
        .onImageAvailable = on_image_available,
    };
    if (AImageReader_setImageListener(q->reader, &listener) != AMEDIA_OK) {
        queue_free(q);
        return 0;
    }

    return (jlong)(uintptr_t)q;
}

JNIEXPORT jobject JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeGetSurface(
    JNIEnv *env, jobject thiz, jlong ptr)
{
    struct canvas_queue *q = (struct canvas_queue *)(uintptr_t)ptr;

    ANativeWindow *window = NULL;
    if (AImageReader_getWindow(q->reader, &window) != AMEDIA_OK)
        return NULL;
    return ANativeWindow_toSurface(env, window);
}

JNIEXPORT jlong JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeAcquire(
    JNIEnv *env, jobject thiz, jlong ptr, jint timeout_ms, jintArray fence_out)
{
    struct canvas_queue *q = (struct canvas_queue *)(uintptr_t)ptr;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    AImage *image = NULL;
    int fence_fd = -1;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        const uint64_t frames = q->frames;
        pthread_mutex_unlock(&q->lock);
        media_status_t ret =
            AImageReader_acquireNextImageAsync(q->reader, &image, &fence_fd);
        pthread_mutex_lock(&q->lock);
        if (ret == AMEDIA_OK)
            break;
        if (ret != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
            goto fail;
        /* HWUI queues the frame asynchronously after syncAndDraw() */
        while (q->frames == frames) {
            if (pthread_cond_timedwait(&q->cond, &q->lock, &deadline) ==
                ETIMEDOUT)
                goto fail;
        }
    }
    pthread_mutex_unlock(&q->lock);

    q->outstanding++;
    jint fd = fence_fd;
    (*env)->SetIntArrayRegion(env, fence_out, 0, 1, &fd);
    return (jlong)(uintptr_t)image;

fail:
    pthread_mutex_unlock(&q->lock);
    return 0;
}

JNIEXPORT jobject JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeGetHardwareBuffer(
    JNIEnv *env, jobject thiz, jlong image_ptr)
{
    AImage *image = (AImage *)(uintptr_t)image_ptr;

    AHardwareBuffer *buffer = NULL;
    if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK)
        return NULL;
    return AHardwareBuffer_toHardwareBuffer(env, buffer);
}

JNIEXPORT void JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeRelease(
    JNIEnv *env, jobject thiz, jlong ptr, jlong image_ptr, jint fence_fd)
{
    struct canvas_queue *q = (struct canvas_queue *)(uintptr_t)ptr;
    AImage *image = (AImage *)(uintptr_t)image_ptr;

    AImage_deleteAsync(image, fence_fd >= 0 ? fence_fd : -1);
    q->outstanding--;
    if (q->closed && !q->outstanding)
        queue_free(q);
}

JNIEXPORT void JNICALL
Java_org_nopeforge_nopegl_NGLAndroidCanvasImageQueue_nativeClose(JNIEnv *env,
                                                                 jobject thiz,
                                                                 jlong ptr)
{
    struct canvas_queue *q = (struct canvas_queue *)(uintptr_t)ptr;

    q->closed = true;
    if (!q->outstanding)
        queue_free(q);
}
