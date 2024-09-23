package org.nopeforge.nopegl

abstract class NGLCustomTexture : NGLNode(NGLNodeType.CUSTOMTEXTURE) {
    private val nativeContextPtr: Long

    init {
        nativeContextPtr = nativeCustomTextureCreate(nativePtr)
    }

    protected abstract fun init()
    protected abstract fun prepare()
    protected abstract fun prefetch()
    protected abstract fun update(time: Double)
    protected abstract fun draw()
    protected abstract fun release()
    protected abstract fun uninit()

    protected fun setTextureInfo(info: Info?) {
        if (info == null || info.texture == 0) {
            nativeCustomTextureSetTextureInfo(nativeContextPtr)
        } else {
            nativeCustomTextureSetTextureInfo(
                nativeContextPtr,
                info.texture,
                info.target,
                info.width,
                info.height,
            )
        }
    }

    private external fun nativeCustomTextureCreate(nativePtr: Long): Long
    private external fun nativeCustomTextureSetTextureInfo(
        nativeContextPtr: Long,
        texture: Int = 0,
        target: Int = 0,
        width: Int = 0,
        height: Int = 0,
    ): Int

    private external fun nativeCustomTextureRelease(nativeContextPtr: Long)

    data class Info(
        val width: Int,
        val height: Int,
        val texture: Int,
        val target: Int,
    )
}
