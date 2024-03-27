package org.nopeforge.nopegl

import java.nio.ByteBuffer


open class Node(
    val nativePtr: Long,
    label: String? = null,
) {
    constructor(nodeType: NodeType) : this(nativeCreate(nodeType.type))

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

    fun setBoolean(key: String, value: Boolean): Boolean {
        return nativeSetBoolean(nativePtr, key, value) == 0
    }

    fun setData(key: String, data: Data): Boolean {
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

    fun setIVec2(key: String, value: IVec2): Boolean {
        return nativeSetIVec2(nativePtr, key, value.array) == 0
    }

    fun setIVec3(key: String, value: IVec3): Boolean {
        return nativeSetIVec3(nativePtr, key, value.array) == 0
    }

    fun setIVec4(key: String, value: IVec4): Boolean {
        return nativeSetIVec4(nativePtr, key, value.array) == 0
    }

    fun setMat4(key: String, value: Mat4): Boolean {
        return nativeSetMat4(nativePtr, key, value) == 0
    }

    fun setNode(key: String, value: Long): Boolean {
        return nativeSetNode(nativePtr, key, value) == 0
    }

    fun addNodes(key: String, nodes: List<Node>): Boolean {
        val nodePointers = nodes.map { it.nativePtr }.toLongArray()
        return nativeAddNodes(nativePtr, key, nodes.size, nodePointers) == 0
    }

    fun setRational(key: String, rational: Rational): Boolean {
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

    fun setUVec2(key: String, value: UVec2): Boolean {
        return nativeSetUVec2(nativePtr, key, value.array) == 0
    }

    fun setUVec3(key: String, value: UVec3): Boolean {
        return nativeSetUVec3(nativePtr, key, value.array) == 0
    }

    fun setUVec4(key: String, value: UVec4): Boolean {
        return nativeSetUVec4(nativePtr, key, value.array) == 0
    }

    fun setVec2(key: String, value: Vec2): Boolean {
        return nativeSetVec2(nativePtr, key, value.array) == 0
    }

    fun setVec3(key: String, value: Vec3): Boolean {
        return nativeSetVec3(nativePtr, key, value.array) == 0
    }

    fun setVec4(key: String, value: Vec4): Boolean {
        return nativeSetVec4(nativePtr, key, value.array) == 0
    }

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

@JvmInline
value class Vec2 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float) : this(floatArrayOf(x, y))
}

@JvmInline
value class Vec3 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float, z: Float) : this(floatArrayOf(x, y, z))
}

@JvmInline
value class Vec4 private constructor(val array: FloatArray) {
    constructor(x: Float, y: Float, z: Float, w: Float) : this(floatArrayOf(x, y, z, w))
}

@JvmInline
value class IVec2 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int) : this(intArrayOf(x, y))
}

@JvmInline
value class IVec3 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int, z: Int) : this(intArrayOf(x, y, z))
}

@JvmInline
value class IVec4 private constructor(val array: IntArray) {
    constructor(x: Int, y: Int, z: Int, w: Int) : this(intArrayOf(x, y, z, w))
}

@JvmInline
value class UVec2 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt) : this(intArrayOf(x.toInt(), y.toInt()))
}

@JvmInline
value class UVec3 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt, z: UInt) : this(intArrayOf(x.toInt(), y.toInt(), z.toInt()))
}

@JvmInline
value class UVec4 private constructor(val array: IntArray) {
    constructor(x: UInt, y: UInt, z: UInt, w: UInt) : this(
        intArrayOf(
            x.toInt(),
            y.toInt(),
            z.toInt(),
            w.toInt()
        )
    )
}

class Rational(val den: Int, val num: Int)
class Data(val buffer: ByteBuffer, val size: Long)

typealias Mat4 = FloatArray

private fun fourCharacters(a: Char, b: Char, c: Char, d: Char): Int {
    return (a.code shl 24) or (b.code shl 16) or (c.code shl 8) or d.code
}

sealed interface NodeOrValue<T : Any> {
    data class Node<T : Any>(val value: org.nopeforge.nopegl.Node) : NodeOrValue<T>
    data class Value<T : Any>(val value: T) : NodeOrValue<T>

    companion object {
        fun <T : Any> node(value: org.nopeforge.nopegl.Node): NodeOrValue<T> =
            Node(value)

        fun <T : Any> value(value: T): NodeOrValue<T> = Value(value)
    }
}

enum class NodeType(val type: Int) {
    ANIMATEDBUFFERFLOAT(fourCharacters('A', 'B', 'f', '1')),
    ANIMATEDBUFFERVEC2(fourCharacters('A', 'B', 'f', '2')),
    ANIMATEDBUFFERVEC3(fourCharacters('A', 'B', 'f', '3')),
    ANIMATEDBUFFERVEC4(fourCharacters('A', 'B', 'f', '4')),
    ANIMATEDCOLOR(fourCharacters('A', 'n', 'm', 'c')),
    ANIMATEDPATH(fourCharacters('A', 'n', 'm', 'P')),
    ANIMATEDTIME(fourCharacters('A', 'n', 'm', 'T')),
    ANIMATEDFLOAT(fourCharacters('A', 'n', 'm', '1')),
    ANIMATEDVEC2(fourCharacters('A', 'n', 'm', '2')),
    ANIMATEDVEC3(fourCharacters('A', 'n', 'm', '3')),
    ANIMATEDVEC4(fourCharacters('A', 'n', 'm', '4')),
    ANIMATEDQUAT(fourCharacters('A', 'n', 'm', 'Q')),
    ANIMKEYFRAMEBUFFER(fourCharacters('A', 'K', 'F', 'B')),
    ANIMKEYFRAMEFLOAT(fourCharacters('A', 'K', 'F', '1')),
    ANIMKEYFRAMEVEC2(fourCharacters('A', 'K', 'F', '2')),
    ANIMKEYFRAMEVEC3(fourCharacters('A', 'K', 'F', '3')),
    ANIMKEYFRAMEVEC4(fourCharacters('A', 'K', 'F', '4')),
    ANIMKEYFRAMEQUAT(fourCharacters('A', 'K', 'F', 'Q')),
    ANIMKEYFRAMECOLOR(fourCharacters('A', 'K', 'F', 'c')),
    BLOCK(fourCharacters('B', 'l', 'c', 'k')),
    BUFFERBYTE(fourCharacters('B', 's', 'b', '1')),
    BUFFERBVEC2(fourCharacters('B', 's', 'b', '2')),
    BUFFERBVEC3(fourCharacters('B', 's', 'b', '3')),
    BUFFERBVEC4(fourCharacters('B', 's', 'b', '4')),
    BUFFERINT(fourCharacters('B', 's', 'i', '1')),
    BUFFERINT64(fourCharacters('B', 's', 'l', '1')),
    BUFFERIVEC2(fourCharacters('B', 's', 'i', '2')),
    BUFFERIVEC3(fourCharacters('B', 's', 'i', '3')),
    BUFFERIVEC4(fourCharacters('B', 's', 'i', '4')),
    BUFFERSHORT(fourCharacters('B', 's', 's', '1')),
    BUFFERSVEC2(fourCharacters('B', 's', 's', '2')),
    BUFFERSVEC3(fourCharacters('B', 's', 's', '3')),
    BUFFERSVEC4(fourCharacters('B', 's', 's', '4')),
    BUFFERUBYTE(fourCharacters('B', 'u', 'b', '1')),
    BUFFERUBVEC2(fourCharacters('B', 'u', 'b', '2')),
    BUFFERUBVEC3(fourCharacters('B', 'u', 'b', '3')),
    BUFFERUBVEC4(fourCharacters('B', 'u', 'b', '4')),
    BUFFERUINT(fourCharacters('B', 'u', 'i', '1')),
    BUFFERUIVEC2(fourCharacters('B', 'u', 'i', '2')),
    BUFFERUIVEC3(fourCharacters('B', 'u', 'i', '3')),
    BUFFERUIVEC4(fourCharacters('B', 'u', 'i', '4')),
    BUFFERUSHORT(fourCharacters('B', 'u', 's', '1')),
    BUFFERUSVEC2(fourCharacters('B', 'u', 's', '2')),
    BUFFERUSVEC3(fourCharacters('B', 'u', 's', '3')),
    BUFFERUSVEC4(fourCharacters('B', 'u', 's', '4')),
    BUFFERFLOAT(fourCharacters('B', 'f', 'v', '1')),
    BUFFERVEC2(fourCharacters('B', 'f', 'v', '2')),
    BUFFERVEC3(fourCharacters('B', 'f', 'v', '3')),
    BUFFERVEC4(fourCharacters('B', 'f', 'v', '4')),
    BUFFERMAT4(fourCharacters('B', 'f', 'm', '4')),
    CAMERA(fourCharacters('C', 'm', 'r', 'a')),
    CIRCLE(fourCharacters('C', 'r', 'c', 'l')),
    COLORKEY(fourCharacters('C', 'K', 'e', 'y')),
    COLORSTATS(fourCharacters('C', 'l', 'r', 'S')),
    COMPUTE(fourCharacters('C', 'p', 't', ' ')),
    COMPUTEPROGRAM(fourCharacters('C', 'p', 't', 'P')),
    DRAW(fourCharacters('D', 'r', 'a', 'w')),
    DRAWCOLOR(fourCharacters('D', 'c', 'l', 'r')),
    DRAWDISPLACE(fourCharacters('D', 'd', 's', 'p')),
    DRAWGRADIENT(fourCharacters('D', 'g', 'r', 'd')),
    DRAWGRADIENT4(fourCharacters('D', 'g', 'd', '4')),
    DRAWHISTOGRAM(fourCharacters('D', 'h', 's', 't')),
    DRAWNOISE(fourCharacters('D', 'N', 'o', 'i')),
    DRAWPATH(fourCharacters('D', 'P', 't', 'h')),
    DRAWTEXTURE(fourCharacters('D', 't', 'e', 'x')),
    DRAWWAVEFORM(fourCharacters('D', 'w', 'f', 'm')),
    EVALFLOAT(fourCharacters('E', 'v', 'f', '1')),
    EVALVEC2(fourCharacters('E', 'v', 'f', '2')),
    EVALVEC3(fourCharacters('E', 'v', 'f', '3')),
    EVALVEC4(fourCharacters('E', 'v', 'f', '4')),
    FILTERALPHA(fourCharacters('F', 'a', 'l', 'f')),
    FILTERCOLORMAP(fourCharacters('F', 'm', 'a', 'p')),
    FILTERCONTRAST(fourCharacters('F', 'c', 't', 'r')),
    FILTEREXPOSURE(fourCharacters('F', 'e', 'x', 'p')),
    FILTERINVERSEALPHA(fourCharacters('F', '1', '-', 'a')),
    FILTERLINEAR2SRGB(fourCharacters('F', 'r', 'g', 'b')),
    FILTEROPACITY(fourCharacters('F', 'o', 'p', 'a')),
    FILTERPREMULT(fourCharacters('F', 'p', 'r', 'e')),
    FILTERSATURATION(fourCharacters('F', 's', 'a', 't')),
    FILTERSELECTOR(fourCharacters('F', 'S', 'e', 'l')),
    FILTERSRGB2LINEAR(fourCharacters('F', 'l', 'i', 'n')),
    FASTGAUSSIANBLUR(fourCharacters('F', 'G', 'B', 'l')),
    FONTFACE(fourCharacters('F', 'o', 'n', 't')),
    GAUSSIANBLUR(fourCharacters('G', 'B', 'l', 'r')),
    GEOMETRY(fourCharacters('G', 'e', 'o', 'm')),
    GRAPHICCONFIG(fourCharacters('G', 'r', 'C', 'f')),
    GRIDLAYOUT(fourCharacters('G', 'r', 'd', 'L')),
    GROUP(fourCharacters('G', 'r', 'p', ' ')),
    IDENTITY(fourCharacters('I', 'd', ' ', ' ')),
    IOINT(fourCharacters('I', 'O', 'i', '1')),
    IOIVEC2(fourCharacters('I', 'O', 'i', '2')),
    IOIVEC3(fourCharacters('I', 'O', 'i', '3')),
    IOIVEC4(fourCharacters('I', 'O', 'i', '4')),
    IOUINT(fourCharacters('I', 'O', 'u', '1')),
    IOUIVEC2(fourCharacters('I', 'O', 'u', '2')),
    IOUIVEC3(fourCharacters('I', 'O', 'u', '3')),
    IOUIVEC4(fourCharacters('I', 'O', 'u', '4')),
    IOFLOAT(fourCharacters('I', 'O', 'f', '1')),
    IOVEC2(fourCharacters('I', 'O', 'f', '2')),
    IOVEC3(fourCharacters('I', 'O', 'f', '3')),
    IOVEC4(fourCharacters('I', 'O', 'f', '4')),
    IOMAT3(fourCharacters('I', 'O', 'm', '3')),
    IOMAT4(fourCharacters('I', 'O', 'm', '4')),
    IOBOOL(fourCharacters('I', 'O', 'b', '1')),
    MEDIA(fourCharacters('M', 'd', 'i', 'a')),
    NOISEFLOAT(fourCharacters('N', 'z', 'f', '1')),
    NOISEVEC2(fourCharacters('N', 'z', 'f', '2')),
    NOISEVEC3(fourCharacters('N', 'z', 'f', '3')),
    NOISEVEC4(fourCharacters('N', 'z', 'f', '4')),
    PATH(fourCharacters('P', 'a', 't', 'h')),
    PATHKEYBEZIER2(fourCharacters('P', 'h', 'K', '2')),
    PATHKEYBEZIER3(fourCharacters('P', 'h', 'K', '3')),
    PATHKEYCLOSE(fourCharacters('P', 'h', 'C', 'l')),
    PATHKEYLINE(fourCharacters('P', 'h', 'K', '1')),
    PATHKEYMOVE(fourCharacters('P', 'h', 'K', '0')),
    PROGRAM(fourCharacters('P', 'r', 'g', 'm')),
    QUAD(fourCharacters('Q', 'u', 'a', 'd')),
    RENDERTOTEXTURE(fourCharacters('R', 'T', 'T', ' ')),
    RESOURCEPROPS(fourCharacters('R', 'e', 's', 'P')),
    ROTATE(fourCharacters('T', 'R', 'o', 't')),
    ROTATEQUAT(fourCharacters('T', 'R', 'o', 'Q')),
    SCALE(fourCharacters('T', 's', 'c', 'l')),
    SKEW(fourCharacters('T', 's', 'k', 'w')),
    SMOOTHPATH(fourCharacters('S', 'P', 't', 'h')),
    STREAMEDINT(fourCharacters('S', 't', 'i', '1')),
    STREAMEDIVEC2(fourCharacters('S', 't', 'i', '2')),
    STREAMEDIVEC3(fourCharacters('S', 't', 'i', '3')),
    STREAMEDIVEC4(fourCharacters('S', 't', 'i', '4')),
    STREAMEDUINT(fourCharacters('S', 't', 'u', '1')),
    STREAMEDUIVEC2(fourCharacters('S', 't', 'u', '2')),
    STREAMEDUIVEC3(fourCharacters('S', 't', 'u', '3')),
    STREAMEDUIVEC4(fourCharacters('S', 't', 'u', '4')),
    STREAMEDFLOAT(fourCharacters('S', 't', 'f', '1')),
    STREAMEDVEC2(fourCharacters('S', 't', 'f', '2')),
    STREAMEDVEC3(fourCharacters('S', 't', 'f', '3')),
    STREAMEDVEC4(fourCharacters('S', 't', 'f', '4')),
    STREAMEDMAT4(fourCharacters('S', 't', 'm', '4')),
    STREAMEDBUFFERINT(fourCharacters('S', 'B', 'i', '1')),
    STREAMEDBUFFERIVEC2(fourCharacters('S', 'B', 'i', '2')),
    STREAMEDBUFFERIVEC3(fourCharacters('S', 'B', 'i', '3')),
    STREAMEDBUFFERIVEC4(fourCharacters('S', 'B', 'i', '4')),
    STREAMEDBUFFERUINT(fourCharacters('S', 'B', 'u', '1')),
    STREAMEDBUFFERUIVEC2(fourCharacters('S', 'B', 'u', '2')),
    STREAMEDBUFFERUIVEC3(fourCharacters('S', 'B', 'u', '3')),
    STREAMEDBUFFERUIVEC4(fourCharacters('S', 'B', 'u', '4')),
    STREAMEDBUFFERFLOAT(fourCharacters('S', 'B', 'f', '1')),
    STREAMEDBUFFERVEC2(fourCharacters('S', 'B', 'f', '2')),
    STREAMEDBUFFERVEC3(fourCharacters('S', 'B', 'f', '3')),
    STREAMEDBUFFERVEC4(fourCharacters('S', 'B', 'f', '4')),
    STREAMEDBUFFERMAT4(fourCharacters('S', 'B', 'm', '4')),
    TEXT(fourCharacters('T', 'e', 'x', 't')),
    TEXTEFFECT(fourCharacters('T', 'x', 'F', 'X')),
    TEXTURE2D(fourCharacters('T', 'e', 'x', '2')),
    TEXTURE2DARRAY(fourCharacters('T', 'e', '2', 'A')),
    TEXTURE3D(fourCharacters('T', 'e', 'x', '3')),
    TEXTURECUBE(fourCharacters('T', 'e', 'x', 'C')),
    TEXTUREVIEW(fourCharacters('T', 'e', 'x', 'V')),
    TIME(fourCharacters('T', 'i', 'm', 'e')),
    TIMERANGEFILTER(fourCharacters('T', 'R', 'F', 'l')),
    TRANSFORM(fourCharacters('T', 'r', 'f', 'm')),
    TRANSLATE(fourCharacters('T', 'm', 'o', 'v')),
    TRIANGLE(fourCharacters('T', 'r', 'g', 'l')),
    UNIFORMBOOL(fourCharacters('U', 'n', 'b', '1')),
    UNIFORMINT(fourCharacters('U', 'n', 'i', '1')),
    UNIFORMIVEC2(fourCharacters('U', 'n', 'i', '2')),
    UNIFORMIVEC3(fourCharacters('U', 'n', 'i', '3')),
    UNIFORMIVEC4(fourCharacters('U', 'n', 'i', '4')),
    UNIFORMUINT(fourCharacters('U', 'n', 'u', '1')),
    UNIFORMUIVEC2(fourCharacters('U', 'n', 'u', '2')),
    UNIFORMUIVEC3(fourCharacters('U', 'n', 'u', '3')),
    UNIFORMUIVEC4(fourCharacters('U', 'n', 'u', '4')),
    UNIFORMMAT4(fourCharacters('U', 'n', 'M', '4')),
    UNIFORMFLOAT(fourCharacters('U', 'n', 'f', '1')),
    UNIFORMVEC2(fourCharacters('U', 'n', 'f', '2')),
    UNIFORMVEC3(fourCharacters('U', 'n', 'f', '3')),
    UNIFORMVEC4(fourCharacters('U', 'n', 'f', '4')),
    UNIFORMCOLOR(fourCharacters('U', 'n', 'c', '3')),
    UNIFORMQUAT(fourCharacters('U', 'n', 'Q', 't')),
    USERSELECT(fourCharacters('U', 'S', 'e', 'l')),
    USERSWITCH(fourCharacters('U', 'S', 'c', 'h')),
    VELOCITYFLOAT(fourCharacters('V', 'l', 'y', '1')),
    VELOCITYVEC2(fourCharacters('V', 'l', 'y', '2')),
    VELOCITYVEC3(fourCharacters('V', 'l', 'y', '3')),
    VELOCITYVEC4(fourCharacters('V', 'l', 'y', '4'));
}