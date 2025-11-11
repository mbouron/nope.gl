/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2024 Satyan Jacquens <satyan@mojo.video>
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

package org.nopeforge.nopegl.components

import android.view.TextureView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import org.nopeforge.nopegl.NGLScene
import org.nopeforge.nopegl.engine.EngineRenderer
import org.nopeforge.nopegl.engine.attach
import timber.log.Timber
import kotlin.time.Duration

@Composable
fun Engine(
    renderer: EngineRenderer,
    modifier: Modifier = Modifier,
) {
    if (LocalInspectionMode.current) {
        Box(modifier = modifier.background(color = Color.Green))
        return
    }
    AndroidView(
        modifier = modifier,
        factory = { context ->
            TextureView(context)
                .also(renderer::attach)
        },
    )
}

@Composable
fun rememberEngineRenderer(
    scene: NGLScene?,
    playWhenReady: Boolean = true,
    onIsPlayingChanged: (Boolean) -> Unit = {},
    onSceneLoaded: () -> Unit = {},
    onPositionChanged: (Duration) -> Unit = { },
    onStateChanged: (Int) -> Unit = {},
    looping: Boolean = true,
): EngineRenderer {
    val renderer = remember { EngineRenderer() }
    DisposableEffect(renderer) {
        val callback = object : EngineRenderer.PlaybackCallback {
            override fun onStateChanged(state: Int) {
                onStateChanged(state)
            }

            override fun onStarted() {
                onIsPlayingChanged(true)
            }

            override fun onStopped() {
                onIsPlayingChanged(false)
            }

            override fun onPaused() {
                onIsPlayingChanged(false)
            }

            override fun onReleased() {
                onIsPlayingChanged(false)
            }

            override fun onPositionChanged(position: Duration) {
                onPositionChanged(position)
            }

            override fun onSceneChanged(scene: NGLScene?, elapsed: Duration?) {
                if (scene != null) {
                    onSceneLoaded()
                    Timber.d("Scene loaded $elapsed")
                }
            }
        }
        renderer.addPlaybackCallback(callback)
        onDispose {
            renderer.removePlaybackCallback(callback)
        }
    }
    val isResumed by rememberLifecycleResumedState()
    LaunchedEffect(scene) { renderer.setScene(scene) }
    LaunchedEffect(playWhenReady, isResumed) { renderer.playWhenReady = playWhenReady && isResumed }
    LaunchedEffect(looping) { renderer.looping = looping }
    DisposableEffect(renderer) { onDispose { renderer.release() } }
    return renderer
}

@Composable
internal fun rememberLifecycleResumedState(): State<Boolean> {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val isResumedState = remember {
        mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
    }
    val lifecycleObserver = remember {
        object : DefaultLifecycleObserver {
            override fun onResume(owner: LifecycleOwner) {
                isResumedState.value = true
            }

            override fun onPause(owner: LifecycleOwner) {
                isResumedState.value = false
            }
        }
    }
    DisposableEffect(lifecycle) {
        lifecycle.addObserver(lifecycleObserver)
        onDispose {
            lifecycle.removeObserver(lifecycleObserver)
        }
    }
    return isResumedState
}
