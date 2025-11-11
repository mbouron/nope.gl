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

package org.nopeforge.nopegl

class NGLCustomTexture(callback: Callback) : NGLNode(NGLNodeType.CUSTOMTEXTURE) {

    abstract class Callback {
        private var nativePtr: Long = 0L

        protected abstract fun init()
        protected abstract fun prepare()
        protected abstract fun prefetch()
        protected abstract fun update(time: Double)
        protected abstract fun draw()
        protected abstract fun release()
        protected abstract fun uninit()

        internal fun setNativePtr(ptr: Long) {
            nativePtr = ptr;
        }

        data class Info(
            val width: Int,
            val height: Int,
            val texture: Int,
            val target: Int,
        )

        protected fun setTextureInfo(info: Info?) {
            if (info == null || info.texture == 0) {
                nativeSetTextureInfo(nativePtr)
            } else {
                nativeSetTextureInfo(
                    nativePtr,
                    info.texture,
                    info.target,
                    info.width,
                    info.height,
                )
            }
        }

        private external fun nativeSetTextureInfo(
            nativeContextPtr: Long,
            texture: Int = 0,
            target: Int = 0,
            width: Int = 0,
            height: Int = 0,
        ): Int

    }

    init {
        val nativeContextPtr = nativeCreate(nativePtr, callback)
        callback.setNativePtr(nativeContextPtr)
    }

    private external fun nativeCreate(nativePtr: Long, callback: Callback): Long

}
