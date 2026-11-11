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

package org.nopeforge.nopemedia

import android.graphics.PixelFormat
import android.media.ImageReader
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.nopeforge.Nope
import org.nopeforge.nopegl.NGLContentProvider

@RunWith(AndroidJUnit4::class)
class NopeMediaTest {
    private fun checkPlayback(uri: Uri) {
        val reader = ImageReader.newInstance(320, 240, PixelFormat.RGBA_8888, 2)
        try {
            val player = Player(uri, reader.surface)
            player.start()
            player.draw(0.5, blocking = true)
            assertEquals(0.5, player.currentTime, 0.0)
            player.release()
        } finally {
            reader.close()
        }
    }

    @Test
    fun contentUriPlayback() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val file = File(context.cacheDir, "player-cat.mp4")
        context.resources.assets.open("cat.mp4").use { input ->
            file.outputStream().use { input.copyTo(it) }
        }
        try {
            Nope.init(context)
            checkPlayback(Uri.parse(NGLContentProvider.getUri("medias${file.absolutePath}")))
        } finally {
            file.delete()
        }
    }

    @Test
    fun assetUriPlayback() {
        Nope.init(InstrumentationRegistry.getInstrumentation().targetContext)
        checkPlayback(Uri.parse("file:///android_asset/cat.mp4"))
    }
}
