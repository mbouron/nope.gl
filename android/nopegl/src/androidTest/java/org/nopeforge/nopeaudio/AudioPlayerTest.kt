/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread
import org.nopeforge.Nope
import org.nopeforge.nopegl.NGLContentProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.time.Duration
import kotlin.time.Duration.Companion.days
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.nanoseconds
import kotlin.time.Duration.Companion.seconds
import kotlin.math.PI
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.random.Random

@RunWith(AndroidJUnit4::class)
class AudioPlayerTest {
    /** A stereo WAV file at 48 kHz, both channels of each frame the sample of its index */
    private fun withAudio(frames: Int = 4800, sample: (Int) -> Short = { 0 }, test: (File) -> Unit) {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val file = File.createTempFile("nau-test", ".wav", context.cacheDir)
        val dataSize = frames * 4
        val data = ByteBuffer.allocate(44 + dataSize).order(ByteOrder.LITTLE_ENDIAN)
        data.put("RIFF".toByteArray()).putInt(36 + dataSize).put("WAVEfmt ".toByteArray())
        data.putInt(16).putShort(1).putShort(2).putInt(48000).putInt(48000 * 4)
        data.putShort(4).putShort(16).put("data".toByteArray()).putInt(dataSize)
        for (i in 0 until frames) {
            val value = sample(i)
            data.putShort(value).putShort(value)
        }
        file.writeBytes(data.array())
        try {
            test(file)
        } finally {
            file.delete()
        }
    }

    private fun addTrack(player: AudioPlayer, file: File): AudioPlayer.Track {
        val result = player.addTrack(file.absolutePath)
        assertEquals(AudioPlayer.OK, result.status)
        return checkNotNull(result.track)
    }

    /**
     * The position moves at the pace of the output, and once playing the track's audio is there in time
     * (starting at once, the track may not have its first frames yet)
     */
    private fun playsInRealTime(mode: AudioPlayer.Mode) = withAudio(frames = 48000) { file ->
        AudioPlayer(mode).use { player ->
            player.duration = 1.seconds
            addTrack(player, file)
            val start = System.nanoTime()
            assertEquals(AudioPlayer.OK, player.play())
            Thread.sleep(100)
            val started = player.stats
            Thread.sleep(400)
            val played = player.position
            val heard = player.getPositionAt(AudioPlayer.now())
            val elapsed = (System.nanoTime() - start).nanoseconds
            val stats = player.stats
            assertEquals(AudioPlayer.OK, player.stop())
            // Rendered ahead by the output's buffering, behind by its start
            assertTrue("$played played in $elapsed", (played - elapsed).absoluteValue < 200.milliseconds)
            assertEquals(0L, stats.errors)
            assertEquals(started.silentFrames, stats.silentFrames)
            // What is heard is behind what is rendered, by the output's latency
            checkNotNull(heard)
            Log.i("AudioPlayerTest", "$mode: rendered ${played - heard} ahead of the output")
            assertTrue("heard $heard, played $played", heard <= played && played - heard < 200.milliseconds)
        }
    }

    @Test
    fun playsOnTheOutput() = playsInRealTime(AudioPlayer.Mode.Device)

    @Test
    fun playsOnTheNullDevice() = playsInRealTime(AudioPlayer.Mode.NullDevice)

    /**
     * Controls in quick succession, the output stopped while its callbacks run, then the player
     * released while playing
     */
    @Test
    fun outputFollowsRapidControls() = withAudio(frames = 48000) { file ->
        val random = Random(1)
        AudioPlayer(AudioPlayer.Mode.Device).use { player ->
            player.duration = 1.seconds
            addTrack(player, file)
            repeat(200) {
                val status = when (random.nextInt(4)) {
                    0 -> player.play()
                    1 -> player.pause()
                    2 -> player.seek(random.nextInt(900).milliseconds)
                    else -> player.stop()
                }
                assertEquals(AudioPlayer.OK, status)
                player.getPositionAt(AudioPlayer.now())
            }
            assertEquals(0L, player.stats.errors)
            assertEquals(AudioPlayer.OK, player.play())
            Thread.sleep(50)
            assertTrue(player.isPlaying)
        }
    }

    /** A control call holds the monitor of the player as long as it blocks: the getters return meanwhile */
    @Test
    fun playbackGettersDoNotWaitBehindControlCalls() = withAudio { file ->
        AudioPlayer(AudioPlayer.Mode.NullDevice).use { player ->
            player.duration = 1.seconds
            addTrack(player, file)
            assertEquals(AudioPlayer.OK, player.seek(50.milliseconds))
            val getters = FutureTask {
                val heard = player.getPositionAt(AudioPlayer.now())
                Triple(player.isPlaying, player.position, heard) to player.stats
            }
            synchronized(player) {
                thread { getters.run() }
                val (playback, stats) = getters.get(10, TimeUnit.SECONDS)
                assertEquals(Triple(false, 50.milliseconds, 50.milliseconds), playback)
                assertEquals(0L, stats.errors)
            }
        }
    }

    /** A getter either reads the player or finds it released, never a player being freed */
    @Test
    fun playbackGettersAreSafeWhileReleasing() = withAudio { file ->
        repeat(50) { round ->
            val player = AudioPlayer(AudioPlayer.Mode.NullDevice)
            addTrack(player, file)
            assertEquals(AudioPlayer.OK, player.play())
            val end = AtomicReference<Throwable>()
            val reader = thread {
                try {
                    while (true) {
                        player.getPositionAt(AudioPlayer.now())
                        player.position
                        player.isPlaying
                        player.stats
                    }
                } catch (e: Throwable) {
                    end.set(e)
                }
            }
            Thread.sleep(round % 5L)
            player.close()
            reader.join(10_000)
            assertTrue("${end.get()}", end.get() is IllegalStateException)
        }
    }

    @Test
    fun removedTracksAreInvalid() = withAudio { file ->
        AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
            val track = addTrack(player, file)
            assertEquals(AudioPlayer.OK, player.removeTrack(track))
            assertThrows(IllegalStateException::class.java) { player.getTrackDuration(track) }
            assertThrows(IllegalStateException::class.java) { player.getTrackLength(track) }
            assertThrows(IllegalStateException::class.java) { player.removeTrack(track) }
            addTrack(player, file)
            assertThrows(IllegalStateException::class.java) { player.setTrackVolume(track, 0f) }
        }
    }

    @Test
    fun clearedAndReleasedTracksAreInvalid() = withAudio { file ->
        val player = AudioPlayer(AudioPlayer.Mode.Offline)
        val cleared = addTrack(player, file)
        assertEquals(AudioPlayer.OK, player.clearTracks())
        assertThrows(IllegalStateException::class.java) { player.getTrackDuration(cleared) }
        val released = addTrack(player, file)
        player.close()
        assertThrows(IllegalStateException::class.java) { player.getTrackDuration(released) }
        player.close()
    }

    @Test
    fun foreignTracksAreRejected() = withAudio { file ->
        AudioPlayer(AudioPlayer.Mode.Offline).use { first ->
            AudioPlayer(AudioPlayer.Mode.Offline).use { second ->
                val track = addTrack(first, file)
                assertThrows(IllegalArgumentException::class.java) { second.getTrackDuration(track) }
                assertThrows(IllegalArgumentException::class.java) { second.removeTrack(track) }
            }
        }
    }

    /** A content URI opens with the context Nope.init() gives, nope.gl not being initialized */
    @Test
    fun contentUriPlays() = withAudio(sample = { 1000 }) { file ->
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val output = File.createTempFile("nau-content", ".wav", context.cacheDir)
        try {
            Nope.init(context)
            Nope.init(context)
            AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
                player.duration = 100.milliseconds
                val result = player.addTrack(NGLContentProvider.getUri("medias${file.absolutePath}"))
                assertEquals(AudioPlayer.OK, result.status)
                assertEquals(100.milliseconds, player.getTrackLength(checkNotNull(result.track)))
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val samples = readWav(output)
                assertEquals(2 * 4800, samples.size)
                assertEquals(1000 / 32768f, samples[2 * 2400], 1e-6f)
            }
        } finally {
            output.delete()
        }
    }

    @Test
    fun missingMediaReturnsAnError() = withAudio { file ->
        AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
            val result = player.addTrack(File(file, "missing.wav").absolutePath)
            assertEquals(AudioPlayer.ERROR_IO, result.status)
            assertNull(result.track)
        }
    }

    @Test
    fun largeDurationsPreserveTheirValue() {
        AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
            val duration = 10000.days
            player.duration = duration
            assertEquals(duration, player.duration)
            assertEquals(AudioPlayer.OK, player.seek(duration))
            assertEquals(duration, player.position)
            assertThrows(IllegalArgumentException::class.java) { player.seek(Duration.INFINITE) }
        }
    }

    /** The samples of a stereo float WAV file written by the player */
    /** The float samples of a WAV file. FFmpeg can write JUNK, fact and LIST chunks before data. */
    private fun readWav(file: File): FloatArray {
        val data = ByteBuffer.wrap(file.readBytes()).order(ByteOrder.LITTLE_ENDIAN)
        data.position(12)
        while (true) {
            val id = ByteArray(4).also { data.get(it) }
            val size = data.int
            if (String(id, Charsets.US_ASCII) == "data")
                return FloatArray(size / 4) { data.float }
            data.position(data.position() + size + (size and 1))
        }
    }

    /** The level of the left channel over [from, to), at 48 kHz */
    private fun rms(samples: FloatArray, from: Duration, to: Duration): Double {
        val first = (from.inWholeMicroseconds * 48 / 1000).toInt()
        val last = (to.inWholeMicroseconds * 48 / 1000).toInt()
        var sum = 0.0
        for (i in first until last)
            sum += samples[2 * i].toDouble() * samples[2 * i]
        return sqrt(sum / (last - first))
    }

    /** A fade in over a second, heard halfway through it: each shape at its own level */
    @Test
    fun faderFollowsItsRampShape() = withAudio(
        frames = 48000,
        sample = { (16384 * sin(2 * PI * 1000 * it / 48000)).toInt().toShort() },
    ) { file ->
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val output = File.createTempFile("nau-fader", ".wav", context.cacheDir)
        try {
            AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
                player.duration = 1.seconds
                val track = addTrack(player, file)
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val level = rms(readWav(output), 450.milliseconds, 550.milliseconds)
                assertTrue("$level", level > 0.3)

                // A fade in over the whole second, heard halfway through it
                fun halfway(shape: AudioPlayer.RampShape, curvature: Float = 0f): Double {
                    val fadeIn = listOf(
                        AudioPlayer.Ramp(Duration.ZERO, 0f),
                        AudioPlayer.Ramp(Duration.ZERO, 1f, 1.seconds, shape, curvature),
                    )
                    assertEquals(AudioPlayer.OK, player.setTrackFader(track, fadeIn))
                    assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                    return rms(readWav(output), 450.milliseconds, 550.milliseconds) / level
                }
                // Linear in amplitude: half; dB from silence: along a 60 dB ramp, -30.3 dB
                assertEquals(0.5, halfway(AudioPlayer.RampShape.Linear), 0.05)
                assertEquals(0.03065, halfway(AudioPlayer.RampShape.Db), 0.01)
                assertEquals(0.707, halfway(AudioPlayer.RampShape.EqualPower), 0.05)
                assertTrue(halfway(AudioPlayer.RampShape.Linear, 1f) > 0.85)

                for (invalid in listOf(2f, Float.NaN))
                    assertEquals(
                        AudioPlayer.ERROR_INVALID,
                        player.setTrackFader(track, listOf(AudioPlayer.Ramp(Duration.ZERO, 1f, curvature = invalid))),
                    )
            }
        } finally {
            output.delete()
        }
    }

    /** The effects and their parameters, as the library describes them */
    @Test
    fun effectsAreDescribedByTheLibrary() {
        val types = AudioPlayer.effectTypes
        assertTrue(types.isNotEmpty())
        assertEquals(types.size, types.map { it.name }.toSet().size)
        for (type in types) {
            for (param in type.params) {
                assertTrue("$type $param", param.min <= param.defaultValue && param.defaultValue <= param.max)
                assertTrue(type.param(param.name) === param)
            }
        }
        val passthrough = AudioPlayer.effectType("passthrough")
        assertTrue(passthrough.params.isEmpty())
        assertThrows(IllegalArgumentException::class.java) { AudioPlayer.effectType("unknown") }
        assertThrows(IllegalArgumentException::class.java) { passthrough.param("unknown") }
        val lowpass = AudioPlayer.effectType("lowpass")
        assertEquals(AudioPlayer.ParamScale.Log, lowpass.param("cutoff").scale)
        assertEquals(AudioPlayer.ParamScale.Stepped, lowpass.param("slope").scale)
    }

    /** Under water from 0.2 s to 0.6 s: the filter closes on a 4 kHz tone, then opens again */
    /** Effects added, moved and removed by their handles: passthroughs leave the track as it is */
    @Test
    fun passthroughsLeaveTheTrackAsIs() = withAudio(
        frames = 48000,
        sample = { (16384 * sin(2 * PI * 1000 * it / 48000)).toInt().toShort() },
    ) { file ->
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val output = File.createTempFile("nau-passthrough", ".wav", context.cacheDir)
        try {
            AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
                player.duration = 1.seconds
                val track = addTrack(player, file)
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val dry = readWav(output)

                val passthrough = AudioPlayer.effectType("passthrough")
                val pre = checkNotNull(player.addEffect(track, passthrough).effect)
                val post = checkNotNull(player.addEffect(track, passthrough, AudioPlayer.Placement.PostFader).effect)
                assertEquals(AudioPlayer.OK, player.moveEffect(pre, AudioPlayer.Placement.PostFader, 1))
                assertEquals(AudioPlayer.ERROR_INVALID, player.moveEffect(post, AudioPlayer.Placement.PreFader, 1))
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                assertTrue(dry.contentEquals(readWav(output)))

                // Removed, and with their track
                assertEquals(AudioPlayer.OK, player.removeEffect(pre))
                assertThrows(IllegalStateException::class.java) { player.moveEffect(pre, AudioPlayer.Placement.PreFader, 0) }
                assertEquals(AudioPlayer.OK, player.removeTrack(track))
                assertThrows(IllegalStateException::class.java) { player.removeEffect(post) }
            }
        } finally {
            output.delete()
        }
    }

    @Test
    fun lowpassMufflesTheTrack() = withAudio(
        frames = 48000,
        sample = { (16384 * sin(2 * PI * 4000 * it / 48000)).toInt().toShort() },
    ) { file ->
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val output = File.createTempFile("nau-lowpass", ".wav", context.cacheDir)
        try {
            AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
                player.duration = 1.seconds
                val track = addTrack(player, file)
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val dry = readWav(output)

                val lowpass = AudioPlayer.effectType("lowpass")
                val cutoff = lowpass.param("cutoff")
                val resonance = lowpass.param("resonance")
                val amount = lowpass.param("amount")
                val resonanceScale = lowpass.param("resonance_scale")
                val effect = checkNotNull(player.addEffect(track, lowpass).effect)
                val dive = listOf(
                    AudioPlayer.Ramp(200.milliseconds, 400f, 50.milliseconds),
                    AudioPlayer.Ramp(600.milliseconds, cutoff.max, 200.milliseconds),
                )
                val peak = listOf(
                    AudioPlayer.Ramp(200.milliseconds, 1.2f, 50.milliseconds),
                    AudioPlayer.Ramp(600.milliseconds, resonance.defaultValue, 200.milliseconds),
                )
                assertEquals(AudioPlayer.OK, player.setEffectAutomation(effect, mapOf(cutoff to dive, resonance to peak)))
                for (invalid in listOf(Float.NaN, 0f, cutoff.max * 2))
                    assertEquals(
                        AudioPlayer.ERROR_INVALID,
                        player.setEffectAutomation(effect, cutoff, listOf(AudioPlayer.Ramp(Duration.ZERO, invalid))),
                    )
                for (invalid in listOf(0f, 41f))
                    assertEquals(AudioPlayer.ERROR_INVALID, player.setEffectParam(effect, resonance, invalid))
                // The lanes change together: none does with an invalid one
                val invalidPeak = listOf(AudioPlayer.Ramp(Duration.ZERO, 0f))
                assertEquals(
                    AudioPlayer.ERROR_INVALID,
                    player.setEffectAutomation(effect, mapOf(cutoff to emptyList(), resonance to invalidPeak)),
                )
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val wet = readWav(output)
                val level = rms(dry, 100.milliseconds, 200.milliseconds)
                assertTrue("$level", level > 0.3)
                assertEquals(level, rms(wet, 100.milliseconds, 200.milliseconds), 0.01 * level)
                assertTrue(rms(wet, 300.milliseconds, 600.milliseconds) < level / 100)
                assertEquals(level, rms(wet, 850.milliseconds, 1000.milliseconds), 0.01 * level)

                // Live: no depth, the tone heard under water as it is
                assertEquals(AudioPlayer.OK, player.setEffectParam(effect, amount, 0f))
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                assertEquals(level, rms(readWav(output), 300.milliseconds, 600.milliseconds), 0.01 * level)
                assertEquals(AudioPlayer.ERROR_INVALID, player.setEffectParam(effect, amount, Float.NaN))
                assertEquals(AudioPlayer.ERROR_INVALID, player.setEffectParam(effect, resonanceScale, -1f))
                assertEquals(AudioPlayer.OK, player.setEffectParam(effect, resonanceScale, 2f))
                assertEquals(AudioPlayer.OK, player.setEffectParam(effect, resonanceScale, 1f))
                assertEquals(AudioPlayer.OK, player.setEffectParam(effect, amount, 1f))

                // A 12 dB filter, then none
                assertEquals(AudioPlayer.ERROR_INVALID, player.setEffectParam(effect, lowpass.param("slope"), 0.5f))
                assertEquals(AudioPlayer.OK, player.setEffectParam(effect, lowpass.param("slope"), 0f))
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                val gentle = rms(readWav(output), 300.milliseconds, 600.milliseconds)
                assertTrue("$gentle", gentle > rms(wet, 300.milliseconds, 600.milliseconds) && gentle < level / 10)
                assertEquals(AudioPlayer.OK, player.removeEffect(effect))
                assertEquals(AudioPlayer.OK, player.export(output, AudioPlayer.ExportFormat.F32))
                assertTrue(dry.contentEquals(readWav(output)))
                assertThrows(IllegalStateException::class.java) { player.setEffectParam(effect, amount, 0f) }

                // The effects of a removed track are rejected
                val other = checkNotNull(player.addEffect(track, lowpass).effect)
                assertEquals(AudioPlayer.OK, player.removeTrack(track))
                assertThrows(IllegalStateException::class.java) { player.setEffectAutomation(other, cutoff, dive) }
                assertThrows(IllegalStateException::class.java) { player.removeEffect(other) }
            }
        } finally {
            output.delete()
        }
    }

    @Test
    fun invalidVolumeIsRejected() {
        AudioPlayer(AudioPlayer.Mode.Offline).use { player ->
            player.volume = 0.5f
            for (volume in listOf(-1f, Float.NaN, Float.POSITIVE_INFINITY)) {
                assertThrows(IllegalArgumentException::class.java) { player.volume = volume }
                assertEquals(0.5f, player.volume, 0f)
            }
        }
    }
}
