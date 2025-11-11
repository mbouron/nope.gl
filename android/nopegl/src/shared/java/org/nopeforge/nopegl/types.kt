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
