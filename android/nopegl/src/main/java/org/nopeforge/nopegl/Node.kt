package org.nopeforge.nopegl

import java.nio.ByteBuffer

class Node(private var nativePtr: Long, private var stealRef: Boolean = false) {

    init {
        if (!stealRef) {
            nativeRef(nativePtr)
        }
    }

    fun finalize() {
        nativeUnref(nativePtr)
        nativePtr = 0
    }

    fun setBoolean(key: String, value: Boolean) : Boolean {
		return nativeSetBoolean(nativePtr, key, value) == 0
	}
    fun setData(key: String, size: Long, data: ByteBuffer) : Boolean {
		return nativeSetData(nativePtr, key, size, data) == 0
	}
    fun setDict(key: String, name: String, value: Long) : Boolean {
		return nativeSetDict(nativePtr, key, name, value) == 0
	}
    fun setFloat(key: String, value: Float) : Boolean {
		return nativeSetFloat(nativePtr, key, value) == 0
	}
    fun setDouble(key: String, value: Double) : Boolean {
		return nativeSetDouble(nativePtr, key, value) == 0
	}
    fun setFlags(key: String, value: String) : Boolean {
		return nativeSetFlags(nativePtr, key, value) == 0
	}
    fun setInt(key: String, value: Int) : Boolean {
		return nativeSetInt(nativePtr, key, value) == 0
	}
    fun setIVec2(key: String, value: IntArray) : Boolean {
		return nativeSetIVec2(nativePtr, key, value) == 0
	}
    fun setIVec3(key: String, value: IntArray) : Boolean {
		return nativeSetIVec3(nativePtr, key, value) == 0
	}
    fun setIVec4(key: String, value: IntArray) : Boolean {
		return nativeSetIVec4(nativePtr, key, value) == 0
	}
    fun setMat4(key: String, value: FloatArray) : Boolean {
		return nativeSetMat4(nativePtr, key, value) == 0
	}
    fun setNode(key: String, value: Long) : Boolean {
		return nativeSetNode(nativePtr, key, value) == 0
	}
    fun setRational(key: String, num: Int, den: Int) : Boolean {
		return nativeSetRational(nativePtr, key, num, den) == 0
	}
    fun setSelect(key: String, value: String) : Boolean {
		return nativeSetSelect(nativePtr, key, value) == 0
	}
    fun setString(key: String, value: String) : Boolean {
		return nativeSetString(nativePtr, key, value) == 0
	}
    fun setUInt(key: String, value: Int) : Boolean {
		return nativeSetUInt(nativePtr, key, value) == 0
	}
    fun setUVec2(key: String, value: IntArray) : Boolean {
		return nativeSetUVec2(nativePtr, key, value) == 0
	}
    fun setUVec3(key: String, value: IntArray) : Boolean {
		return nativeSetUVec3(nativePtr, key, value) == 0
	}
    fun setUVec4(key: String, value: IntArray) : Boolean {
		return nativeSetUVec4(nativePtr, key, value) == 0
	}
    fun setVec2(key: String, value: FloatArray) : Boolean {
		return nativeSetVec2(nativePtr, key, value) == 0
	}
    fun setVec3(key: String, value: FloatArray) : Boolean {
		return nativeSetVec3(nativePtr, key, value) == 0
	}
    fun setVec4(key: String, value: FloatArray) : Boolean {
		return nativeSetVec4(nativePtr, key, value) == 0
	}

    private external fun nativeRef(nativePtr: Long)
    private external fun nativeUnref(nativePtr: Long)
    private external fun nativeSetBoolean(nativePtr: Long, key: String, value: Boolean) : Int
    private external fun nativeSetData(nativePtr: Long, key: String, size: Long, data: ByteBuffer) : Int
    private external fun nativeSetDict(nativePtr: Long, key: String, name: String, value: Long) : Int
    private external fun nativeSetFloat(nativePtr: Long, key: String, value: Float) : Int
    private external fun nativeSetDouble(nativePtr: Long, key: String, value: Double) : Int
    private external fun nativeSetFlags(nativePtr: Long, key: String, value: String) : Int
    private external fun nativeSetInt(nativePtr: Long, key: String, value: Int) : Int
    private external fun nativeSetIVec2(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetIVec3(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetIVec4(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetMat4(nativePtr: Long, key: String, value: FloatArray) : Int
    private external fun nativeSetNode(nativePtr: Long, key: String, value: Long) : Int
    private external fun nativeSetRational(nativePtr: Long, key: String, num: Int, den: Int) : Int
    private external fun nativeSetSelect(nativePtr: Long, key: String, value: String) : Int
    private external fun nativeSetString(nativePtr: Long, key: String, value: String) : Int
    private external fun nativeSetUInt(nativePtr: Long, key: String, value: Int) : Int
    private external fun nativeSetUVec2(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetUVec3(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetUVec4(nativePtr: Long, key: String, value: IntArray) : Int
    private external fun nativeSetVec2(nativePtr: Long, key: String, value: FloatArray) : Int
    private external fun nativeSetVec3(nativePtr: Long, key: String, value: FloatArray) : Int
    private external fun nativeSetVec4(nativePtr: Long, key: String, value: FloatArray) : Int
}