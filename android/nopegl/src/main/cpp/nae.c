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

#include <nae/miniaudio.h>
#include <nae/player.h>

#define PLAYER(ptr) ((struct nae_player *)(uintptr_t)(ptr))
#define TRACK(ptr) ((struct nae_track *)(uintptr_t)(ptr))

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeCreate(JNIEnv *env, jclass clazz,
                                                                        jint mode, jint sample_rate,
                                                                        jint period_ms)
{
    const struct nae_player_config config = {
        .mode = (enum nae_player_mode)mode,
        .sample_rate = (uint32_t)sample_rate,
        .period_ms = (uint32_t)period_ms,
    };
    struct nae_player *player = NULL;
    if (nae_player_create(&config, &player) < 0)
        return 0;
    return (jlong)(uintptr_t)player;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeRelease(JNIEnv *env, jclass clazz, jlong ptr)
{
    struct nae_player *player = PLAYER(ptr);
    nae_player_freep(&player);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetSampleRate(JNIEnv *env, jclass clazz, jlong ptr)
{
    return (jint)nae_player_get_sample_rate(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeAddTrack(JNIEnv *env, jclass clazz, jlong ptr,
                                                                          jstring path, jlong start, jlong trim,
                                                                          jlong duration, jfloat volume,
                                                                          jboolean normalize,
                                                                          jlongArray output)
{
    if ((*env)->GetArrayLength(env, output) != 1)
        return NAE_ERROR_INVALID;
    const jlong empty = 0;
    (*env)->SetLongArrayRegion(env, output, 0, 1, &empty);
    if ((*env)->ExceptionCheck(env))
        return NAE_ERROR_MEMORY;
    const char *filename = (*env)->GetStringUTFChars(env, path, 0);
    if (!filename)
        return NAE_ERROR_MEMORY;
    const struct nae_track_params params = {
        .filename = filename,
        .start = start,
        .trim = trim,
        .duration = duration,
        .volume = volume,
        .normalize = normalize,
    };
    struct nae_track *track = NULL;
    const int ret = nae_player_add_track(PLAYER(ptr), &params, &track);
    (*env)->ReleaseStringUTFChars(env, path, filename);
    if (ret == NAE_OK) {
        const jlong pointer = (jlong)(uintptr_t)track;
        (*env)->SetLongArrayRegion(env, output, 0, 1, &pointer);
    }
    return ret;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetTrackTimings(JNIEnv *env, jclass clazz,
                                                                                jlong ptr, jlong track,
                                                                                jlong start, jlong trim,
                                                                                jlong duration)
{
    return nae_player_set_track_timings(PLAYER(ptr), TRACK(track), start, trim, duration);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetTrackVolume(JNIEnv *env, jclass clazz,
                                                                               jlong ptr, jlong track,
                                                                               jfloat volume)
{
    return nae_player_set_track_volume(PLAYER(ptr), TRACK(track), volume);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetTrackFader(JNIEnv *env, jclass clazz,
                                                                              jlong ptr, jlong track,
                                                                              jlongArray frames,
                                                                              jlongArray lengths,
                                                                              jfloatArray volumes)
{
    const jsize count = (*env)->GetArrayLength(env, frames);
    if (count > 65536 || (*env)->GetArrayLength(env, lengths) != count || (*env)->GetArrayLength(env, volumes) != count)
        return NAE_ERROR_INVALID;
    struct nae_keyframe *keyframes = calloc((size_t)count + 1, sizeof(*keyframes));
    if (!keyframes)
        return NAE_ERROR_MEMORY;
    jlong *f = (*env)->GetLongArrayElements(env, frames, NULL);
    jlong *l = (*env)->GetLongArrayElements(env, lengths, NULL);
    jfloat *v = (*env)->GetFloatArrayElements(env, volumes, NULL);
    int ret = NAE_ERROR_MEMORY;
    if (f && l && v) {
        for (jsize i = 0; i < count; i++)
            keyframes[i] = (struct nae_keyframe){.frame = f[i], .length = l[i], .volume = v[i]};
        ret = nae_player_set_track_fader(PLAYER(ptr), TRACK(track), keyframes, (uint32_t)count);
    }
    if (f)
        (*env)->ReleaseLongArrayElements(env, frames, f, JNI_ABORT);
    if (l)
        (*env)->ReleaseLongArrayElements(env, lengths, l, JNI_ABORT);
    if (v)
        (*env)->ReleaseFloatArrayElements(env, volumes, v, JNI_ABORT);
    free(keyframes);
    return ret;
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetTrackLength(JNIEnv *env, jclass clazz,
                                                                                jlong track)
{
    return nae_track_get_length(TRACK(track));
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetTrackDuration(JNIEnv *env, jclass clazz,
                                                                                  jlong track)
{
    return nae_track_get_duration(TRACK(track));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeRemoveTrack(JNIEnv *env, jclass clazz, jlong ptr,
                                                                            jlong track)
{
    struct nae_track *t = TRACK(track);
    return nae_player_remove_track(PLAYER(ptr), &t);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeClearTracks(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_clear_tracks(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativePlay(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_play(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativePause(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_pause(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeStop(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_stop(PLAYER(ptr));
}

JNIEXPORT jboolean JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeIsPlaying(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_is_playing(PLAYER(ptr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSeek(JNIEnv *env, jclass clazz, jlong ptr,
                                                                     jlong frame)
{
    return nae_player_seek(PLAYER(ptr), frame);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetPosition(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_get_position(PLAYER(ptr));
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetPositionAt(JNIEnv *env, jclass clazz,
                                                                               jlong ptr, jlong time)
{
    int64_t position;
    return nae_player_get_position_at(PLAYER(ptr), time, &position) == NAE_OK ? position : -1;
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeNow(JNIEnv *env, jclass clazz)
{
    return nae_sink_now();
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetDuration(JNIEnv *env, jclass clazz, jlong ptr,
                                                                            jlong frames)
{
    return nae_player_set_duration(PLAYER(ptr), frames);
}

JNIEXPORT jlong JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetDuration(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_get_duration(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetLooping(JNIEnv *env, jclass clazz, jlong ptr,
                                                                           jboolean looping)
{
    return nae_player_set_looping(PLAYER(ptr), looping);
}

JNIEXPORT jboolean JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeIsLooping(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_is_looping(PLAYER(ptr)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetLoopRange(JNIEnv *env, jclass clazz, jlong ptr,
                                                                             jlong start, jlong end)
{
    return nae_player_set_loop_range(PLAYER(ptr), start, end);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetLoopHold(JNIEnv *env, jclass clazz, jlong ptr,
                                                                            jlong frames)
{
    return nae_player_set_loop_hold(PLAYER(ptr), frames);
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeSetVolume(JNIEnv *env, jclass clazz, jlong ptr,
                                                                          jfloat volume)
{
    return nae_player_set_volume(PLAYER(ptr), volume);
}

JNIEXPORT jfloat JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetVolume(JNIEnv *env, jclass clazz, jlong ptr)
{
    return nae_player_get_volume(PLAYER(ptr));
}

JNIEXPORT jint JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeRecord(JNIEnv *env, jclass clazz, jlong ptr,
                                                                       jstring path, jint format)
{
    const char *filename = (*env)->GetStringUTFChars(env, path, 0);
    if (!filename)
        return NAE_ERROR_MEMORY;
    const int ret = nae_player_record(PLAYER(ptr), filename, (enum nae_record_format)format);
    (*env)->ReleaseStringUTFChars(env, path, filename);
    return ret;
}

JNIEXPORT void JNICALL Java_org_nopeforge_nae_AudioPlayer_nativeGetStats(JNIEnv *env, jclass clazz, jlong ptr,
                                                                         jlongArray values)
{
    struct nae_player_stats stats;
    nae_player_get_stats(PLAYER(ptr), &stats);
    const jlong array[] = {stats.underruns, stats.silent_frames, stats.skipped_frames, stats.requests, stats.errors};
    (*env)->SetLongArrayRegion(env, values, 0, 5, array);
}
