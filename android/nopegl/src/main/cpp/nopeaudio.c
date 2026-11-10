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

#include <stdint.h>
#include <stdlib.h>

#include <jni.h>

#include <nopeaudio/nopeaudio.h>

#define PLAYER(ptr) ((struct nau_player *)(uintptr_t)(ptr))
#define TRACK(ptr) ((struct nau_track *)(uintptr_t)(ptr))
#define EFFECT(ptr) ((struct nau_track_effect *)(uintptr_t)(ptr))

/*
 * Convert parallel field arrays of equal length to at most max ramps.
 * The caller owns the array returned in *ramps.
 */
static int read_ramps(JNIEnv *env, jlongArray frames, jlongArray lengths, jfloatArray values, jintArray shapes,
                      jfloatArray curvatures, size_t max, struct nau_ramp **ramps, uint32_t *count)
{
    *ramps = NULL;
    *count = 0;
    const jsize n = (*env)->GetArrayLength(env, frames);
    if ((size_t)n > max || (*env)->GetArrayLength(env, lengths) != n || (*env)->GetArrayLength(env, values) != n ||
        (*env)->GetArrayLength(env, shapes) != n || (*env)->GetArrayLength(env, curvatures) != n)
        return NAU_ERROR_INVALID;
    struct nau_ramp *items = calloc((size_t)n + 1, sizeof(*items));
    if (!items)
        return NAU_ERROR_MEMORY;
    /*
     * A failed array access may leave a JNI exception pending. Release
     * the arrays already acquired and return without acquiring more.
     */
    jlong *f = NULL, *l = NULL;
    jfloat *v = NULL, *cu = NULL;
    jint *sh = NULL;
    int ret = NAU_ERROR_MEMORY;
    f = (*env)->GetLongArrayElements(env, frames, NULL);
    if (!f)
        goto end;
    l = (*env)->GetLongArrayElements(env, lengths, NULL);
    if (!l)
        goto end;
    v = (*env)->GetFloatArrayElements(env, values, NULL);
    if (!v)
        goto end;
    sh = (*env)->GetIntArrayElements(env, shapes, NULL);
    if (!sh)
        goto end;
    cu = (*env)->GetFloatArrayElements(env, curvatures, NULL);
    if (!cu)
        goto end;
    /* The library validates the values */
    for (jsize i = 0; i < n; i++) {
        items[i] = (struct nau_ramp){
            .frame     = f[i],
            .length    = l[i],
            .value     = v[i],
            .shape     = (enum nau_ramp_shape)sh[i],
            .curvature = cu[i],
        };
    }
    ret = NAU_OK;
end:
    if (f)
        (*env)->ReleaseLongArrayElements(env, frames, f, JNI_ABORT);
    if (l)
        (*env)->ReleaseLongArrayElements(env, lengths, l, JNI_ABORT);
    if (v)
        (*env)->ReleaseFloatArrayElements(env, values, v, JNI_ABORT);
    if (sh)
        (*env)->ReleaseIntArrayElements(env, shapes, sh, JNI_ABORT);
    if (cu)
        (*env)->ReleaseFloatArrayElements(env, curvatures, cu, JNI_ABORT);
    if (ret < 0) {
        free(items);
        return ret;
    }
    *ramps = items;
    *count = (uint32_t)n;
    return NAU_OK;
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeCreate(JNIEnv *env, jclass clazz,
                                                                              jint mode, jint sample_rate,
                                                                              jint period_ms)
{
    const struct nau_player_config config = {
        .mode = (enum nau_player_mode)mode,
        .sample_rate = (uint32_t)sample_rate,
        .period_ms = (uint32_t)period_ms,
    };
    struct nau_player *player = nau_player_create();
    if (!player)
        return 0;
    int ret = nau_player_init(player, &config);
    if (ret < 0) {
        nau_player_freep(&player);
        return 0;
    }
    return (jlong)(uintptr_t)player;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeRelease(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct nau_player *player = PLAYER(ptr);
    nau_player_freep(&player);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetSampleRate(JNIEnv *env, jclass clazz, jlong ptr)
{
    return (jint)nau_player_get_sample_rate(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeAddTrack(JNIEnv *env, jclass clazz, jlong ptr,
                                                                               jstring path, jlong start, jlong trim,
                                                                               jlong duration, jfloat volume,
                                                                               jboolean normalize,
                                                                               jlongArray output)
{
    if ((*env)->GetArrayLength(env, output) != 1)
        return NAU_ERROR_INVALID;
    const jlong empty = 0;
    (*env)->SetLongArrayRegion(env, output, 0, 1, &empty);
    if ((*env)->ExceptionCheck(env))
        return NAU_ERROR_MEMORY;
    const char *filename = (*env)->GetStringUTFChars(env, path, 0);
    if (!filename)
        return NAU_ERROR_MEMORY;
    const struct nau_track_params params = {
        .filename = filename,
        .start = start,
        .trim = trim,
        .duration = duration,
        .volume = volume,
        .normalize = normalize,
    };
    struct nau_track *track = NULL;
    int ret = nau_player_add_track(PLAYER(ptr), &params, &track);
    (*env)->ReleaseStringUTFChars(env, path, filename);
    if (ret == NAU_OK) {
        const jlong pointer = (jlong)(uintptr_t)track;
        (*env)->SetLongArrayRegion(env, output, 0, 1, &pointer);
    }
    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetTrackTimings(JNIEnv *env, jclass clazz,
                                                                                      jlong ptr, jlong track,
                                                                                      jlong start, jlong trim,
                                                                                      jlong duration)
{
    return nau_player_set_track_timings(PLAYER(ptr), TRACK(track), start, trim, duration);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetTrackVolume(JNIEnv *env, jclass clazz,
                                                                                     jlong ptr, jlong track,
                                                                                     jfloat volume)
{
    return nau_player_set_track_volume(PLAYER(ptr), TRACK(track), volume);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetTrackFader(JNIEnv *env, jclass clazz,
                                                                                    jlong ptr, jlong track,
                                                                                    jlongArray frames,
                                                                                    jlongArray lengths,
                                                                                    jfloatArray values,
                                                                                    jintArray shapes,
                                                                                    jfloatArray curvatures)
{
    struct nau_ramp *ramps = NULL;
    uint32_t count = 0;
    int ret = read_ramps(env, frames, lengths, values, shapes, curvatures, NAU_MAX_RAMPS, &ramps, &count);
    if (ret == NAU_OK)
        ret = nau_player_set_track_fader(PLAYER(ptr), TRACK(track), ramps, count);
    free(ramps);
    return ret;
}

/*
 * The name of an effect, NULL past the last one; `counts` gets its number of
 * parameters
 */
JNIEXPORT jstring JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetEffect(JNIEnv *env, jclass clazz,
                                                                                   jint type, jintArray counts)
{
    const struct nau_effect_info *info = nau_effect_get_info((enum nau_effect_type)type);
    if (!info || (*env)->GetArrayLength(env, counts) != 1)
        return NULL;
    const jint values[] = {(jint)info->param_count};
    (*env)->SetIntArrayRegion(env, counts, 0, 1, values);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    return (*env)->NewStringUTF(env, info->name);
}

/*
 * The name of a parameter of an effect, NULL if there is none; `values` gets
 * its minimum, maximum, default value and scale
 */
JNIEXPORT jstring JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetParam(JNIEnv *env, jclass clazz,
                                                                                  jint type, jint param,
                                                                                  jfloatArray values)
{
    const struct nau_effect_info *info = nau_effect_get_info((enum nau_effect_type)type);
    if (!info || param < 0 || (uint32_t)param >= info->param_count || (*env)->GetArrayLength(env, values) != 4)
        return NULL;
    const struct nau_param_info *p = &info->params[param];
    const jfloat array[] = {p->min, p->max, p->default_value, (jfloat)p->scale};
    (*env)->SetFloatArrayRegion(env, values, 0, 4, array);
    if ((*env)->ExceptionCheck(env))
        return NULL;
    return (*env)->NewStringUTF(env, p->name);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeAddEffect(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                jlong track, jint type,
                                                                                jint placement, jlongArray output)
{
    if ((*env)->GetArrayLength(env, output) != 1)
        return NAU_ERROR_INVALID;
    const jlong empty = 0;
    (*env)->SetLongArrayRegion(env, output, 0, 1, &empty);
    if ((*env)->ExceptionCheck(env))
        return NAU_ERROR_MEMORY;
    struct nau_track_effect *effect = NULL;
    int ret = nau_player_add_track_effect(PLAYER(ptr), TRACK(track), (enum nau_effect_type)type,
                                          (enum nau_track_effect_placement)placement, &effect);
    if (ret == NAU_OK) {
        const jlong pointer = (jlong)(uintptr_t)effect;
        (*env)->SetLongArrayRegion(env, output, 0, 1, &pointer);
    }
    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeMoveEffect(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                 jlong effect, jint placement,
                                                                                 jint index)
{
    return nau_player_move_track_effect(PLAYER(ptr), EFFECT(effect), (enum nau_track_effect_placement)placement,
                                        (uint32_t)index);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeRemoveEffect(JNIEnv *env, jclass clazz,
                                                                                   jlong ptr, jlong effect)
{
    struct nau_track_effect *e = EFFECT(effect);
    return nau_player_remove_track_effect(PLAYER(ptr), &e);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetEffectParam(JNIEnv *env, jclass clazz,
                                                                                     jlong ptr, jlong effect,
                                                                                     jint param, jfloat value)
{
    return nau_player_set_track_effect_param(PLAYER(ptr), EFFECT(effect), (uint32_t)param, value);
}

/*
 * Each lane consumes counts[i] consecutive ramps from the flattened arrays
 * and applies them to params[i].
 */
JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetEffectAutomation(JNIEnv *env, jclass clazz,
                                                                                          jlong ptr, jlong effect,
                                                                                          jintArray params,
                                                                                          jintArray counts,
                                                                                          jlongArray frames,
                                                                                          jlongArray lengths,
                                                                                          jfloatArray values,
                                                                                          jintArray shapes,
                                                                                          jfloatArray curvatures)
{
    const jsize lane_count = (*env)->GetArrayLength(env, params);
    if ((*env)->GetArrayLength(env, counts) != lane_count)
        return NAU_ERROR_INVALID;
    struct nau_lane *lanes = calloc((size_t)lane_count + 1, sizeof(*lanes));
    if (!lanes)
        return NAU_ERROR_MEMORY;
    struct nau_ramp *ramps = NULL;
    uint32_t ramp_count = 0, offset = 0;
    jint *p = NULL, *c = NULL;
    int ret = NAU_ERROR_MEMORY;
    p = (*env)->GetIntArrayElements(env, params, NULL);
    if (!p)
        goto end;
    c = (*env)->GetIntArrayElements(env, counts, NULL);
    if (!c)
        goto end;
    ret = read_ramps(env, frames, lengths, values, shapes, curvatures, NAU_MAX_RAMPS * (size_t)lane_count, &ramps,
                     &ramp_count);
    if (ret < 0)
        goto end;
    for (jsize i = 0; i < lane_count; i++) {
        if (c[i] < 0 || (uint32_t)c[i] > ramp_count - offset) {
            ret = NAU_ERROR_INVALID;
            goto end;
        }
        lanes[i] = (struct nau_lane){(uint32_t)p[i], ramps + offset, (uint32_t)c[i]};
        offset += (uint32_t)c[i];
    }
    if (offset != ramp_count) {
        ret = NAU_ERROR_INVALID;
        goto end;
    }
    ret = nau_player_set_track_effect_automation(PLAYER(ptr), EFFECT(effect), lanes, (uint32_t)lane_count);
end:
    if (p)
        (*env)->ReleaseIntArrayElements(env, params, p, JNI_ABORT);
    if (c)
        (*env)->ReleaseIntArrayElements(env, counts, c, JNI_ABORT);
    free(ramps);
    free(lanes);
    return ret;
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetTrackLength(JNIEnv *env, jclass clazz,
                                                                                      jlong track)
{
    return nau_track_get_length(TRACK(track));
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetTrackDuration(JNIEnv *env, jclass clazz,
                                                                                        jlong track)
{
    return nau_track_get_duration(TRACK(track));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeRemoveTrack(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                  jlong track)
{
    struct nau_track *t = TRACK(track);
    return nau_player_remove_track(PLAYER(ptr), &t);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeClearTracks(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_clear_tracks(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativePlay(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_play(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativePause(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_pause(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeStop(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_stop(PLAYER(ptr));
}

JNIEXPORT jboolean JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeIsPlaying(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_is_playing(PLAYER(ptr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSeek(JNIEnv *env, jclass clazz, jlong ptr,
                                                                           jlong frame)
{
    return nau_player_seek(PLAYER(ptr), frame);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetPosition(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_get_position(PLAYER(ptr));
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetPositionAt(JNIEnv *env, jclass clazz,
                                                                                     jlong ptr, jlong time)
{
    int64_t position;
    int ret = nau_player_get_position_at(PLAYER(ptr), time, &position);
    return ret == NAU_OK ? position : -1;
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeNow(JNIEnv *env, jclass clazz)
{
    return nau_sink_now();
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetDuration(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                  jlong frames)
{
    return nau_player_set_duration(PLAYER(ptr), frames);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetDuration(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_get_duration(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetLooping(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                 jboolean looping)
{
    return nau_player_set_looping(PLAYER(ptr), looping);
}

JNIEXPORT jboolean JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeIsLooping(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_is_looping(PLAYER(ptr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetLoopRange(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                   jlong start, jlong end)
{
    return nau_player_set_loop_range(PLAYER(ptr), start, end);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetLoopHold(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                  jlong frames)
{
    return nau_player_set_loop_hold(PLAYER(ptr), frames);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeSetVolume(JNIEnv *env, jclass clazz, jlong ptr,
                                                                                jfloat volume)
{
    return nau_player_set_volume(PLAYER(ptr), volume);
}

JNIEXPORT jfloat JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetVolume(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nau_player_get_volume(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeExport(JNIEnv *env, jclass clazz, jlong ptr,
                                                                             jstring path, jint format)
{
    const char *filename = (*env)->GetStringUTFChars(env, path, 0);
    if (!filename)
        return NAU_ERROR_MEMORY;
    int ret = nau_player_export(PLAYER(ptr), filename, (enum nau_export_format)format);
    (*env)->ReleaseStringUTFChars(env, path, filename);
    return ret;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nopeaudio_AudioPlayer_nativeGetStats(JNIEnv *env, jclass clazz, jlong ptr,
                                                                               jlongArray values)
{
    struct nau_player_stats stats;
    nau_player_get_stats(PLAYER(ptr), &stats);
    const jlong array[] = {stats.underruns, stats.silent_frames, stats.skipped_frames, stats.requests, stats.errors};
    (*env)->SetLongArrayRegion(env, values, 0, 5, array);
}
