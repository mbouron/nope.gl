/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkImageReader.h>

#include <libavutil/log.h>
#include <libavcodec/jni.h>

#include <android/hardware_buffer_jni.h>

#include <nopemd.h>
#include <nopegl/nopegl.h>

#define CHECK(cond) do {                                                                                 \
    if (!(cond)) {                                                                                       \
        __android_log_print(ANDROID_LOG_FATAL, "ngl", "Assert %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
        abort();                                                                                         \
    }                                                                                                    \
} while (0)

struct player {
    struct nmd_ctx *context;
    jobject surface;
};

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopemd_Player_nativeInit(JNIEnv *env,
                                                                    jclass clazz,
                                                                    jstring filename_,
                                                                    jobject surface_)
{
    jobject surface = (*env)->NewGlobalRef(env, surface_);
    CHECK(surface != NULL);

    const char *filename = (*env)->GetStringUTFChars(env, filename_, 0);
    CHECK(filename != NULL);

    struct nmd_ctx *context = nmd_create(filename);
    if (context == NULL) {
        (*env)->ReleaseStringUTFChars(env, filename_, filename);
        (*env)->DeleteGlobalRef(env, surface);
        return 0;
    }
    struct player *player = calloc(1,sizeof(*player));
    if (player == NULL) {
        nmd_freep(&context);
        (*env)->ReleaseStringUTFChars(env, filename_, filename);
        (*env)->DeleteGlobalRef(env, surface);
        return 0;
    }
    player->context = context;
    player->surface = surface;

    nmd_set_option(context, "max_nb_packets", 1);
    nmd_set_option(context, "max_nb_frames", 1);
    nmd_set_option(context, "max_nb_sink", 1);
    nmd_set_option(context, "auto_hwaccel", 1);
    nmd_set_option(context, "opaque", &surface);

    (*env)->ReleaseStringUTFChars(env, filename_, filename);
    return (jlong)(uintptr_t)player;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopemd_Player_nativeStop(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    nmd_stop(player->context);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeStart(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    struct nmd_ctx* context = player->context;
    return nmd_start(context);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeSeek(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong ptr,
                                                                  jdouble position)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    return nmd_seek(player->context, position);
}

JNIEXPORT int JNICALL Java_org_nopeforge_nopemd_Player_nativeDraw(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong ptr,
                                                                  jdouble position)
{
    struct player *player = (struct player *)(uintptr_t)ptr;

    struct nmd_frame *frame = NULL;
    int ret = nmd_get_frame(player->context, position, &frame);
    if (ret == NMD_RET_SUCCESS) {
        if (frame->pix_fmt == NMD_PIXFMT_MEDIACODEC) {
            ret = nmd_mc_frame_render_and_releasep(&frame);
        } else {
            __android_log_print(ANDROID_LOG_WARN, "nmd", "Unsupported frame type: %d", frame->pix_fmt);
            nmd_frame_releasep(&frame);
        }
    } else if (ret == NMD_RET_UNCHANGED) {
        ret = NMD_RET_SUCCESS;
    }
    return ret;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopemd_Player_nativeRelease(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct player *player = (struct player *)(uintptr_t)ptr;
    nmd_freep(&player->context);
    (*env)->DeleteGlobalRef(env, player->surface);
    free(player);
}
