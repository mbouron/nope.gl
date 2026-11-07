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

package org.nopeforge.nae

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.time.Duration
import kotlin.time.Duration.Companion.days

@RunWith(AndroidJUnit4::class)
class AudioPlayerTest {
    private fun withAudio(test: (File) -> Unit) {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val file = File.createTempFile("nae-test", ".wav", context.cacheDir)
        val dataSize = 4800 * 4
        val data = ByteBuffer.allocate(44 + dataSize).order(ByteOrder.LITTLE_ENDIAN)
        data.put("RIFF".toByteArray()).putInt(36 + dataSize).put("WAVEfmt ".toByteArray())
        data.putInt(16).putShort(1).putShort(2).putInt(48000).putInt(48000 * 4)
        data.putShort(4).putShort(16).put("data".toByteArray()).putInt(dataSize)
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
