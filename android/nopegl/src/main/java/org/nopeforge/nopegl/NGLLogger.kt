/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
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

import android.util.Log

enum class LogLevel(val priority: Int) {
    Verbose(Log.VERBOSE),
    Debug(Log.DEBUG),
    Info(Log.INFO),
    Warning(Log.WARN),
    Error(Log.ERROR);

    companion object {
        internal fun of(value: Int): LogLevel? {
            return values().firstOrNull { it.priority == value }
        }
    }
}

internal class NGLLogger(
    level: LogLevel,
    private val onLog: (level: LogLevel, tag: String, message: String) -> Unit,
) {
    private val nativePtr: Long

    init {
        nativePtr = nativeInit(level.priority)
        if (nativePtr == 0L) throw OutOfMemoryError()
    }

    fun release() {
        nativeRelease(nativePtr)
    }

    fun log(level: Int, message: String) {
        onLog(LogLevel.of(level) ?: return, "ngl", message)
    }

    private external fun nativeInit(level: Int): Long
    private external fun nativeRelease(nativePtr: Long)

}
