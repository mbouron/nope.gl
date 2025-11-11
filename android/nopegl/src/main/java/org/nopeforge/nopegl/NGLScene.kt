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

class NGLScene {
    var nativePtr: Long = 0

    var duration: Double = 0.0
        private set
    var frameRate: NGLRational = NGLRational(num = 60, den = 1)
        private set
    var aspectRatio: NGLRational = NGLRational(num = 1, den = 1)
        private set

    private var cleanable: NGLCleaner.Cleanable? = null

    private var liveControls: MutableMap<String, NGLNode> = mutableMapOf()

    constructor(serializedScene: String) {
        nativePtr = nativeInitFromString(serializedScene)
        require(nativePtr != 0L) { "Failed to create scene" }

        registerCleanable()

        val ret = nativeAddLiveControls(nativePtr)
        require(ret == 0) { "Failed to add native controls" }
    }

    constructor(
        rootNode: NGLNode,
        duration: Double = 0.0,
        frameRate: NGLRational = NGLRational(num = 60, den = 1),
        aspectRatio: NGLRational = NGLRational(num = 1, den = 1),
    ) {
        nativePtr = nativeCreateScene(
            nodePtr = rootNode.nativePtr,
            duration = duration,
            framerateNum = frameRate.num,
            framerateDen = frameRate.den,
            aspectRatioNum = aspectRatio.num,
            aspectRatioDen = aspectRatio.den
        )
        require(nativePtr != 0L) { "Failed to create scene" }

        registerCleanable()

        val ret = nativeAddLiveControls(nativePtr)
        require(ret == 0) { "Failed to add native controls" }

        setFields(duration, frameRate.num, frameRate.den, aspectRatio.num, aspectRatio.den)
    }

    private fun registerCleanable() {
        val ptr = nativePtr
        cleanable = NGLCleaner.register(this) {
            nativeRelease(ptr)
        }
    }

    private fun setFields(
        duration: Double,
        frameRateNum: Int,
        framerateDen: Int,
        aspectRatioNum: Int,
        aspectRatioDen: Int,
    ) {
        this.duration = duration
        this.frameRate = NGLRational(num = frameRateNum, den = framerateDen)
        this.aspectRatio = NGLRational(num = aspectRatioNum, den = aspectRatioDen)
    }

    fun serialize(): String {
        return nativeSerialize(nativePtr)
    }

    fun dot(): String {
        return nativeDot(nativePtr)
    }

    fun release() {
        cleanable?.clean()
        cleanable = null
        nativePtr = 0
    }

    private fun addLiveControl(id: String, nativePtr: Long) {
        liveControls[id] = NGLNode(nativePtr, true)
    }

    fun getLiveControl(id: String): NGLNode? {
        return liveControls[id]
    }

    private external fun nativeInitFromString(scene: String): Long
    private external fun nativeAddLiveControls(nativePtr: Long): Int
    private external fun nativeSerialize(nativePtr: Long): String
    private external fun nativeDot(nativePtr: Long): String

    companion object {
        @JvmStatic
        external fun nativeCreateScene(
            nodePtr: Long,
            duration: Double,
            framerateNum: Int,
            framerateDen: Int,
            aspectRatioNum: Int,
            aspectRatioDen: Int,
        ): Long

        @JvmStatic
        private external fun nativeRelease(nativePtr: Long)

    }
}
