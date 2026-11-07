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

package org.nopeforge.nae

import java.io.File
import kotlin.time.Duration
import kotlin.time.Duration.Companion.microseconds

/**
 * A player of tracks of media files laid on a project, as an editor plays
 * the audio of its timeline (nae's player, see nae/player.h).
 *
 * Each track plays a part of its media from a time of the project, through
 * its fader and volume, mixed under a master volume. Tracks can be added,
 * moved, faded and removed while playing, and the playback can loop over a
 * range with a hold at its end. None of the functions waits for the audio
 * thread. NGLContext.init() must have been called, for nope.media to open
 * content URIs.
 */
class AudioPlayer(
    mode: Mode = Mode.Playback,
    sampleRate: Int = 0,
    periodMs: Int = 0,
) : AutoCloseable {

    enum class Mode(internal val nativeId: Int) {
        /** Plays on the default audio output */
        Playback(0),

        /** Plays on a headless output, in real time */
        Null(1),

        /** Renders as fast as it can to a file, see [record] */
        Offline(3),
    }

    enum class RecordFormat(internal val nativeId: Int) {
        S16(0),
        F32(1),
    }

    /** A track of the player, until removed */
    class Track internal constructor(internal val pointer: Long)

    /** A point of the volume automation of a track: from [start], the volume goes linearly to [volume] over [length] */
    data class Keyframe(val start: Duration, val volume: Float, val length: Duration = Duration.ZERO)

    data class Stats(
        val underruns: Long,
        val silentFrames: Long,
        val skippedFrames: Long,
        val requests: Long,
        val errors: Long,
    )

    private var nativePtr: Long = nativeCreate(mode.nativeId, sampleRate, periodMs)

    init {
        if (nativePtr == 0L) throw IllegalStateException("Failed to create the audio player")
    }

    val sampleRate: Int = nativeGetSampleRate(nativePtr)

    private fun ptr(): Long = nativePtr.also { check(it != 0L) { "The audio player is released" } }

    private fun Duration.toFrames(): Long = inWholeMicroseconds * sampleRate / 1_000_000

    private fun Long.toDuration(): Duration = (this * 1_000_000 / sampleRate).microseconds

    /**
     * Lays a track of the media at [path] from [start] of the project, playing it from [trim] for
     * [duration] (the rest of the media if null). Null if the media could not be opened.
     */
    fun addTrack(
        path: String,
        start: Duration = Duration.ZERO,
        trim: Duration = Duration.ZERO,
        duration: Duration? = null,
        volume: Float = 1f,
        normalize: Boolean = false,
    ): Track? {
        val track = nativeAddTrack(
            ptr(), path, start.toFrames(), trim.toFrames(), duration?.toFrames() ?: -1, volume, normalize,
        )
        return if (track == 0L) null else Track(track)
    }

    fun setTrackTimings(track: Track, start: Duration, trim: Duration, duration: Duration?): Int =
        nativeSetTrackTimings(ptr(), track.pointer, start.toFrames(), trim.toFrames(), duration?.toFrames() ?: -1)

    fun setTrackVolume(track: Track, volume: Float): Int = nativeSetTrackVolume(ptr(), track.pointer, volume)

    /** Keyframes from the start of the track */
    fun setTrackFader(track: Track, keyframes: List<Keyframe>): Int = nativeSetTrackFader(
        ptr(),
        track.pointer,
        keyframes.map { it.start.toFrames() }.toLongArray(),
        keyframes.map { it.length.toFrames() }.toLongArray(),
        keyframes.map { it.volume }.toFloatArray(),
    )

    /** The length of the media of a track, zero if unknown */
    fun getTrackLength(track: Track): Duration = nativeGetTrackLength(track.pointer).toDuration()

    /** The time a track plays */
    fun getTrackDuration(track: Track): Duration = nativeGetTrackDuration(track.pointer).toDuration()

    fun removeTrack(track: Track): Int = nativeRemoveTrack(ptr(), track.pointer)

    fun clearTracks(): Int = nativeClearTracks(ptr())

    fun play(): Int = nativePlay(ptr())

    /** The output runs on, silent: playing again restarts nothing, and seeks reach the tracks at once */
    fun pause(): Int = nativePause(ptr())

    /** Pauses, back to the start, the audio output stopped */
    fun stop(): Int = nativeStop(ptr())

    val isPlaying: Boolean
        get() = nativeIsPlaying(ptr())

    fun seek(position: Duration): Int = nativeSeek(ptr(), position.toFrames().coerceAtLeast(0))

    /** The time rendered next, or of a seek not applied yet */
    val position: Duration
        get() = nativeGetPosition(ptr()).toDuration()

    /**
     * The time of the project heard at [nanoTime], in the time base of System.nanoTime(), as for a
     * Choreographer frame: the latency of the output accounted for, and the jitter of its callbacks
     * smoothed out. The position while stopped, or null if the clock history is unavailable.
     */
    fun getPositionAt(nanoTime: Long): Duration? =
        nativeGetPositionAt(ptr(), nanoTime).takeIf { it >= 0 }?.toDuration()

    var duration: Duration
        get() = nativeGetDuration(ptr()).toDuration()
        set(value) {
            nativeSetDuration(ptr(), value.toFrames().coerceAtLeast(0))
        }

    var isLooping: Boolean
        get() = nativeIsLooping(ptr())
        set(value) {
            nativeSetLooping(ptr(), value)
        }

    /** The range looped over, [start, end); an empty range loops over the whole duration */
    fun setLoopRange(start: Duration, end: Duration): Int =
        nativeSetLoopRange(ptr(), start.toFrames().coerceAtLeast(0), end.toFrames().coerceAtLeast(0))

    /** Silence at the end of each pass of the loop, the position staying at its end */
    fun setLoopHold(hold: Duration): Int = nativeSetLoopHold(ptr(), hold.toFrames().coerceAtLeast(0))

    var volume: Float
        get() = nativeGetVolume(ptr())
        set(value) {
            nativeSetVolume(ptr(), value)
        }

    /** [Mode.Offline]: writes the project, from its start to its duration, to a WAV file */
    fun record(file: File, format: RecordFormat = RecordFormat.S16): Int =
        nativeRecord(ptr(), file.absolutePath, format.nativeId)

    val stats: Stats
        get() {
            val values = LongArray(5)
            nativeGetStats(ptr(), values)
            return Stats(values[0], values[1], values[2], values[3], values[4])
        }

    fun release() {
        if (nativePtr != 0L) {
            nativeRelease(nativePtr)
            nativePtr = 0L
        }
    }

    override fun close() = release()

    companion object {
        /** The time base of [getPositionAt], that of System.nanoTime() */
        @JvmStatic
        fun now(): Long = nativeNow()

        init {
            System.loadLibrary("lcms2")
            System.loadLibrary("avutil")
            System.loadLibrary("avcodec")
            System.loadLibrary("avformat")
            System.loadLibrary("avdevice")
            System.loadLibrary("swresample")
            System.loadLibrary("swscale")
            System.loadLibrary("avfilter")
            System.loadLibrary("fribidi")
            System.loadLibrary("harfbuzz")
            System.loadLibrary("nopemd")
            System.loadLibrary("ngpu")
            System.loadLibrary("nopegl")
            System.loadLibrary("nae")
            System.loadLibrary("nae-nopemd")
            System.loadLibrary("nae-miniaudio")
            System.loadLibrary("nae-player")
            System.loadLibrary("nopegl_native")
        }

        @JvmStatic private external fun nativeCreate(mode: Int, sampleRate: Int, periodMs: Int): Long
        @JvmStatic private external fun nativeRelease(ptr: Long)
        @JvmStatic private external fun nativeGetSampleRate(ptr: Long): Int
        @JvmStatic private external fun nativeAddTrack(
            ptr: Long, path: String, start: Long, trim: Long, duration: Long, volume: Float, normalize: Boolean,
        ): Long
        @JvmStatic private external fun nativeSetTrackTimings(ptr: Long, track: Long, start: Long, trim: Long, duration: Long): Int
        @JvmStatic private external fun nativeSetTrackVolume(ptr: Long, track: Long, volume: Float): Int
        @JvmStatic private external fun nativeSetTrackFader(
            ptr: Long, track: Long, frames: LongArray, lengths: LongArray, volumes: FloatArray,
        ): Int
        @JvmStatic private external fun nativeGetTrackLength(track: Long): Long
        @JvmStatic private external fun nativeGetTrackDuration(track: Long): Long
        @JvmStatic private external fun nativeRemoveTrack(ptr: Long, track: Long): Int
        @JvmStatic private external fun nativeClearTracks(ptr: Long): Int
        @JvmStatic private external fun nativePlay(ptr: Long): Int
        @JvmStatic private external fun nativePause(ptr: Long): Int
        @JvmStatic private external fun nativeStop(ptr: Long): Int
        @JvmStatic private external fun nativeIsPlaying(ptr: Long): Boolean
        @JvmStatic private external fun nativeSeek(ptr: Long, frame: Long): Int
        @JvmStatic private external fun nativeGetPosition(ptr: Long): Long
        @JvmStatic private external fun nativeGetPositionAt(ptr: Long, time: Long): Long
        @JvmStatic private external fun nativeNow(): Long
        @JvmStatic private external fun nativeSetDuration(ptr: Long, frames: Long): Int
        @JvmStatic private external fun nativeGetDuration(ptr: Long): Long
        @JvmStatic private external fun nativeSetLooping(ptr: Long, looping: Boolean): Int
        @JvmStatic private external fun nativeIsLooping(ptr: Long): Boolean
        @JvmStatic private external fun nativeSetLoopRange(ptr: Long, start: Long, end: Long): Int
        @JvmStatic private external fun nativeSetLoopHold(ptr: Long, frames: Long): Int
        @JvmStatic private external fun nativeSetVolume(ptr: Long, volume: Float): Int
        @JvmStatic private external fun nativeGetVolume(ptr: Long): Float
        @JvmStatic private external fun nativeRecord(ptr: Long, path: String, format: Int): Int
        @JvmStatic private external fun nativeGetStats(ptr: Long, values: LongArray)
    }
}
