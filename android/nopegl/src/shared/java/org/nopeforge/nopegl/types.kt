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

package org.nopeforge.nopegl

import java.nio.ByteBuffer

@JvmInline
value class NGLVec2 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float) : this(floatArrayOf(x, y))
}

@JvmInline
value class NGLVec3 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float, z: Float) : this(floatArrayOf(x, y, z))
}

@JvmInline
value class NGLVec4 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float, z: Float, w: Float) : this(floatArrayOf(x, y, z, w))
}

@JvmInline
value class NGLIVec2 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int) : this(intArrayOf(x, y))
}

@JvmInline
value class NGLIVec3 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int, z: Int) : this(intArrayOf(x, y, z))
}

@JvmInline
value class NGLIVec4 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int, z: Int, w: Int) : this(intArrayOf(x, y, z, w))
}

@JvmInline
value class NGLUVec2 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt) : this(intArrayOf(x.toInt(), y.toInt()))
}

@JvmInline
value class NGLUVec3 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt, z: UInt) : this(intArrayOf(x.toInt(), y.toInt(), z.toInt()))
}

@JvmInline
value class NGLUVec4 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt, z: UInt, w: UInt) : this(
        intArrayOf(
            x.toInt(),
            y.toInt(),
            z.toInt(),
            w.toInt()
        )
    )
}

class NGLRational(val den: Int, val num: Int)
class NGLData(val buffer: ByteBuffer, val size: Long)
typealias NGLMat4 = FloatArray
