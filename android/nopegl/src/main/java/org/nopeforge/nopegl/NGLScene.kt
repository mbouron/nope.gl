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
    var duration : Double = 0.0
        private set
    var frameRate : NGLRational = NGLRational(num = 60, den = 1)
        private set
    var aspectRatio : NGLRational = NGLRational(num = 1, den = 1)
        private set

    constructor(serializedScene: String) {
        nativePtr = nativeInitFromString(serializedScene)
        if (nativePtr == 0L)
            throw Exception()

        val ret = nativeAddLiveControls(nativePtr)
        if (ret < 0)
            throw Exception()
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
        if (nativePtr == 0L)
            throw Exception()

        val ret = nativeAddLiveControls(nativePtr)
        if (ret < 0)
            throw Exception()
    }

    private fun setFields(duration : Double, frameRateNum : Int, framerateDen: Int, aspectRatioNum: Int, aspectRatioDen: Int) {
        this.duration = duration
        this.frameRate = NGLRational(num = frameRateNum, den = framerateDen)
        this.aspectRatio = NGLRational(num = aspectRatioNum, den = aspectRatioDen)
    }

    var nativePtr: Long = 0
    private var liveControls: MutableMap<String, NGLNode> = mutableMapOf()

    fun serialize(): String {
        return nativeSerialize(nativePtr)
    }

    fun release() {
        if (nativePtr != 0L) {
            nativeRelease(nativePtr)
            nativePtr = 0
        }
    }

    fun finalize() {
        release()
    }

    private fun addLiveControl(id: String, nativePtr: Long) {
        liveControls[id] = NGLNode(nativePtr, true)
    }

    fun getLiveControl(id: String): NGLNode? {
        return liveControls[id]
    }

    private external fun nativeInitFromString(scene: String): Long
    private external fun nativeAddLiveControls(nativePtr: Long): Int
    private external fun nativeRelease(nativePtr: Long)
    private external fun nativeSerialize(nativePtr: Long): String

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
    }
}