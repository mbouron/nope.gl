/*
 * Copyright 2023-2024 Nope Forge
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

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    av_jni_set_java_vm(vm, NULL);
    ngl_jni_set_java_vm(vm);

    return JNI_VERSION_1_6;
}

__attribute__((__format__(__printf__, 3, 0)))
static void av_android_log(void *arg, int level, const char *fmt, va_list vl)
{
    int current_level = av_log_get_level();
    if (level > current_level)
        return;
    static const int levels[] = {
        [AV_LOG_TRACE]   = ANDROID_LOG_VERBOSE,
        [AV_LOG_VERBOSE] = ANDROID_LOG_VERBOSE,
        [AV_LOG_DEBUG]   = ANDROID_LOG_DEBUG,
        [AV_LOG_INFO]    = ANDROID_LOG_INFO,
        [AV_LOG_WARNING] = ANDROID_LOG_WARN,
        [AV_LOG_ERROR]   = ANDROID_LOG_ERROR,
    };
    const int mapped            = level >= 0 && level < FF_ARRAY_ELEMS(levels);
    const int android_log_level = mapped ? levels[level] : ANDROID_LOG_VERBOSE;
    __android_log_vprint(android_log_level, "ffmpeg", fmt, vl);
}

__attribute__((__format__(__printf__, 6, 0)))
static void ngl_android_log(void *arg,
                            enum ngl_log_level level,
                            const char *filename,
                            int ln,
                            const char *fn,
                            const char *fmt,
                            va_list vl)
{
    static const int levels[] = {
        [NGL_LOG_VERBOSE] = ANDROID_LOG_VERBOSE,
        [NGL_LOG_DEBUG]   = ANDROID_LOG_DEBUG,
        [NGL_LOG_INFO]    = ANDROID_LOG_INFO,
        [NGL_LOG_WARNING] = ANDROID_LOG_WARN,
        [NGL_LOG_ERROR]   = ANDROID_LOG_ERROR,
    };
    const int mapped            = level >= 0 && level < FF_ARRAY_ELEMS(levels);
    const int android_log_level = mapped ? levels[level] : ANDROID_LOG_VERBOSE;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    __android_log_print(android_log_level, "ngl", "%s:%d %s: %s", filename, ln, fn, buf);
}

static struct {
    const char *level;
    int av_log_level;
    enum ngl_log_level ngl_log_level;
} log_levels[] = {
    {"verbose", AV_LOG_DEBUG,   NGL_LOG_VERBOSE},
    {"debug",   AV_LOG_DEBUG,   NGL_LOG_DEBUG  },
    {"info",    AV_LOG_INFO,    NGL_LOG_INFO   },
    {"warning", AV_LOG_WARNING, NGL_LOG_WARNING},
    {"error",   AV_LOG_ERROR,   NGL_LOG_ERROR  },
};

static void set_log_levels(const char *log_level)
{
    int av_log_level  = AV_LOG_INFO;
    enum ngl_log_level ngl_log_level = NGL_LOG_INFO;
    for (size_t i = 0; i < sizeof(log_levels) / sizeof(*log_levels); i++) {
        if (!strcmp(log_levels[i].level, log_level)) {
            av_log_level  = log_levels[i].av_log_level;
            ngl_log_level = log_levels[i].ngl_log_level;
            break;
        }
    }

    av_log_set_level(av_log_level);
    av_log_set_callback(av_android_log);

    ngl_log_set_min_level(ngl_log_level);
    ngl_log_set_callback(NULL, ngl_android_log);
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeInit(JNIEnv *env,
                                                                       jclass type,
                                                                       jobject context,
                                                                       jstring log_level)
{
    if (log_level) {
        const char *log_level_str = (*env)->GetStringUTFChars(env, log_level, 0);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            return;
        }
        set_log_levels(log_level_str);
        (*env)->ReleaseStringUTFChars(env, log_level, log_level_str);
    }

    ngl_android_set_application_context(context);
    av_jni_set_android_app_ctx((*env)->NewGlobalRef(env, context), NULL);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeCreateNativeWindow(JNIEnv *env,
                                                                                      jclass type,
                                                                                      jobject surface)
{
    return (jlong)ANativeWindow_fromSurface(env, surface);
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeReleaseNativeWindow(JNIEnv *env,
                                                                                      jclass type,
                                                                                      jlong native_ptr)
{
    if (native_ptr)
        ANativeWindow_release((ANativeWindow *)native_ptr);
}

enum jni_type {
    JNI_TYPE_INT,
    JNI_TYPE_PTR,
    JNI_TYPE_BOOL,
    JNI_TYPE_FLOAT,
    JNI_TYPE_INT_ARRAY,
    JNI_TYPE_FLOAT_ARRAY,
    JNI_TYPE_BYTE_BUFFER,
};

const char *jni_type_str[] = {
    [JNI_TYPE_INT]         = "I",
    [JNI_TYPE_PTR]         = "J",
    [JNI_TYPE_BOOL]        = "Z",
    [JNI_TYPE_FLOAT]       = "F",
    [JNI_TYPE_INT_ARRAY]   = "[I",
    [JNI_TYPE_FLOAT_ARRAY] = "[F",
    [JNI_TYPE_BYTE_BUFFER] = "Ljava/nio/ByteBuffer;",
};

#define OFFSET(x) offsetof(struct ngl_config, x)
static const struct {
    const char *name;
    enum jni_type type;
    int offset;
} config_fields[] = {
    {"backend",       JNI_TYPE_INT,         OFFSET(backend)        },
    {"window",        JNI_TYPE_PTR,         OFFSET(window)         },
    {"offscreen",     JNI_TYPE_BOOL,        OFFSET(offscreen)      },
    {"width",         JNI_TYPE_INT,         OFFSET(width)          },
    {"height",        JNI_TYPE_INT,         OFFSET(height)         },
    {"samples",       JNI_TYPE_INT,         OFFSET(samples)        },
    {"swapInterval",  JNI_TYPE_INT,         OFFSET(swap_interval)  },
    {"disableDepth",  JNI_TYPE_BOOL,        OFFSET(disable_depth)  },
    {"setSurfacePts", JNI_TYPE_BOOL,        OFFSET(set_surface_pts)},
    {"clearColor",    JNI_TYPE_FLOAT_ARRAY, OFFSET(clear_color)    },
    {"captureBuffer", JNI_TYPE_BYTE_BUFFER, OFFSET(capture_buffer) },
    {"hud",           JNI_TYPE_BOOL,        OFFSET(hud)            },
    {"hudScale",      JNI_TYPE_INT,         OFFSET(hud_scale)      },
    {"debug",         JNI_TYPE_BOOL,        OFFSET(debug)          },
};

static void config_init(struct ngl_config *config, JNIEnv *env, jobject config_)
{
    jclass cls = (*env)->GetObjectClass(env, config_);
    CHECK(cls);

    for (int i = 0; i < sizeof(config_fields) / sizeof(*config_fields); i++) {
        const char *name         = config_fields[i].name;
        const enum jni_type type = config_fields[i].type;
        const char *sig          = jni_type_str[type];
        void *dst                = (void *)((uintptr_t)config + config_fields[i].offset);

        jfieldID field_id = (*env)->GetFieldID(env, cls, name, sig);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            continue;
        }

        switch (type) {
        case JNI_TYPE_INT: {
            const int value = (*env)->GetIntField(env, config_, field_id);
            memcpy(dst, &value, sizeof(value));
            break;
        }
        case JNI_TYPE_PTR: {
            const uintptr_t value = (uintptr_t)(*env)->GetLongField(env, config_, field_id);
            memcpy(dst, &value, sizeof(value));
            break;
        }
        case JNI_TYPE_BOOL: {
            const int value = (*env)->GetBooleanField(env, config_, field_id);
            memcpy(dst, &value, sizeof(value));
            break;
        }
        case JNI_TYPE_FLOAT: {
            const float value = (*env)->GetFloatField(env, config_, field_id);
            memcpy(dst, &value, sizeof(value));
            break;
        }
        case JNI_TYPE_INT_ARRAY: {
            jobject array = (*env)->GetObjectField(env, config_, field_id);
            (*env)->GetIntArrayRegion(env, array, 0, 4, dst);
            break;
        }
        case JNI_TYPE_FLOAT_ARRAY: {
            jobject array = (*env)->GetObjectField(env, config_, field_id);
            (*env)->GetFloatArrayRegion(env, array, 0, 4, dst);
            break;
        }
        case JNI_TYPE_BYTE_BUFFER: {
            jobject buffer = (*env)->GetObjectField(env, config_, field_id);
            if (buffer) {
                void *capture_buffer = (*env)->GetDirectBufferAddress(env, buffer);
                memcpy(dst, &capture_buffer, sizeof(capture_buffer));
            }
            break;
        }
        default:
            CHECK(0);
        }

        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }
    }

    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeCreate(JNIEnv *env, jclass type)
{
    struct ngl_ctx *ctx = ngl_create();
    return (jlong)ctx;
}

JNIEXPORT JNIEXPORT jlongArray JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeIntersectingNodes(JNIEnv *env,
                                                                                                    jobject thiz,
                                                                                                    jlong native_ptr,
                                                                                                    jfloat x,
                                                                                                    jfloat y)
{
    const float point[2] = {x, y};

    size_t nb_nodes         = 0;
    struct ngl_node **nodes = NULL;

    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;
    int result = ngl_get_nodes_intersecting_point(ctx, point, &nb_nodes, &nodes);
    if (result < 0)
        return NULL;

    jsize count = (jsize)nb_nodes;
    jlongArray array = (*env)->NewLongArray(env, count);
    if (!array)
        return NULL;

    for (jsize i = 0; i < count; i++) {
        struct ngl_node *node = nodes[i];
        jlong nativePtr       = (jlong)(uintptr_t)node;
        (*env)->SetLongArrayRegion(env, array, i, 1, &nativePtr);
    }

    return array;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeConfigure(JNIEnv *env,
                                                                            jclass type,
                                                                            jlong native_ptr,
                                                                            jobject config_)
{
    struct ngl_ctx *ctx      = (struct ngl_ctx *)(uintptr_t)native_ptr;
    struct ngl_config config = {0};
    config_init(&config, env, config_);

    return ngl_configure(ctx, &config);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeSetScene(JNIEnv *env,
                                                                           jclass type,
                                                                           jlong native_ptr,
                                                                           jlong scene_native_ptr)
{
    struct ngl_ctx *ctx     = (struct ngl_ctx *)(uintptr_t)native_ptr;
    struct ngl_scene *scene = (struct ngl_scene *)(uintptr_t)scene_native_ptr;

    return ngl_set_scene(ctx, scene);
}

JNIEXPORT jint JNICALL
Java_org_nopeforge_nopegl_NGLContext_nativeResize(JNIEnv *env, jclass type, jlong native_ptr, jint width, jint height)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;

    return ngl_resize(ctx, width, height);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeDraw(JNIEnv *env,
                                                                       jclass type,
                                                                       jlong native_ptr,
                                                                       jdouble time)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;
    return ngl_draw(ctx, time);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeUpdate(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong native_ptr,
                                                                         jdouble time)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;
    return ngl_update(ctx, time);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeSetCaptureBuffer(JNIEnv *env,
                                                                                   jobject thiz,
                                                                                   jlong native_ptr,
                                                                                   jobject buffer)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;

    void *capture_buffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!capture_buffer) {
        return -1;
    }

    return ngl_set_capture_buffer(ctx, capture_buffer);
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeRelease(JNIEnv *env, jclass type, jlong native_ptr)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;

    ngl_freep(&ctx);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLContext_nativeResetScene(JNIEnv *env, jclass type, jlong native_ptr, jboolean clear)
{
    struct ngl_ctx *ctx = (struct ngl_ctx *)(uintptr_t)native_ptr;

    ngl_set_scene(ctx, NULL);
    if (clear) {
        ngl_draw(ctx, 0.0);
    }

    return 0;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeRef(JNIEnv *env, jclass clazz, jlong native_ptr)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    ngl_node_ref(node);
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeUnref(JNIEnv *env, jclass clazz, jlong native_ptr)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    ngl_node_unrefp(&node);
}

JNIEXPORT jstring JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeGetLabel(JNIEnv *env, jobject thiz, jlong native_ptr)
{
    const char *label     = NULL;
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    ngl_node_get_label(node, &label);

    return (*env)->NewStringUTF(env, label);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeGetType(JNIEnv *env, jobject thiz, jlong native_ptr)
{
    uint32_t type;
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    ngl_node_get_type(node, &type);

    return (jint)type;
}

JNIEXPORT jfloatArray JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeGetBoundingBox(JNIEnv *env,
                                                                                     jobject thiz,
                                                                                     jlong native_ptr)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    struct ngl_bounding_box bbox;
    int result = ngl_node_get_bounding_box(node, &bbox);
    if (result < 0)
        return NULL;

    jfloatArray array = (*env)->NewFloatArray(env, 4);
    if (!array)
        return NULL;

    (*env)->SetFloatArrayRegion(env, array, 0, 1, &bbox.center[0]);
    (*env)->SetFloatArrayRegion(env, array, 1, 1, &bbox.center[1]);
    (*env)->SetFloatArrayRegion(env, array, 2, 1, &bbox.extent[0]);
    (*env)->SetFloatArrayRegion(env, array, 3, 1, &bbox.extent[1]);

    return array;
}

JNIEXPORT jfloatArray JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeGetOrientedBoundingBox(JNIEnv *env,
                                                                                             jobject thiz,
                                                                                             jlong native_ptr)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    struct ngl_oriented_bounding_box obb;
    int result = ngl_node_get_oriented_bounding_box(node, &obb);
    if (result < 0)
        return NULL;

    jfloatArray array = (*env)->NewFloatArray(env, 7);
    if (!array)
        return NULL;

    (*env)->SetFloatArrayRegion(env, array, 0, 1, &obb.center[0]);
    (*env)->SetFloatArrayRegion(env, array, 1, 1, &obb.center[1]);
    (*env)->SetFloatArrayRegion(env, array, 2, 1, &obb.extent[0]);
    (*env)->SetFloatArrayRegion(env, array, 3, 1, &obb.extent[1]);
    (*env)->SetFloatArrayRegion(env, array, 4, 1, &obb.rotation);

    return array;
}

#define DECLARE_SET_VEC_FUNC(ctype, jtype, name, count)                                                 \
    static jint set_##name(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jfloatArray array) \
    {                                                                                                   \
        struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;                               \
                                                                                                        \
        const char *key_str = (*env)->GetStringUTFChars(env, key, 0);                                   \
        if ((*env)->ExceptionCheck(env)) {                                                              \
            (*env)->ExceptionClear(env);                                                                \
            return -1;                                                                                  \
        }                                                                                               \
                                                                                                        \
        jsize length = (*env)->GetArrayLength(env, array);                                              \
        CHECK(length == count);                                                                         \
                                                                                                        \
        ctype vec[16] = {0};                                                                            \
        (*env)->Get##jtype##ArrayRegion(env, array, 0, count, (void *)vec);                             \
        if ((*env)->ExceptionCheck(env)) {                                                              \
            (*env)->ExceptionClear(env);                                                                \
            (*env)->ReleaseStringUTFChars(env, key, key_str);                                           \
            return -1;                                                                                  \
        }                                                                                               \
                                                                                                        \
        int ret = ngl_node_param_set_##name(node, key_str, vec);                                        \
        (*env)->ReleaseStringUTFChars(env, key, key_str);                                               \
                                                                                                        \
        return ret;                                                                                     \
    }

DECLARE_SET_VEC_FUNC(float, Float, vec2, 2)
DECLARE_SET_VEC_FUNC(float, Float, vec3, 3)
DECLARE_SET_VEC_FUNC(float, Float, vec4, 4)
DECLARE_SET_VEC_FUNC(int32_t, Int, ivec2, 2)
DECLARE_SET_VEC_FUNC(int32_t, Int, ivec3, 3)
DECLARE_SET_VEC_FUNC(int32_t, Int, ivec4, 4)
DECLARE_SET_VEC_FUNC(uint32_t, Int, uvec2, 2)
DECLARE_SET_VEC_FUNC(uint32_t, Int, uvec3, 3)
DECLARE_SET_VEC_FUNC(uint32_t, Int, uvec4, 4)
DECLARE_SET_VEC_FUNC(float, Float, mat4, 16)

#define DECLARE_SET_TYPE_FUNC(ctype, jtype, name)                                                 \
    static jint set_##name(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, ctype value) \
    {                                                                                             \
        struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;                         \
                                                                                                  \
        const char *key_str = (*env)->GetStringUTFChars(env, key, 0);                             \
        assert(key_str);                                                                          \
                                                                                                  \
        int ret = ngl_node_param_set_##name(node, key_str, value);                                \
                                                                                                  \
        (*env)->ReleaseStringUTFChars(env, key, key_str);                                         \
                                                                                                  \
        return ret;                                                                               \
    }

DECLARE_SET_TYPE_FUNC(float, Float, f32)
DECLARE_SET_TYPE_FUNC(double, Double, f64)
DECLARE_SET_TYPE_FUNC(int32_t, Int, i32)
DECLARE_SET_TYPE_FUNC(uint32_t, Int, u32)
DECLARE_SET_TYPE_FUNC(bool, Boolean, bool)
DECLARE_SET_TYPE_FUNC(struct ngl_node *, Long, node)

#define DECLARE_SET_STR_FUNC(name)                                                                  \
    static jint set_##name(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jstring value) \
    {                                                                                               \
        struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;                           \
        const char *key_str   = (*env)->GetStringUTFChars(env, key, 0);                             \
        const char *value_str = (*env)->GetStringUTFChars(env, value, 0);                           \
        CHECK(key_str);                                                                             \
        CHECK(value_str);                                                                           \
                                                                                                    \
        int ret = ngl_node_param_set_##name(node, key_str, value_str);                              \
                                                                                                    \
        (*env)->ReleaseStringUTFChars(env, key, key_str);                                           \
        (*env)->ReleaseStringUTFChars(env, value, value_str);                                       \
                                                                                                    \
        return ret;                                                                                 \
    }

DECLARE_SET_STR_FUNC(str)

DECLARE_SET_STR_FUNC(flags)

DECLARE_SET_STR_FUNC(select)

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetBoolean(JNIEnv *env,
                                                                          jobject thiz,
                                                                          jlong native_ptr,
                                                                          jstring key,
                                                                          jboolean value)
{
    return set_bool(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetData(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jlong size,
                                                                       jobject data)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    const char *key_str = (*env)->GetStringUTFChars(env, key, 0);
    CHECK(key_str);

    void *data_buffer          = (*env)->GetDirectBufferAddress(env, data);
    jlong data_buffer_capacity = (*env)->GetDirectBufferCapacity(env, data);

    CHECK(data_buffer);
    CHECK(size >= 0 && size <= data_buffer_capacity);

    int ret = ngl_node_param_set_data(node, key_str, (size_t)size, data_buffer);

    (*env)->ReleaseStringUTFChars(env, key, key_str);

    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetDict(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jstring name,
                                                                       jlong value)
{
    struct ngl_node *node       = (struct ngl_node *)(uintptr_t)native_ptr;
    struct ngl_node *value_node = (struct ngl_node *)(uintptr_t)value;

    const char *key_str  = (*env)->GetStringUTFChars(env, key, 0);
    const char *name_str = (*env)->GetStringUTFChars(env, name, 0);
    CHECK(key_str);
    CHECK(name_str);

    int ret = ngl_node_param_set_dict(node, key_str, name_str, value_node);

    (*env)->ReleaseStringUTFChars(env, key, key_str);
    (*env)->ReleaseStringUTFChars(env, name, name_str);

    return ret;
}

JNIEXPORT jint JNICALL
Java_org_nopeforge_nopegl_NGLNode_nativeSetFloat(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jfloat value)
{
    return set_f32(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetDouble(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong native_ptr,
                                                                         jstring key,
                                                                         jdouble value)
{
    return set_f64(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeAddDoubles(JNIEnv *env,
                                                                          jobject thiz,
                                                                          jlong native_ptr,
                                                                          jstring key,
                                                                          jint count,
                                                                          jdoubleArray values)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;
    const char *key_str   = (*env)->GetStringUTFChars(env, key, 0);
    const jsize length    = (*env)->GetArrayLength(env, values);
    CHECK(key_str);
    CHECK(length == count);

    double *f64s = (*env)->GetDoubleArrayElements(env, values, NULL);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->ReleaseStringUTFChars(env, key, key_str);
        return -1;
    }

    int ret = ngl_node_param_add_f64s(node, key_str, count, f64s);

    (*env)->ReleaseDoubleArrayElements(env, values, f64s, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, key, key_str);
    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSwapElement(JNIEnv *env,
                                                                           jobject thiz,
                                                                           jlong native_ptr,
                                                                           jstring key,
                                                                           jint from,
                                                                           jint to)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    const char *key_str = (*env)->GetStringUTFChars(env, key, 0);
    CHECK(key_str);

    int ret = ngl_node_param_swap_elem(node, key_str, from, to);

    (*env)->ReleaseStringUTFChars(env, key, key_str);

    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetFlags(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jstring value)
{
    return set_flags(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL
Java_org_nopeforge_nopegl_NGLNode_nativeSetInt(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jint value)
{
    return set_i32(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetIVec2(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_ivec2(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetIVec3(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_ivec3(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetIVec4(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_ivec4(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetMat4(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jfloatArray value)
{
    return set_mat4(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL
Java_org_nopeforge_nopegl_NGLNode_nativeSetNode(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jlong value)
{
    return set_node(env, thiz, native_ptr, key, (struct ngl_node *)(uintptr_t)value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeAddNodes(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jsize count,
                                                                        jlongArray node_pointers)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;
    const char *key_str   = (*env)->GetStringUTFChars(env, key, 0);
    CHECK(key_str);
    jsize length = (*env)->GetArrayLength(env, node_pointers);
    CHECK(length == count);

    jlong *ptrs = (*env)->GetLongArrayElements(env, node_pointers, NULL);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->ReleaseStringUTFChars(env, key, key_str);
        return -1;
    }

    struct ngl_node **nodes = calloc(count, sizeof(*nodes));
    if (!nodes) {
        (*env)->ReleaseStringUTFChars(env, key, key_str);
        (*env)->ReleaseLongArrayElements(env, node_pointers, ptrs, JNI_ABORT);
        return -1;
    }

    for (jsize i = 0; i < count; i++)
        nodes[i] = (struct ngl_node *)(uintptr_t)ptrs[i];

    int ret = ngl_node_param_add_nodes(node, key_str, count, nodes);

    free(nodes);

    (*env)->ReleaseStringUTFChars(env, key, key_str);
    (*env)->ReleaseLongArrayElements(env, node_pointers, ptrs, JNI_ABORT);

    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeTimeRangeFilterUpdate(JNIEnv *env,
                                                                                     jobject thiz,
                                                                                     jlong native_ptr,
                                                                                     jdouble start_time,
                                                                                     jdouble end_time)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;
    return ngl_timerangefilter_set_range(node, start_time, end_time);
}


JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetRational(JNIEnv *env,
                                                                           jobject thiz,
                                                                           jlong native_ptr,
                                                                           jstring key,
                                                                           jint num,
                                                                           jint den)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    const char *key_str = (*env)->GetStringUTFChars(env, key, 0);
    CHECK(key_str);

    int ret = ngl_node_param_set_rational(node, key_str, num, den);

    (*env)->ReleaseStringUTFChars(env, key, key_str);

    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetSelect(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong native_ptr,
                                                                         jstring key,
                                                                         jstring value)
{
    return set_select(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetString(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong native_ptr,
                                                                         jstring key,
                                                                         jstring value)
{
    return set_str(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL
Java_org_nopeforge_nopegl_NGLNode_nativeSetUInt(JNIEnv *env, jobject thiz, jlong native_ptr, jstring key, jint value)
{
    return set_u32(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetUVec2(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_uvec2(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetUVec3(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_uvec3(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetUVec4(JNIEnv *env,
                                                                        jobject thiz,
                                                                        jlong native_ptr,
                                                                        jstring key,
                                                                        jintArray value)
{
    return set_uvec4(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetVec2(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jfloatArray value)
{
    return set_vec2(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetVec3(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jfloatArray value)
{
    return set_vec3(env, thiz, native_ptr, key, value);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeSetVec4(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jlong native_ptr,
                                                                       jstring key,
                                                                       jfloatArray value)
{
    return set_vec4(env, thiz, native_ptr, key, value);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeInitFromString(JNIEnv *env,
                                                                                jobject thiz,
                                                                                jstring scene)
{
    const char *scene_str = (*env)->GetStringUTFChars(env, scene, 0);

    struct ngl_scene *s = ngl_scene_create();
    if (!s)
        return (jlong)NULL;

    int ret = ngl_scene_init_from_str(s, scene_str);
    if (ret < 0) {
        ngl_scene_unrefp(&s);
        return (jlong)NULL;
    }

    jclass cls       = (*env)->GetObjectClass(env, thiz);
    jmethodID set_id = (*env)->GetMethodID(env, cls, "setFields", "(DIIII)V");

    const struct ngl_scene_params *p = ngl_scene_get_params(s);
    (*env)->CallVoidMethod(env, thiz, set_id, p->duration, p->framerate[0], p->framerate[1], p->aspect_ratio[0], p->aspect_ratio[1]);

    return (jlong)s;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeAddLiveControls(JNIEnv *env,
                                                                                jobject thiz,
                                                                                jlong native_ptr)
{
    struct ngl_scene *scene = (struct ngl_scene *)(uintptr_t)native_ptr;

    size_t nb_livectls           = 0;
    struct ngl_livectl *livectls = NULL;
    int ret                      = ngl_livectls_get(scene, &nb_livectls, &livectls);
    if (ret < 0)
        return ret;

    jclass cls       = (*env)->GetObjectClass(env, thiz);
    jmethodID add_id = (*env)->GetMethodID(env, cls, "addLiveControl", "(Ljava/lang/String;J)V");

    for (size_t i = 0; i < nb_livectls; i++) {
        const struct ngl_livectl *livectl = &livectls[i];
        jstring id                        = (*env)->NewStringUTF(env, livectl->id);
        jlong ptr                         = (jlong)(uintptr_t)ngl_node_ref(livectl->node);
        (*env)->CallVoidMethod(env, thiz, add_id, id, ptr);
    }

    ngl_livectls_freep(&livectls);

    return 0;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeRelease(JNIEnv *env, jclass clazz, jlong native_ptr)
{
    struct ngl_scene *scene = (struct ngl_scene *)(uintptr_t)native_ptr;
    ngl_scene_unrefp(&scene);
}

JNIEXPORT jstring JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeSerialize(JNIEnv *env,
                                                                             jobject thiz,
                                                                             jlong native_ptr)
{
    struct ngl_scene *scene = (struct ngl_scene *)(uintptr_t)native_ptr;
    char *str               = ngl_scene_serialize(scene);
    return (*env)->NewStringUTF(env, str);
}

JNIEXPORT jstring JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeDot(JNIEnv *env,
                                                                             jobject thiz,
                                                                             jlong native_ptr)
{
    struct ngl_scene *scene = (struct ngl_scene *)(uintptr_t)native_ptr;
    char *str               = ngl_scene_dot(scene);
    return (*env)->NewStringUTF(env, str);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLNode_nativeCreate(JNIEnv *env, jclass clazz, jint type)
{
    return (jlong)(uintptr_t)ngl_node_create(type);
}

JNIEXPORT jstring JNICALL Java_org_nopeforge_nopegl_NGLError_nativeGetMessage(JNIEnv *env, jclass clazz, jint code)
{
    const char *str = ngl_error_to_string(code);
    return (*env)->NewStringUTF(env, str);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLScene_nativeCreateScene(JNIEnv *env,
                                                                             jclass clazz,
                                                                             jlong node_ptr,
                                                                             jdouble duration,
                                                                             jint framerate_num,
                                                                             jint framerate_den,
                                                                             jint aspect_ratio_num,
                                                                             jint aspect_ratio_den)
{
    struct ngl_scene_params params = {
        .root            = (struct ngl_node *)(uintptr_t)node_ptr,
        .duration        = duration,
        .framerate[0]    = framerate_num,
        .framerate[1]    = framerate_den,
        .aspect_ratio[0] = aspect_ratio_num,
        .aspect_ratio[1] = aspect_ratio_den,
    };

    struct ngl_scene *scene = ngl_scene_create();

    if (!scene)
        return (jlong)NULL;

    int ret = ngl_scene_init(scene, &params);

    if (ret < 0) {
        ngl_scene_unrefp(&scene);
        return (jlong)NULL;
    }

    return (jlong)(uintptr_t)scene;
}

struct player {
    struct nmd_ctx *context;
    jobject surface;
};

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopemd_Player_nativeInit(JNIEnv *env,
                                                                    jclass clazz,
                                                                    jstring filename_,
                                                                    jobject surface_)
{
    jobject surface      = (*env)->NewGlobalRef(env, surface_);
    if (surface == NULL) {
        return 0;
    }
    const char *filename = (*env)->GetStringUTFChars(env, filename_, 0);
    if (filename == NULL) {
        return 0;
    }
    struct nmd_ctx *context = nmd_create(filename);
    if (context == NULL) {
        (*env)->ReleaseStringUTFChars(env, filename_, filename);
        return 0;
    }
    struct player *player = calloc(1,sizeof(*player));
    if (player == NULL) {
        nmd_freep(&context);
        (*env)->ReleaseStringUTFChars(env, filename_, filename);
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
    struct nmd_frame *frame;
    int ret = nmd_get_frame(player->context, position, &frame);
    if (ret == NMD_RET_SUCCESS) {
        if (frame->pix_fmt == NMD_PIXFMT_MEDIACODEC) {
            ret = nmd_mc_frame_render_and_releasep(&frame);
        } else {
            __android_log_print(ANDROID_LOG_WARN, "ngl", "Unsupported frame type: %d", frame->pix_fmt);
            nmd_frame_releasep(&frame);
        }
    } else if (ret == NMD_RET_UNCHANGED) {
        ret = NMD_RET_SUCCESS;
    }
    return ret;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopemd_Player_nativeRelease(JNIEnv *env, jclass clazz, jlong ptr) {
    struct player *player = (struct player *)(uintptr_t)ptr;
    nmd_freep(&player->context);
    (*env)->DeleteGlobalRef(env, player->surface);
    free(player);
}

struct custom_texture_ctx {
    struct ngl_node *node;
    jobject object;
    jmethodID init;
    jmethodID prepare;
    jmethodID prefetch;
    jmethodID update;
    jmethodID draw;
    jmethodID release;
    jmethodID uninit;
};

static int custom_texture_init(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->init);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return NGL_ERROR_EXTERNAL;
    }
    return 0;
}

static int custom_texture_prepare(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->prepare);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return NGL_ERROR_EXTERNAL;
    }
    return 0;
}

static int custom_texture_prefetch(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->prefetch);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return NGL_ERROR_EXTERNAL;
    }
    return 0;
}

static int custom_texture_update(void *reversed, void *user_data, double t)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->update, t);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return NGL_ERROR_EXTERNAL;
    }
    return 0;
}

void custom_texture_draw(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->draw);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return;
    }
}

void custom_texture_release(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->release);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return;
    }
}

void custom_texture_uninit(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();
    (*env)->CallVoidMethod(env, ctx->object, ctx->uninit);
    if ((*env)->ExceptionCheck(env)) {
        ((*env)->ExceptionClear(env));
        return;
    }
}

void custom_texture_free(void *reversed, void *user_data)
{
    struct custom_texture_ctx *ctx = user_data;
    JNIEnv *env = ngl_jni_get_env();

    (*env)->DeleteGlobalRef(env, ctx->object);
    ctx->object = NULL;

    free(ctx);
}

static struct ngl_node_funcs custom_texture_funcs = {
    .init = custom_texture_init,
    .prepare = custom_texture_prepare,
    .prefetch = custom_texture_prefetch,
    .update = custom_texture_update,
    .draw = custom_texture_draw,
    .release = custom_texture_release,
    .uninit = custom_texture_uninit,
    .free = custom_texture_free,
};

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLCustomTexture_nativeCreate(JNIEnv *env,
                                                                                jobject object,
                                                                                jlong native_ptr,
                                                                                jobject listener)
{
    struct ngl_node *node = (struct ngl_node *)(uintptr_t)native_ptr;

    struct custom_texture_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return 0;

    // We *must* *not* reference the CustomTexture node here. The memory of the custom_texture_ctx
    // structure is handed to nope.gl and will have the same lifetime as the C node.
    ctx->node = node;

    // On the other hand, we *must* take a reference of the Java Callback object as it will be used
    // during the lifetime of the C node.
    ctx->object = (*env)->NewGlobalRef(env, listener);
    if (!ctx->object)
        goto fail;

    jclass cls = (*env)->GetObjectClass(env, ctx->object);
    ctx->init = (*env)->GetMethodID(env, cls, "init", "()V");
    ctx->prepare = (*env)->GetMethodID(env, cls, "prepare", "()V");
    ctx->prefetch = (*env)->GetMethodID(env, cls, "prefetch", "()V");
    ctx->update = (*env)->GetMethodID(env, cls, "update", "(D)V");
    ctx->draw = (*env)->GetMethodID(env, cls, "draw", "()V");
    ctx->release = (*env)->GetMethodID(env, cls, "release", "()V");
    ctx->uninit = (*env)->GetMethodID(env, cls, "uninit", "()V");

    int ret = ngl_node_set_funcs(node, ctx, &custom_texture_funcs);
    if (ret < 0)
        goto fail;

    return (jlong)(uintptr_t)ctx;

fail:

    (*env)->DeleteGlobalRef(env, ctx->object);
    ctx->object = NULL;

    free(ctx);

    return 0;
}


JNIEXPORT jint JNICALL Java_org_nopeforge_nopegl_NGLCustomTexture_00024Callback_nativeSetTextureInfo(JNIEnv *env,
                                                                                                     jobject object,
                                                                                                     jlong native_context_ptr,
                                                                                                     jint texture,
                                                                                                     jint target,
                                                                                                     jint width,
                                                                                                     jint height)
{
    struct custom_texture_ctx *ctx = (struct custom_texture_ctx *)(uintptr_t)native_context_ptr;

    if (texture) {
        struct ngl_custom_texture_info info = {
                .backend = NGL_BACKEND_OPENGLES,
                .backend_texture_info = &(struct ngl_custom_texture_info_gl) {
                    .texture = texture,
                    .target = target,
                },
                .width = width,
                .height = height,
        };
        return ngl_custom_texture_set_texture_info(ctx->node, &info);
    } else {
        return ngl_custom_texture_set_texture_info(ctx->node, NULL);
    }
}

struct logger_ctx {
    jobject object;
    jmethodID log;
};

__attribute__((__format__(__printf__, 6, 0)))
static void logger_log(void *arg,
                       enum ngl_log_level level,
                       const char *filename,
                       int ln,
                       const char *fn,
                       const char *fmt, va_list vl)
{
    static const int levels[] = {
        [NGL_LOG_VERBOSE] = ANDROID_LOG_VERBOSE,
        [NGL_LOG_DEBUG]   = ANDROID_LOG_DEBUG,
        [NGL_LOG_INFO]    = ANDROID_LOG_INFO,
        [NGL_LOG_WARNING] = ANDROID_LOG_WARN,
        [NGL_LOG_ERROR]   = ANDROID_LOG_ERROR,
    };
    const int mapped            = level >= 0 && level < FF_ARRAY_ELEMS(levels);
    const int android_log_level = mapped ? levels[level] : ANDROID_LOG_VERBOSE;

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);

    char msg[1024];
    snprintf(msg, sizeof(msg), "%s:%d %s: %s", filename, ln, fn, buf);

    struct logger_ctx *ctx = arg;
    JNIEnv *env = ngl_jni_get_env();
    jstring string = (*env)->NewStringUTF(env, msg);
    (*env)->CallVoidMethod(env, ctx->object, ctx->log, android_log_level, string);
    (*env)->DeleteLocalRef(env, string);

}

static struct {
    int level;
    int av_log_level;
    int ngl_log_level;
} logger_levels_map[] = {
    {ANDROID_LOG_DEBUG, AV_LOG_DEBUG,   NGL_LOG_VERBOSE},
    {ANDROID_LOG_DEBUG, AV_LOG_DEBUG,   NGL_LOG_DEBUG  },
    {ANDROID_LOG_INFO,  AV_LOG_INFO,    NGL_LOG_INFO   },
    {ANDROID_LOG_WARN,  AV_LOG_WARNING, NGL_LOG_WARNING},
    {ANDROID_LOG_ERROR, AV_LOG_ERROR,   NGL_LOG_ERROR  },
};

static void logger_init(struct logger_ctx *ctx, int log_level)
{
    int av_log_level  = AV_LOG_INFO;
    int ngl_log_level = NGL_LOG_INFO;
    for (size_t i = 0; i < sizeof(logger_levels_map) / sizeof(*logger_levels_map); i++) {
        if (logger_levels_map[i].level == log_level) {
            av_log_level  = log_levels[i].av_log_level;
            ngl_log_level = log_levels[i].ngl_log_level;
            break;
        }
    }

    av_log_set_level(av_log_level);
    av_log_set_callback(av_android_log);

    ngl_log_set_min_level(ngl_log_level);
    ngl_log_set_callback(ctx, logger_log);
}


JNIEXPORT jlong JNICALL Java_org_nopeforge_nopegl_NGLLogger_nativeInit(JNIEnv *env,
                                                                       jobject thiz,
                                                                       jint level)
{
    struct logger_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return 0;
    ctx->object = (*env)->NewGlobalRef(env, thiz);
    if (!ctx->object) {
        free(ctx);
        return 0;
    }
    jclass cls = (*env)->GetObjectClass(env, ctx->object);
    ctx->log = (*env)->GetMethodID(env, cls, "log", "(ILjava/lang/String;)V");
    logger_init(ctx, level);
    return (jlong)(uintptr_t)ctx;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopegl_NGLLogger_nativeRelease(JNIEnv *env,
                                                                         jobject thiz,
                                                                         jlong native_ptr)
{
    struct logger_ctx *ctx = (struct logger_ctx *)(uintptr_t)native_ptr;
    if (!ctx)
        return;
    (*env)->DeleteGlobalRef(env, ctx->object);
    free(ctx);
}


JNIEXPORT jlong JNICALL
Java_org_nopeforge_nopegl_EGLHelper_nativeCreateEGLImage(JNIEnv *env,
                                                         jobject thiz,
                                                         jobject hardware_buffer)
{
    AHardwareBuffer *buffer = AHardwareBuffer_fromHardwareBuffer(env, hardware_buffer);
    if (!buffer) {
        return 0;
    }

    EGLClientBuffer egl_buffer = eglGetNativeClientBufferANDROID(buffer);
    if (!egl_buffer) {
        return 0;
    }

    static const EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR,
        EGL_TRUE,
        EGL_NONE,
    };

    EGLImage egl_image = eglCreateImageKHR(
        eglGetCurrentDisplay(),
        EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID,
        egl_buffer,
        attrs);
    if (!egl_image) {
        return 0;
    }

    return (jlong)(uintptr_t)egl_image;
}

JNIEXPORT void JNICALL
Java_org_nopeforge_nopegl_EGLHelper_nativeUploadEGLImage(JNIEnv *env,
                                                         jobject thiz,
                                                         jlong egl_image,
                                                         jint texture)
{
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (EGLImageKHR)(uintptr_t)egl_image);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
}

JNIEXPORT void JNICALL
Java_org_nopeforge_nopegl_EGLHelper_nativeReleaseEGLImage(JNIEnv *env,
                                                          jobject thiz,
                                                          jlong image) {
    EGLImageKHR egl_image = (EGLImageKHR)(uintptr_t)image;
    eglDestroyImageKHR(eglGetCurrentDisplay(), egl_image);
}
