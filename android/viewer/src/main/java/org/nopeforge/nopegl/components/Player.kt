/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.systemGestureExclusion
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconToggleButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.constraintlayout.compose.ConstraintLayout
import androidx.constraintlayout.compose.ConstraintSet
import androidx.constraintlayout.compose.Dimension
import androidx.constraintlayout.compose.layoutId
import org.nopeforge.nopegl.NGLScene
import org.nopeforge.nopegl.R
import org.nopeforge.nopegl.engine.EngineRenderer
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

private object Refs {
    const val ENGINE = "engine"
    const val CONTROLS = "controls"
}

private fun constraintSet() = ConstraintSet {
    val engine = createRefFor(Refs.ENGINE)
    val controls = createRefFor(Refs.CONTROLS)
    constrain(engine) {
        linkTo(start = parent.start, end = parent.end)
        top.linkTo(parent.top)
        width = Dimension.fillToConstraints
        height = Dimension.percent(0.8f)
    }
    constrain(controls) {
        linkTo(start = parent.start, end = parent.end)
        top.linkTo(engine.bottom)
        width = Dimension.fillToConstraints
        height = Dimension.wrapContent
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
    val constraintSet = remember { constraintSet() }

    ConstraintLayout(
        modifier = modifier
            .background(MaterialTheme.colorScheme.background),
        constraintSet = constraintSet,
    ) {
        Engine(
            renderer = renderer,
            modifier = Modifier
                .layoutId(Refs.ENGINE)
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
            onStop = { renderer.stop() },
        )
    }
}

private fun formatTime(timeInMs: Float): String {
    val totalSeconds = (timeInMs / 1000).toInt()
    val minutes = (totalSeconds % 3600) / 60
    val seconds = totalSeconds % 60
    val millis = (timeInMs % 1000).toInt()
    return "%d:%02d.%03d".format(minutes, seconds, millis)
}

@OptIn(ExperimentalMaterial3Api::class)
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
    onStop: () -> Unit = {},
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.background)
    ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = formatTime(progress),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                    modifier = Modifier.width(60.dp)
                )
                Slider(
                    modifier = Modifier
                        .weight(1f),
                    value = progress,
                    valueRange = progressRange,
                    onValueChange = onProgressChange,
                    onValueChangeFinished = onProgressChangeFinished,
                    colors = SliderDefaults.colors(
                        thumbColor = MaterialTheme.colorScheme.primary,
                        activeTrackColor = MaterialTheme.colorScheme.primary,
                        inactiveTrackColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f),
                    ),
                    track = { sliderState ->
                        SliderDefaults.Track(
                            sliderState = sliderState,
                            modifier = Modifier.height(3.dp),
                            thumbTrackGapSize = 0.dp,
                            trackInsideCornerSize = 0.dp,
                            drawStopIndicator = null,
                        )
                    },
                    thumb = {
                        SliderDefaults.Thumb(
                            interactionSource = remember { MutableInteractionSource() },
                            modifier = Modifier
                                .padding(0.dp)
                                .size(16.dp)
                                .background(MaterialTheme.colorScheme.primary, CircleShape),
                            colors = SliderDefaults.colors(
                                thumbColor = MaterialTheme.colorScheme.primary,
                            ),
                            thumbSize = DpSize(2.dp, 2.dp),
                        )
                    }
                )
                Text(
                    text = formatTime(progressRange.endInclusive),
                    modifier = Modifier.width(60.dp),
                    color = MaterialTheme.colorScheme.onSurface,
                    style = MaterialTheme.typography.labelSmall,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(
                    modifier = Modifier.size(36.dp),
                    onClick = { onStep(-1) }
                ) {
                    Icon(
                        painter = painterResource(id = R.drawable.ic_prev),
                        contentDescription = "Previous frame",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(28.dp)
                    )
                }

                Spacer(modifier = Modifier.width(8.dp))

                IconToggleButton(
                    modifier = Modifier.size(36.dp),
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
                        contentDescription = if (isPlaying) "Pause" else "Play",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(28.dp)
                    )
                }

                Spacer(modifier = Modifier.width(8.dp))

                IconButton(
                    modifier = Modifier.size(36.dp),
                    onClick = { onStep(1) }
                ) {
                    Icon(
                        painter = painterResource(id = R.drawable.ic_next),
                        contentDescription = "Next frame",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(28.dp)
                    )
                }

                Spacer(modifier = Modifier.width(8.dp))

                IconButton(
                    modifier = Modifier.size(36.dp),
                    onClick = onStop
                ) {
                    Icon(
                        painter = painterResource(id = R.drawable.ic_stop),
                        contentDescription = "Stop",
                        tint = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.size(28.dp)
                    )
                }
            }
    }
}


@Preview
@Composable
private fun PlayerPreview() {
    PlaybackControls(
        isPlaying = true,
        onIsPlayingCheckedChanged = {},
        progress = 15000f,
        progressRange = 0f..30000f,
        onProgressChange = {},
        onProgressChangeFinished = {},
        onStep = {},
        onStop = {},
        modifier = Modifier.fillMaxWidth()
    )
}
