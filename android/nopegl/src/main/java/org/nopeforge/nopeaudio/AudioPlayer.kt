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

package org.nopeforge.nopeaudio

import org.nopeforge.Nope
import java.io.File
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write
import kotlin.time.Duration
import kotlin.time.Duration.Companion.nanoseconds
import kotlin.time.Duration.Companion.seconds

/**
 * Play media tracks on a timeline. Calls are serialized on this instance;
 * opening media, exporting and stopping the output may block.
 *
 * [isPlaying], [position], [getPositionAt] and [stats] can be read from a video
 * rendering thread without waiting for edits or media to open.
 *
 * Track edits return [AGAIN] while the previous edit is pending; retry later.
 * Call Nope.init(context) before using content URIs. Release the player with
 * [close] when it is no longer needed.
 */
class AudioPlayer(
    mode: Mode = Mode.Device,
    sampleRate: Int = 0,
    periodMs: Int = 0,
) : AutoCloseable {

    enum class Mode(internal val nativeId: Int) {
        /** Play on the default audio output */
        Device(0),

        /** Render in real time without audio output */
        NullDevice(1),

        /** Render to a file as fast as possible, see [export] */
        Offline(3),
    }

    enum class ExportFormat(internal val nativeId: Int) {
        S16(0),
        F32(1),
    }

    /** A track of the player, until removed */
    class Track internal constructor(internal val owner: AudioPlayer, internal var pointer: Long) {
        internal val effects = mutableSetOf<Effect>()
    }

    data class TrackResult(val status: Int, val track: Track?)

    /** Interpolation curve of a ramp */
    enum class RampShape(internal val nativeId: Int) {
        /** Linearly: in amplitude for a volume, along the scale of a parameter of an effect */
        Linear(0),

        /**
         * Linear in dB. Wider spans and fades to or from silence use a 60 dB curve with the
         * same endpoints. Equivalent to [Linear] for logarithmic parameters.
         */
        Db(1),

        /** Sine/cosine fades for a constant-power crossfade of uncorrelated signals */
        EqualPower(2),

        /** Easing in and out */
        SCurve(3),
    }

    /**
     * Automation ramp for a track's fader or effect parameter. At [start], relative to the
     * track's start, interpolate from the previous target to [value] over [length], then hold
     * that value. A zero length makes a step. The first ramp starts from 1 for a fader or the
     * current value for an effect parameter.
     *
     * Ramps are ordered by [start]. A new ramp takes over as soon as it starts, so overlapping
     * ramps can introduce a discontinuity.
     *
     * [curvature] is within [-1, 1]: positive values approach the target earlier, negative
     * values later. Zero leaves [shape] unchanged.
     */
    data class Ramp(
        val start: Duration,
        val value: Float,
        val length: Duration = Duration.ZERO,
        val shape: RampShape = RampShape.Linear,
        val curvature: Float = 0f,
    )

    /** Interpolation scale of an effect parameter */
    enum class ParamScale {
        /** Linearly */
        Linear,

        /** By constant ratios, as a frequency by octaves */
        Log,

        /** By integer steps (a choice), never in between */
        Stepped,
    }

    /** Description of an effect parameter */
    class Param internal constructor(
        internal val id: Int,
        val name: String,
        val min: Float,
        val max: Float,
        val defaultValue: Float,
        val scale: ParamScale,
    ) {
        override fun toString() = name
    }

    /** Description of an effect type, see [effectTypes] */
    class EffectType internal constructor(
        internal val id: Int,
        val name: String,
        val params: List<Param>,
        /** Whether the effect listens to other tracks, see [setEffectSidechain] */
        val sidechain: Boolean,
    ) {
        /** The parameter named [name] */
        fun param(name: String): Param =
            params.find { it.name == name } ?: throw IllegalArgumentException("$this has no parameter $name")

        override fun toString() = name
    }

    /** Placement relative to the track's volume automation and adjustable gain. */
    enum class Placement(internal val nativeId: Int) {
        /** Before both volume controls */
        PreFader(0),

        /** After both volume controls */
        PostFader(1),
    }

    /** Effect handle, valid until the effect or its track is removed */
    class Effect internal constructor(val track: Track, val type: EffectType, internal var pointer: Long)

    data class EffectResult(val status: Int, val effect: Effect?)

    data class Stats(
        val underruns: Long,
        val silentFrames: Long,
        val skippedFrames: Long,
        val requests: Long,
        val errors: Long,
    )

    private var nativePtr: Long = nativeCreate(mode.nativeId, sampleRate, periodMs)
    private val tracks = mutableSetOf<Track>()

    /*
     * Playback getters bypass the instance monitor so opening media or stopping output does
     * not stall the video thread. A read lock keeps the native player alive; release() takes
     * the write lock before freeing it.
     */
    private val releaseLock = ReentrantReadWriteLock()

    init {
        if (nativePtr == 0L) throw IllegalStateException("Failed to create the audio player")
    }

    val sampleRate: Int = nativeGetSampleRate(nativePtr)

    private fun ptr(): Long = nativePtr.also { check(it != 0L) { "The audio player is released" } }

    private fun trackPtr(track: Track): Long {
        ptr()
        require(track.owner === this) { "The track belongs to another player" }
        check(track.pointer != 0L) { "The track is removed" }
        return track.pointer
    }

    private fun effectPtr(effect: Effect): Long {
        trackPtr(effect.track)
        check(effect.pointer != 0L) { "The effect is removed" }
        return effect.pointer
    }

    private fun paramId(effect: Effect, param: Param): Int {
        require(effect.type.params.getOrNull(param.id) === param) { "$param is not a parameter of ${effect.type}" }
        return param.id
    }

    private fun invalidate(track: Track) {
        track.pointer = 0L
        track.effects.forEach { it.pointer = 0L }
        track.effects.clear()
    }

    private fun invalidateTracks() {
        tracks.forEach { invalidate(it) }
        tracks.clear()
    }

    /* Round to the nearest frame so truncation in toDuration() does not lose a frame. */
    private fun Duration.toFrames(): Long {
        require(isFinite()) { "The duration must be finite" }
        val seconds = inWholeSeconds
        val remainder = (this - seconds.seconds).inWholeNanoseconds
        val frames = Math.floorDiv(remainder * sampleRate + 500_000_000L, 1_000_000_000L)
        return Math.addExact(Math.multiplyExact(seconds, sampleRate.toLong()), frames)
    }

    private fun Long.toDuration(): Duration =
        (this / sampleRate).seconds + ((this % sampleRate) * 1_000_000_000 / sampleRate).nanoseconds

    /**
     * Add [path] at project time [start], playing from media time [trim] for [duration].
     * A null duration plays the remaining media. The result contains a track only on [OK].
     * If the media length is unknown, an explicit duration is required.
     *
     * [path] can be a file path or URL, including a content URI converted with Uri.toString().
     */
    @Synchronized
    fun addTrack(
        path: String,
        start: Duration = Duration.ZERO,
        trim: Duration = Duration.ZERO,
        duration: Duration? = null,
        volume: Float = 1f,
        normalize: Boolean = false,
    ): TrackResult {
        val output = LongArray(1)
        val status = nativeAddTrack(
            ptr(), path, start.toFrames(), trim.toFrames(), duration?.toFrames() ?: -1, volume, normalize, output,
        )
        if (status != OK)
            return TrackResult(status, null)
        val track = Track(this, output[0])
        tracks.add(track)
        return TrackResult(OK, track)
    }

    @Synchronized
    fun setTrackTimings(track: Track, start: Duration, trim: Duration, duration: Duration?): Int =
        nativeSetTrackTimings(ptr(), trackPtr(track), start.toFrames(), trim.toFrames(), duration?.toFrames() ?: -1)

    @Synchronized
    fun setTrackVolume(track: Track, volume: Float): Int = nativeSetTrackVolume(ptr(), trackPtr(track), volume)

    private inline fun withRamps(
        ramps: List<Ramp>,
        set: (LongArray, LongArray, FloatArray, IntArray, FloatArray) -> Int,
    ): Int = set(
        ramps.map { it.start.toFrames() }.toLongArray(),
        ramps.map { it.length.toFrames() }.toLongArray(),
        ramps.map { it.value }.toFloatArray(),
        ramps.map { it.shape.nativeId }.toIntArray(),
        ramps.map { it.curvature }.toFloatArray(),
    )

    /**
     * Set volume ramps with linear gain values and times relative to the track start.
     * An empty list removes the ramps.
     */
    @Synchronized
    fun setTrackFader(track: Track, ramps: List<Ramp>): Int =
        withRamps(ramps) { frames, lengths, values, shapes, curvatures ->
            nativeSetTrackFader(ptr(), trackPtr(track), frames, lengths, values, shapes, curvatures)
        }

    /**
     * Append an effect of [type] to [track] at [placement], with default parameter values.
     * The result contains an effect only on [OK].
     */
    @Synchronized
    fun addEffect(track: Track, type: EffectType, placement: Placement = Placement.PreFader): EffectResult {
        val output = LongArray(1)
        val status = nativeAddEffect(ptr(), trackPtr(track), type.id, placement.nativeId, output)
        if (status != OK)
            return EffectResult(status, null)
        val effect = Effect(track, type, output[0])
        track.effects.add(effect)
        return EffectResult(OK, effect)
    }

    /**
     * Move [effect] to [index] in the [placement] chain, counted after removing it from its old
     * position. Processing state is preserved, but the new order can change the output abruptly.
     */
    @Synchronized
    fun moveEffect(effect: Effect, placement: Placement, index: Int): Int =
        nativeMoveEffect(ptr(), effectPtr(effect), placement.nativeId, index)

    @Synchronized
    fun removeEffect(effect: Effect): Int {
        val status = nativeRemoveEffect(ptr(), effectPtr(effect))
        if (status == OK) {
            effect.pointer = 0L
            effect.track.effects.remove(effect)
        }
        return status
    }

    /**
     * Set [param] of [effect] to [value], within the parameter's range. Changes ramp over 20 ms
     * while rendering; stepped parameters use integers and change immediately. Resuming from
     * silence applies the target immediately. Never returns [AGAIN]. Automation uses this value
     * until its first ramp.
     */
    @Synchronized
    fun setEffectParam(effect: Effect, param: Param, value: Float): Int =
        nativeSetEffectParam(ptr(), effectPtr(effect), paramId(effect, param), value)

    /**
     * Set automation for the parameters in [lanes], with times relative to the track start.
     * At least one lane is required.
     * An empty ramp list removes that parameter's automation; omitted parameters keep theirs.
     * All lanes take effect together, or none changes if the edit fails. Processing state is
     * preserved.
     */
    @Synchronized
    fun setEffectAutomation(effect: Effect, lanes: Map<Param, List<Ramp>>): Int =
        withRamps(lanes.values.flatten()) { frames, lengths, values, shapes, curvatures ->
            nativeSetEffectAutomation(
                ptr(), effectPtr(effect), lanes.keys.map { paramId(effect, it) }.toIntArray(),
                lanes.values.map { it.size }.toIntArray(), frames, lengths, values, shapes, curvatures,
            )
        }

    /** Set automation for a single parameter, preserving the other lanes. */
    @Synchronized
    fun setEffectAutomation(effect: Effect, param: Param, ramps: List<Ramp>): Int =
        setEffectAutomation(effect, mapOf(param to ramps))

    /**
     * Set the tracks the sidechain of [effect] listens to (see [EffectType.sidechain]), each one
     * heard after its volume and before its effects placed after it: tracks can listen to each
     * other through effects placed after their volume. Other tracks of the player, each of them
     * once.
     */
    @Synchronized
    fun setEffectSidechain(effect: Effect, keys: List<Track>): Int =
        nativeSetEffectSidechain(ptr(), effectPtr(effect), keys.map { trackPtr(it) }.toLongArray())

    /** Get the media length of a track, or zero if unknown. */
    @Synchronized
    fun getTrackLength(track: Track): Duration = nativeGetTrackLength(trackPtr(track)).toDuration()

    /** Get the playback duration of a track. */
    @Synchronized
    fun getTrackDuration(track: Track): Duration = nativeGetTrackDuration(trackPtr(track)).toDuration()

    @Synchronized
    fun removeTrack(track: Track): Int {
        val status = nativeRemoveTrack(ptr(), trackPtr(track))
        if (status == OK) {
            invalidate(track)
            tracks.remove(track)
        }
        return status
    }

    @Synchronized
    fun clearTracks(): Int {
        val status = nativeClearTracks(ptr())
        if (status == OK)
            invalidateTracks()
        return status
    }

    @Synchronized
    fun play(): Int = nativePlay(ptr())

    /** Pause playback while the output keeps running and applying seeks. */
    @Synchronized
    fun pause(): Int = nativePause(ptr())

    /** Stop the output and return to the project start. */
    @Synchronized
    fun stop(): Int = nativeStop(ptr())

    val isPlaying: Boolean
        get() = releaseLock.read { nativeIsPlaying(ptr()) }

    @Synchronized
    fun seek(position: Duration): Int = nativeSeek(ptr(), position.toFrames().coerceAtLeast(0))

    /** The next render position, or a pending seek position. */
    val position: Duration
        get() = releaseLock.read { nativeGetPosition(ptr()) }.toDuration()

    /**
     * Get the project position heard at [nanoTime], in the time base of System.nanoTime().
     * Accounts for output latency and smooths callback timing. Returns [position] while
     * rendering is stopped, or null if clock history is unavailable.
     */
    fun getPositionAt(nanoTime: Long): Duration? =
        releaseLock.read { nativeGetPositionAt(ptr(), nanoTime) }.takeIf { it >= 0 }?.toDuration()

    @get:Synchronized
    @set:Synchronized
    var duration: Duration
        get() = nativeGetDuration(ptr()).toDuration()
        set(value) {
            nativeSetDuration(ptr(), value.toFrames().coerceAtLeast(0))
        }

    @get:Synchronized
    @set:Synchronized
    var isLooping: Boolean
        get() = nativeIsLooping(ptr())
        set(value) {
            nativeSetLooping(ptr(), value)
        }

    /** Set the loop range, including [start] and excluding [end]; an empty range loops the project. */
    @Synchronized
    fun setLoopRange(start: Duration, end: Duration): Int =
        nativeSetLoopRange(ptr(), start.toFrames().coerceAtLeast(0), end.toFrames().coerceAtLeast(0))

    /** Set silence after each loop pass, holding the position at the loop end. */
    @Synchronized
    fun setLoopHold(hold: Duration): Int = nativeSetLoopHold(ptr(), hold.toFrames().coerceAtLeast(0))

    @get:Synchronized
    @set:Synchronized
    var volume: Float
        get() = nativeGetVolume(ptr())
        set(value) {
            require(nativeSetVolume(ptr(), value) == OK) { "The volume must be finite and nonnegative" }
        }

    /** Export the whole project to WAV in [Mode.Offline], ignoring loop settings. */
    @Synchronized
    fun export(file: File, format: ExportFormat = ExportFormat.S16): Int =
        nativeExport(ptr(), file.absolutePath, format.nativeId)

    val stats: Stats
        get() {
            val values = LongArray(5)
            releaseLock.read { nativeGetStats(ptr(), values) }
            return Stats(values[0], values[1], values[2], values[3], values[4])
        }

    @Synchronized
    fun release() {
        releaseLock.write {
            if (nativePtr != 0L) {
                nativeRelease(nativePtr)
                nativePtr = 0L
                invalidateTracks()
            }
        }
    }

    override fun close() = release()

    companion object {
        const val OK = 0
        const val AGAIN = 1
        const val ERROR_INVALID = -1
        const val ERROR_MEMORY = -2
        const val ERROR_UNSUPPORTED = -3
        const val ERROR_IO = -4

        /** Available effect types and their parameter descriptions */
        val effectTypes: List<EffectType> by lazy { loadEffectTypes() }

        /** The effect named [name] in [effectTypes] */
        fun effectType(name: String): EffectType =
            effectTypes.find { it.name == name } ?: throw IllegalArgumentException("No effect named $name")

        private fun loadEffectTypes(): List<EffectType> {
            val types = mutableListOf<EffectType>()
            val counts = IntArray(2)
            val values = FloatArray(4)
            while (true) {
                val type = types.size
                val name = nativeGetEffect(type, counts) ?: return types
                val params = List(counts[0]) { param ->
                    val paramName = checkNotNull(nativeGetParam(type, param, values))
                    Param(param, paramName, values[0], values[1], values[2], ParamScale.entries[values[3].toInt()])
                }
                types.add(EffectType(type, name, params, counts[1] != 0))
            }
        }

        /** The time base of [getPositionAt], that of System.nanoTime() */
        @JvmStatic
        fun now(): Long = nativeNow()

        init {
            Nope.load()
        }

        @JvmStatic private external fun nativeCreate(mode: Int, sampleRate: Int, periodMs: Int): Long
        @JvmStatic private external fun nativeRelease(ptr: Long)
        @JvmStatic private external fun nativeGetSampleRate(ptr: Long): Int
        @JvmStatic private external fun nativeAddTrack(
            ptr: Long, path: String, start: Long, trim: Long, duration: Long, volume: Float, normalize: Boolean,
            track: LongArray,
        ): Int
        @JvmStatic private external fun nativeSetTrackTimings(ptr: Long, track: Long, start: Long, trim: Long, duration: Long): Int
        @JvmStatic private external fun nativeSetTrackVolume(ptr: Long, track: Long, volume: Float): Int
        @JvmStatic private external fun nativeSetTrackFader(
            ptr: Long, track: Long, frames: LongArray, lengths: LongArray, values: FloatArray,
            shapes: IntArray, curvatures: FloatArray,
        ): Int
        @JvmStatic private external fun nativeGetEffect(type: Int, counts: IntArray): String?
        @JvmStatic private external fun nativeGetParam(type: Int, param: Int, values: FloatArray): String?
        @JvmStatic private external fun nativeAddEffect(
            ptr: Long, track: Long, type: Int, placement: Int, effect: LongArray,
        ): Int
        @JvmStatic private external fun nativeMoveEffect(ptr: Long, effect: Long, placement: Int, index: Int): Int
        @JvmStatic private external fun nativeRemoveEffect(ptr: Long, effect: Long): Int
        @JvmStatic private external fun nativeSetEffectParam(ptr: Long, effect: Long, param: Int, value: Float): Int
        @JvmStatic private external fun nativeSetEffectAutomation(
            ptr: Long, effect: Long, params: IntArray, counts: IntArray, frames: LongArray, lengths: LongArray,
            values: FloatArray, shapes: IntArray, curvatures: FloatArray,
        ): Int
        @JvmStatic private external fun nativeSetEffectSidechain(ptr: Long, effect: Long, keys: LongArray): Int
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
        @JvmStatic private external fun nativeExport(ptr: Long, path: String, format: Int): Int
        @JvmStatic private external fun nativeGetStats(ptr: Long, values: LongArray)
    }
}
