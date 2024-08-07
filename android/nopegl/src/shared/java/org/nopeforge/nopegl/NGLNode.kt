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

import java.nio.ByteBuffer

open class NGLNode(
    val nativePtr: Long,
    label: String? = null,
) {
    constructor(nodeType: NGLNodeType) : this(nativeCreate(nodeType.type))

    constructor(nativePtr: Long, stealRef: Boolean) : this(nativePtr) {
        if (!stealRef) {
            nativeRef(nativePtr)
        }
    }

    init {
        label?.let { setString("label", it) }
    }

    fun finalize() {
        nativeUnref(nativePtr)
    }

    fun setLabel(label: String) {
        setString("label", label)
    }

    fun getLabel(): String {
        return nativeGetLabel(nativePtr)
    }

    fun getType(): NGLNodeType {
        return NGLNodeType.values().first { it.type == nativeGetType(nativePtr) }
    }

    fun getBoundingBox(): BoundingBox? {
        val nativeBoundingBox = nativeGetBoundingBox(nativePtr) ?: return null

        return BoundingBox(
            centerX = nativeBoundingBox[0],
            centerY = nativeBoundingBox[1],
            extentWidth = nativeBoundingBox[2],
            extentHeight = nativeBoundingBox[3],
        )
    }

    fun setBoolean(key: String, value: Boolean): Boolean {
        return nativeSetBoolean(nativePtr, key, value) == 0
    }

    fun setData(key: String, data: NGLData): Boolean {
        return nativeSetData(nativePtr, key, data.size, data.buffer) == 0
    }

    fun setDict(key: String, name: String, value: Long): Boolean {
        return nativeSetDict(nativePtr, key, name, value) == 0
    }

    fun setFloat(key: String, value: Float): Boolean {
        return nativeSetFloat(nativePtr, key, value) == 0
    }

    fun setDouble(key: String, value: Double): Boolean {
        return nativeSetDouble(nativePtr, key, value) == 0
    }

    fun addDoubles(key: String, values: List<Double>): Boolean {
        return nativeAddDoubles(nativePtr, key, values.size, values.toDoubleArray()) == 0
    }

    fun setFlags(key: String, value: String): Boolean {
        return nativeSetFlags(nativePtr, key, value) == 0
    }

    fun setInt(key: String, value: Int): Boolean {
        return nativeSetInt(nativePtr, key, value) == 0
    }

    fun setIVec2(key: String, value: NGLIVec2): Boolean {
        return nativeSetIVec2(nativePtr, key, value.array) == 0
    }

    fun setIVec3(key: String, value: NGLIVec3): Boolean {
        return nativeSetIVec3(nativePtr, key, value.array) == 0
    }

    fun setIVec4(key: String, value: NGLIVec4): Boolean {
        return nativeSetIVec4(nativePtr, key, value.array) == 0
    }

    fun setMat4(key: String, value: NGLMat4): Boolean {
        return nativeSetMat4(nativePtr, key, value) == 0
    }

    fun setNode(key: String, value: Long): Boolean {
        return nativeSetNode(nativePtr, key, value) == 0
    }

    fun addNodes(key: String, nodes: List<NGLNode>): Boolean {
        val nodePointers = nodes.map { it.nativePtr }.toLongArray()
        return nativeAddNodes(nativePtr, key, nodes.size, nodePointers) == 0
    }

    fun setRational(key: String, rational: NGLRational): Boolean {
        return nativeSetRational(
            nativePtr = nativePtr,
            key = key,
            num = rational.num,
            den = rational.den
        ) == 0
    }

    fun setSelect(key: String, value: String): Boolean {
        return nativeSetSelect(nativePtr, key, value) == 0
    }

    fun setString(key: String, value: String): Boolean {
        return nativeSetString(nativePtr, key, value) == 0
    }

    fun setUInt(key: String, value: UInt): Boolean {
        return nativeSetUInt(nativePtr, key, value.toInt()) == 0
    }

    fun setUVec2(key: String, value: NGLUVec2): Boolean {
        return nativeSetUVec2(nativePtr, key, value.array) == 0
    }

    fun setUVec3(key: String, value: NGLUVec3): Boolean {
        return nativeSetUVec3(nativePtr, key, value.array) == 0
    }

    fun setUVec4(key: String, value: NGLUVec4): Boolean {
        return nativeSetUVec4(nativePtr, key, value.array) == 0
    }

    fun setVec2(key: String, value: NGLVec2): Boolean {
        return nativeSetVec2(nativePtr, key, value.array) == 0
    }

    fun setVec3(key: String, value: NGLVec3): Boolean {
        return nativeSetVec3(nativePtr, key, value.array) == 0
    }

    fun setVec4(key: String, value: NGLVec4): Boolean {
        return nativeSetVec4(nativePtr, key, value.array) == 0
    }

    private external fun nativeGetLabel(nativePtr: Long): String
    private external fun nativeGetType(nativePtr: Long): Int
    private external fun nativeGetBoundingBox(nativePtr: Long): FloatArray?
    private external fun nativeRef(nativePtr: Long)
    private external fun nativeUnref(nativePtr: Long)
    private external fun nativeSetBoolean(nativePtr: Long, key: String, value: Boolean): Int
    private external fun nativeSetData(
        nativePtr: Long,
        key: String,
        size: Long,
        data: ByteBuffer,
    ): Int

    private external fun nativeSetDict(nativePtr: Long, key: String, name: String, value: Long): Int
    private external fun nativeSetFloat(nativePtr: Long, key: String, value: Float): Int
    private external fun nativeSetDouble(nativePtr: Long, key: String, value: Double): Int
    private external fun nativeAddDoubles(
        nativePtr: Long,
        key: String,
        count: Int,
        values: DoubleArray,
    ): Int

    private external fun nativeSetFlags(nativePtr: Long, key: String, value: String): Int
    private external fun nativeSetInt(nativePtr: Long, key: String, value: Int): Int
    private external fun nativeSetIVec2(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetIVec3(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetIVec4(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetMat4(nativePtr: Long, key: String, value: FloatArray): Int
    private external fun nativeSetNode(nativePtr: Long, key: String, value: Long): Int
    private external fun nativeSetRational(nativePtr: Long, key: String, num: Int, den: Int): Int
    private external fun nativeSetSelect(nativePtr: Long, key: String, value: String): Int
    private external fun nativeSetString(nativePtr: Long, key: String, value: String): Int
    private external fun nativeSetUInt(nativePtr: Long, key: String, value: Int): Int
    private external fun nativeSetUVec2(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetUVec3(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetUVec4(nativePtr: Long, key: String, value: IntArray): Int
    private external fun nativeSetVec2(nativePtr: Long, key: String, value: FloatArray): Int
    private external fun nativeSetVec3(nativePtr: Long, key: String, value: FloatArray): Int
    private external fun nativeSetVec4(nativePtr: Long, key: String, value: FloatArray): Int
    private external fun nativeAddNodes(
        nativePtr: Long,
        key: String,
        count: Int,
        nodePointers: LongArray,
    ): Int

    companion object {
        @JvmStatic
        external fun nativeCreate(type: Int): Long
    }
}
