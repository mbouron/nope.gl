/*
 * Copyright 2024-2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

package org.nopeforge.nopegl.engine

import android.util.Rational
import android.view.Choreographer
import kotlin.time.Duration
import kotlin.time.Duration.Companion.nanoseconds
import kotlin.time.Duration.Companion.seconds
import kotlin.time.DurationUnit

class Clock {
    @Volatile
    var looping: Boolean = false

    @Volatile
    var listener: Clock.Listener? = null

    @Volatile
    var frameRate: Rational = Rational(60, 1)

    private var previousFrameTimeNanos = Long.MIN_VALUE

    private var state = State.Stopped

    @Volatile
    private var frameIndex: Long = 0

    private val maxFrameIndex: Long
        get() {
            return (duration.toDouble(DurationUnit.SECONDS) * frameRate.toDouble()).toLong()
                .coerceAtLeast(1L)
        }

    val time: Duration
        get() {
            val index = frameIndex
            val rate = frameRate
            return (index / rate.toDouble()).seconds
        }

    @Volatile
    var duration: Duration = Duration.ZERO

    private var choreographer: Choreographer? = null
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            onFrame(frameTimeNanos)
            if (state == State.Playing) {
                choreographer?.postFrameCallback(this)
            }
        }

        private fun onFrame(frameTimeNanos: Long) {
            synchronized(this@Clock) {
                if (state == State.Playing) {
                    if (previousFrameTimeNanos == Long.MIN_VALUE) {
                        previousFrameTimeNanos = frameTimeNanos
                    }
                    val elapsedDuration = (frameTimeNanos - previousFrameTimeNanos).nanoseconds
                    val elapsedFrames =
                        (elapsedDuration.toDouble(DurationUnit.SECONDS) * frameRate.toDouble()).toLong()

                    if (elapsedFrames > 0) {
                        previousFrameTimeNanos = frameTimeNanos
                        frameIndex = if (looping) {
                            (frameIndex + elapsedFrames) % maxFrameIndex
                        } else {
                            (frameIndex + elapsedFrames).coerceAtMost(maxFrameIndex)
                        }
                    }
                }
                listener?.onTick(time)
            }
        }

    }

    fun play() = synchronized(this) {
        state = State.Playing
        previousFrameTimeNanos = Long.MIN_VALUE
        choreographer = choreographer ?: Choreographer.getInstance()
        choreographer?.postFrameCallback(frameCallback)
    }

    fun pause() = synchronized(this) {
        choreographer?.removeFrameCallback(frameCallback)
        choreographer = null
        state = State.Paused
        previousFrameTimeNanos = Long.MIN_VALUE
    }

    fun stop() = synchronized(this) {
        choreographer?.removeFrameCallback(frameCallback)
        choreographer = null
        frameIndex = 0
        previousFrameTimeNanos = Long.MIN_VALUE
        state = State.Stopped
    }

    fun seek(time: Duration) = synchronized(this) {
        val newFrameIndex = (time.toDouble(DurationUnit.SECONDS) * frameRate.toDouble()).toLong()
        previousFrameTimeNanos = Long.MIN_VALUE
        frameIndex = newFrameIndex.coerceAtMost(maxFrameIndex)
        listener?.onTick(time)
    }

    fun step(step: Int) = synchronized(this) {
        frameIndex = (frameIndex + step).coerceIn(0L, maxFrameIndex)
        listener?.onTick(time)
    }

    private enum class State { Stopped, Paused, Playing }

    fun interface Listener {
        fun onTick(position: Duration)
    }
}
