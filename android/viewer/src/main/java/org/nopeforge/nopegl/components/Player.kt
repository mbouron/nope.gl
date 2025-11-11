/*
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

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.systemGestureExclusion
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconToggleButton
import androidx.compose.material3.Slider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.toSize
import androidx.constraintlayout.compose.ConstraintLayout
import androidx.constraintlayout.compose.ConstraintSet
import androidx.constraintlayout.compose.Dimension
import androidx.constraintlayout.compose.layoutId
import org.nopeforge.nopegl.NGLScene
import org.nopeforge.nopegl.R
import org.nopeforge.nopegl.engine.EngineRenderer
import org.nopeforge.nopegl.engine.seek
import org.nopeforge.nopegl.engine.toFloat
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

private val Shape = RoundedCornerShape(16.dp)

private object Refs {
    const val ENGINE = "engine"
    const val CONTROLS = "controls"
}

private fun constraintSet(centerEngine: Boolean) = ConstraintSet {
    val engine = createRefFor(Refs.ENGINE)
    val controls = createRefFor(Refs.CONTROLS)
    constrain(engine) {
        linkTo(start = parent.start, end = parent.end)
        linkTo(top = parent.top, bottom = parent.bottom, bias = if (centerEngine) 0.5f else 0f)
        width = Dimension.fillToConstraints
        height = Dimension.wrapContent
    }
    constrain(controls) {
        linkTo(start = parent.start, end = parent.end)
        width = Dimension.fillToConstraints
        if (centerEngine) {
            bottom.linkTo(parent.bottom)
        } else {
            bottom.linkTo(engine.bottom)
        }
    }
}

@Composable
internal fun Player(
    renderer: EngineRenderer,
    scene: NGLScene,
    isPlaying: Boolean,
    onIsPlayingCheckedChanged: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    var sliderPosition by remember { mutableStateOf(renderer.time) }
    val updatedIsPlaying by rememberUpdatedState(newValue = isPlaying)
    var isSeeking by remember { mutableStateOf(false) }
    DisposableEffect(renderer) {
        val callback = object : EngineRenderer.PlaybackCallback {
            override fun onPositionChanged(position: Duration) {
                if (!isSeeking) sliderPosition = position
            }
        }
        renderer.addPlaybackCallback(callback)
        onDispose {
            renderer.removePlaybackCallback(callback)
        }
    }
    var engineSize by remember { mutableStateOf(Size.Zero) }
    var centerEngine by remember { mutableStateOf(false) }
    val constraintSet = remember(centerEngine) { constraintSet(centerEngine) }
    ConstraintLayout(
        modifier = modifier,
        constraintSet = constraintSet,
    ) {
        Engine(
            renderer = renderer,
            modifier = Modifier
                .layoutId(Refs.ENGINE)
                .onGloballyPositioned { coordinates ->
                    val height = coordinates.size.height
                    val parentHeight = coordinates.parentLayoutCoordinates?.size?.height ?: height
                    val ratio = height / parentHeight.toFloat()
                    centerEngine = ratio < 0.9f
                }
                .padding(vertical = 6.dp, horizontal = 3.dp)
                .onSizeChanged { engineSize = it.toSize() }
                .aspectRatio(
                    ratio = scene.aspectRatio.toFloat(),
                    matchHeightConstraintsFirst = true
                )
                .clip(Shape)
        )
        val duration = remember(scene) { scene.duration.seconds.inWholeMilliseconds.toFloat() }
        PlaybackControls(
            modifier = Modifier
                .layoutId(Refs.CONTROLS)
                .systemGestureExclusion(),
            isPlaying = updatedIsPlaying,
            onIsPlayingCheckedChanged = { onIsPlayingCheckedChanged(it) },
            progress = sliderPosition.inWholeMilliseconds.toFloat(),
            progressRange = 0f..duration,
            onProgressChange = { progress ->
                isSeeking = true
                renderer.pause()
                val newPosition = progress.toLong().milliseconds
                sliderPosition = newPosition
                renderer.seek(newPosition)
            },
            onProgressChangeFinished = { isSeeking = false },
            onStep = { step -> renderer.step(step) },
        )
    }
}

@Composable
fun PlaybackControls(
    isPlaying: Boolean,
    onIsPlayingCheckedChanged: (Boolean) -> Unit,
    progress: Float,
    progressRange: ClosedFloatingPointRange<Float>,
    onProgressChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    onProgressChangeFinished: () -> Unit = {},
    onStep: (step: Int) -> Unit = {},
) {
    Row(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconToggleButton(
            modifier = Modifier.size(48.dp),
            checked = isPlaying,
            onCheckedChange = onIsPlayingCheckedChanged,
        ) {
            val painter = if (isPlaying) {
                painterResource(id = R.drawable.ic_pause)
            } else {
                painterResource(id = R.drawable.ic_play)
            }
            Icon(
                painter = painter,
                contentDescription = null,
            )
        }
        IconButton(
            modifier = Modifier.size(48.dp),
            onClick = { onStep(-1) }
        ) {
            Icon(
                painter = painterResource(id = R.drawable.ic_prev),
                contentDescription = null,
            )
        }
        IconButton(
            modifier = Modifier.size(48.dp),
            onClick = { onStep(1) }
        ) {
            Icon(
                painter = painterResource(id = R.drawable.ic_next),
                contentDescription = null,
            )
        }
        Slider(
            modifier = Modifier.weight(1f),
            value = progress,
            valueRange = progressRange,
            onValueChange = onProgressChange,
            onValueChangeFinished = onProgressChangeFinished,
        )
    }
}
