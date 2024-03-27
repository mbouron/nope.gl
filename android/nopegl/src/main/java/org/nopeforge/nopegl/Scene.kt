package org.nopeforge.nopegl

class Scene {
    constructor(serializedScene: String) {
        nativePtr = nativeInitFromString(serializedScene)
        if (nativePtr == 0L)
            throw Exception()

        val ret = nativeAddLiveControls(nativePtr)
        if (ret < 0)
            throw Exception()
    }

    constructor(
        rootNode: Node,
        duration: Double = 0.0,
        framerate: Rational = Rational(num = 60, den = 1),
        aspectRatio: Rational = Rational(num = 1, den = 1),
    ) {
        nativePtr = nativeCreateScene(
            nodePtr = rootNode.nativePtr,
            duration = duration,
            framerateNum = framerate.num,
            framerateDen = framerate.den,
            aspectRatioNum = aspectRatio.num,
            aspectRatioDen = aspectRatio.den
        )
        if (nativePtr == 0L)
            throw Exception()
        val ret = nativeAddLiveControls(nativePtr)
        if (ret < 0)
            throw Exception()
    }


    var nativePtr: Long = 0
    private var liveControls: MutableMap<String, Node> = mutableMapOf()

    fun serialize(): String {
        return nativeSerialize(nativePtr)
    }

    fun finalize() {
        nativeRelease(nativePtr)
        nativePtr = 0
    }

    private fun addLiveControl(id: String, nativePtr: Long) {
        liveControls[id] = Node(nativePtr, true)
    }

    fun getLiveControl(id: String): Node? {
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