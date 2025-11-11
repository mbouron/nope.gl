/*
 * Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

package org.nopeforge.nopegl.utils

import android.content.Context
import timber.log.Timber
import java.io.File
import java.io.FileOutputStream

object AssetUtils {

    var thread: Thread? = null

    private fun getAssetPath(context: Context, name: String): String {
        return context.cacheDir!!.path + "/" + name
    }

    private fun isAssetCached(context: Context, name: String): Boolean {
        return File(getAssetPath(context, name)).exists()
    }

    private fun copyAsset(context: Context, name: String) {
        val inputStream = context.resources.assets.open(name)
        inputStream.copyTo(FileOutputStream(getAssetPath(context, name)))
    }

    fun resolveAssetPath(context: Context, name: String): String {
        thread?.join()
        return context.cacheDir!!.path + "/" + name
    }

    fun initAssets(context: Context) {
        thread = Thread {
            try {
                val assets = listOf(
                    "cat.mp4"
                )
                assets.forEach {
                    if (!isAssetCached(context, it)) {
                        copyAsset(context, it)
                    }
                }
            } catch (e: Exception) {
                Timber.e(e, "Failed to copy assets")
            }
        }
        thread?.start()
    }
}
