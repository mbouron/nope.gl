/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 *
 * This file is available under the terms of either the Apache License,
 * Version 2.0, or the GNU Lesser General Public License, version 2.1 or (at
 * your option) any later version, at your option.
 */

#include <stdbool.h>
#include <stdint.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <libavcodec/jni.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "android_asset.h"
#include "log.h"

#define IO_BUFFER_SIZE 32768

struct android_asset {
    void *log_ctx;
    AAsset *asset;
};

static AAssetManager *get_asset_manager(JNIEnv *env, jobject context)
{
    AAssetManager *manager = NULL;
    jobject jmanager = NULL;

    jclass context_class = (*env)->GetObjectClass(env, context);
    jmethodID get_assets_id = (*env)->GetMethodID(env, context_class, "getAssets",
                                                  "()Landroid/content/res/AssetManager;");
    if (get_assets_id)
        jmanager = (*env)->CallObjectMethod(env, context, get_assets_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        goto end;
    }
    if (jmanager)
        manager = AAssetManager_fromJava(env, jmanager);

end:
    (*env)->DeleteLocalRef(env, jmanager);
    (*env)->DeleteLocalRef(env, context_class);
    return manager;
}

static int open_asset(struct android_asset *s, const char *path)
{
    JavaVM *vm = av_jni_get_java_vm(NULL);
    jobject context = av_jni_get_android_app_ctx();
    if (!vm || !context) {
        LOG(s, ERROR, "the Android application context is not set, unable to open the asset %s", path);
        return AVERROR_EXTERNAL;
    }

    JNIEnv *env = NULL;
    bool attached = false;
    jint jret = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (jret == JNI_EDETACHED) {
        jret = (*vm)->AttachCurrentThread(vm, &env, NULL);
        attached = jret == JNI_OK;
    }
    if (jret != JNI_OK) {
        LOG(s, ERROR, "unable to get a JNI environment (%d)", (int)jret);
        return AVERROR_EXTERNAL;
    }

    int ret = 0;
    AAssetManager *manager = get_asset_manager(env, context);
    if (!manager) {
        LOG(s, ERROR, "unable to get the asset manager of the Android application context");
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    s->asset = AAssetManager_open(manager, path, AASSET_MODE_RANDOM);
    if (!s->asset) {
        LOG(s, ERROR, "unable to open the asset %s", path);
        ret = AVERROR(ENOENT);
    }

end:
    if (attached)
        (*vm)->DetachCurrentThread(vm);
    return ret;
}

static int asset_read(void *opaque, uint8_t *buf, int buf_size)
{
    struct android_asset *s = opaque;

    int n = AAsset_read(s->asset, buf, (size_t)buf_size);
    if (n < 0)
        return AVERROR(EIO);
    if (n == 0)
        return AVERROR_EOF;
    return n;
}

static int64_t asset_seek(void *opaque, int64_t offset, int whence)
{
    struct android_asset *s = opaque;

    if (whence & AVSEEK_SIZE)
        return AAsset_getLength64(s->asset);

    off64_t pos = AAsset_seek64(s->asset, offset, whence & ~AVSEEK_FORCE);
    if (pos < 0)
        return AVERROR(EIO);
    return pos;
}

int nmdi_android_asset_open(void *log_ctx, const char *path, AVIOContext **pbp)
{
    struct android_asset *s = av_mallocz(sizeof(*s));
    if (!s)
        return AVERROR(ENOMEM);
    s->log_ctx = log_ctx;

    int ret = open_asset(s, path);
    if (ret < 0) {
        av_free(s);
        return ret;
    }

    uint8_t *buffer = av_malloc(IO_BUFFER_SIZE);
    if (!buffer) {
        AAsset_close(s->asset);
        av_free(s);
        return AVERROR(ENOMEM);
    }

    AVIOContext *pb = avio_alloc_context(buffer, IO_BUFFER_SIZE, 0, s, asset_read, NULL, asset_seek);
    if (!pb) {
        av_free(buffer);
        AAsset_close(s->asset);
        av_free(s);
        return AVERROR(ENOMEM);
    }

    *pbp = pb;
    return 0;
}

void nmdi_android_asset_close(AVIOContext **pbp)
{
    AVIOContext *pb = *pbp;
    if (!pb)
        return;

    struct android_asset *s = pb->opaque;
    AAsset_close(s->asset);
    av_free(s);
    av_freep(&pb->buffer);
    avio_context_free(pbp);
}
