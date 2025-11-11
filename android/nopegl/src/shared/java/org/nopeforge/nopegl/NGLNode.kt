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
    private var cleanable : NGLCleaner.Cleanable? = null

    init {
        require(nativePtr != 0L)
        label?.let { setString("label", it) }
        registerCleanable()
    }

    private fun registerCleanable() {
        val ptr = nativePtr
        cleanable = NGLCleaner.register(this) {
            nativeUnref(ptr)
        }
    }

    fun release() {
        cleanable?.clean()
        cleanable = null
    }

    internal fun setBoolean(key: String, value: Boolean) {
        val returnCode = nativeSetBoolean(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setData(key: String, data: NGLData) {
        val returnCode = nativeSetData(nativePtr, key, data.size, data.buffer)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setDict(key: String, name: String, value: Long) {
        val returnCode = nativeSetDict(nativePtr, key, name, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setFloat(key: String, value: Float) {
        val returnCode = nativeSetFloat(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setDouble(key: String, value: Double) {
        val returnCode = nativeSetDouble(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun addDoubles(key: String, values: List<Double>) {
        val returnCode = nativeAddDoubles(nativePtr, key, values.size, values.toDoubleArray())
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setFlags(key: String, value: String) {
        val returnCode = nativeSetFlags(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setInt(key: String, value: Int) {
        val returnCode = nativeSetInt(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setIVec2(key: String, value: NGLIVec2) {
        val returnCode = nativeSetIVec2(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setIVec3(key: String, value: NGLIVec3) {
        val returnCode = nativeSetIVec3(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setIVec4(key: String, value: NGLIVec4) {
        val returnCode = nativeSetIVec4(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setMat4(key: String, value: NGLMat4) {
        val returnCode = nativeSetMat4(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setNode(key: String, value: Long) {
        val returnCode = nativeSetNode(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun addNodes(key: String, nodes: List<NGLNode>) {
        val nodePointers = nodes.map { it.nativePtr }.toLongArray()
        val returnCode =  nativeAddNodes(nativePtr, key, nodes.size, nodePointers)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun swapElement(key: String, from: Int, to: Int) {
        val returnCode = nativeSwapElement(nativePtr, key, from, to)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setRational(key: String, rational: NGLRational) {
        val returnCode = nativeSetRational(
            nativePtr = nativePtr,
            key = key,
            num = rational.num,
            den = rational.den
        )
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setSelect(key: String, value: String) {
        val returnCode = nativeSetSelect(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setString(key: String, value: String) {
        val returnCode = nativeSetString(nativePtr, key, value)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setUInt(key: String, value: UInt) {
        val returnCode = nativeSetUInt(nativePtr, key, value.toInt())
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setUVec2(key: String, value: NGLUVec2) {
        val returnCode = nativeSetUVec2(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setUVec3(key: String, value: NGLUVec3) {
        val returnCode = nativeSetUVec3(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setUVec4(key: String, value: NGLUVec4) {
        val returnCode = nativeSetUVec4(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setVec2(key: String, value: NGLVec2) {
        val returnCode = nativeSetVec2(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setVec3(key: String, value: NGLVec3) {
        val returnCode = nativeSetVec3(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setVec4(key: String, value: NGLVec4) {
        val returnCode = nativeSetVec4(nativePtr, key, value.array)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    internal fun setTimeRangeFilterRange(start: Double, end: Double) {
        val returnCode = nativeTimeRangeFilterUpdate(nativePtr, start, end)
        if (returnCode != 0) {
            throw NGLError(returnCode)
        }
    }

    private external fun nativeGetLabel(nativePtr: Long): String
    private external fun nativeGetType(nativePtr: Long): Int
    private external fun nativeGetBoundingBox(nativePtr: Long): FloatArray?
    private external fun nativeGetOrientedBoundingBox(nativePtr: Long): FloatArray?
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

    private external fun nativeSwapElement(nativePtr: Long, key: String, from: Int, to: Int): Int
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

    private external fun nativeTimeRangeFilterUpdate(
        nativePtr: Long,
        start: Double,
        end: Double,
    ): Int

    companion object {
        @JvmStatic
        external fun nativeCreate(type: Int): Long
        @JvmStatic
        external fun nativeRef(nativePtr: Long)
        @JvmStatic
        external fun nativeUnref(nativePtr: Long)
    }
}
