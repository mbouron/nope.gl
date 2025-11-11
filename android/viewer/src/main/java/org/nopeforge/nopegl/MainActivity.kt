/*
 * Copyright 2024 Nope Forge
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

package org.nopeforge.nopegl

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.annotation.RequiresApi
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.nopeforge.nopegl.components.Player
import org.nopeforge.nopegl.components.rememberEngineRenderer
import timber.log.Timber
import java.io.File

class MainActivity : ComponentActivity() {

    private var receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                if (requestPermission()) return
            }

            val scenePath = intent.getStringExtra("scene")
            scenePath?.let {
                val sceneStr = File(scenePath).readText()
                try {
                    val newScene = NGLScene(sceneStr)
                    scene = newScene
                } catch (e: Exception) {
                    Timber.e(e)
                }
            }
        }
    }

    private var scene: NGLScene? by mutableStateOf(null)

    override fun onResume() {
        super.onResume()
        val intentFilter = IntentFilter()
        intentFilter.addAction("scene_update")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (requestPermission()) return
            registerReceiver(receiver, intentFilter, RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(receiver, intentFilter)
        }
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun requestPermission(): Boolean {
        if (!Environment.isExternalStorageManager()) {
            Timber.e("Could not update scene: missing permission")
            val uri = Uri.parse("package:${BuildConfig.APPLICATION_ID}")
            startActivity(
                Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    uri
                )
            )
            return true
        }
        return false
    }

    override fun onPause() {
        unregisterReceiver(receiver)
        super.onPause()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        NGLContext.init(this)
        super.onCreate(savedInstanceState)

        Timber.plant(Timber.DebugTree())
        setContent {
            MaterialTheme {

                var isPlaying by remember { mutableStateOf(true) }
                val renderer = rememberEngineRenderer(
                    scene = scene,
                    onSceneLoaded = {
                        Timber.d("Scene loaded")
                    },
                    playWhenReady = isPlaying,
                    onIsPlayingChanged = { isPlaying = it }
                )
                scene?.let { scene ->
                    Player(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(vertical = 16.dp),
                        scene = scene,
                        renderer = renderer,
                        isPlaying = isPlaying,
                        onIsPlayingCheckedChanged = { isPlaying = it }
                    )
                } ?: Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    Text("No scene loaded", style = MaterialTheme.typography.titleMedium)
                }
            }
        }
    }
}
