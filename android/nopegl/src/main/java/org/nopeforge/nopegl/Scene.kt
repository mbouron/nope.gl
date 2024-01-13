package org.nopeforge.nopegl

import java.lang.Exception

class Scene(scene: String) {
    var nativePtr: Long = 0
    private var liveControls : MutableMap<String, Node> = mutableMapOf()

    init {
        nativePtr = nativeInitFromString(scene)
        if (nativePtr == 0L)
            throw Exception()

        var ret = nativeAddLiveControls(nativePtr)
        if (ret < 0)
            throw Exception()
    }

    fun finalize() {
        nativeRelease(nativePtr)
        nativePtr = 0
    }

    private fun addLiveControl(id: String, nativePtr: Long) {
        liveControls[id] = Node(nativePtr, true)
    }

    fun getLiveControl(id: String) : Node? {
        return liveControls.getOrDefault(id, null)
    }

    private external fun nativeInitFromString(scene : String) : Long
    private external fun nativeAddLiveControls(nativePtr : Long) : Int
    private external fun nativeRelease(nativePtr: Long)

}