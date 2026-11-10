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


#include <pthread.h>

#include <jni.h>

#include <libavcodec/jni.h>

#include <nopegl/nopegl_android.h>

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    av_jni_set_java_vm(vm, NULL);
    ngl_jni_set_java_vm(vm);

    return JNI_VERSION_1_6;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static jobject android_context;

static int set_context(JNIEnv *env, jobject context)
{
    if (android_context && (*env)->IsSameObject(env, android_context, context))
        return 0;

    int ret = ngl_android_set_application_context(context);
    if (ret < 0)
        return ret;

    jobject ref = (*env)->NewGlobalRef(env, context);
    if (!ref)
        return -1;

    ret = av_jni_set_android_app_ctx(ref, NULL);
    if (ret < 0) {
        (*env)->DeleteGlobalRef(env, ref);
        return ret;
    }

    if (android_context)
        (*env)->DeleteGlobalRef(env, android_context);

    android_context = ref;
    return 0;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_Nope_nativeInit(
    JNIEnv *env, jclass clazz, jobject context)
{
    pthread_mutex_lock(&lock);
    int ret = set_context(env, context);
    pthread_mutex_unlock(&lock);
    return ret;
}
