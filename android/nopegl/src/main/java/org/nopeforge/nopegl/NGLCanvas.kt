package org.nopeforge.nopegl

import android.graphics.Canvas
import android.graphics.RenderNode
import android.hardware.HardwareBuffer
import android.opengl.EGL14
import android.opengl.GLES11Ext.GL_TEXTURE_EXTERNAL_OES
import android.opengl.GLES32
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.graphics.CanvasBufferedRenderer
import androidx.opengl.EGLExt
import androidx.opengl.EGLImageKHR
import kotlinx.coroutines.runBlocking

@RequiresApi(Build.VERSION_CODES.Q)
class NGLCanvas(
    // TODO: don't expose the size here
    val width: Int,
    val height: Int,
    private val callback: Callback,
    tag: String? = null,
) {
    private var renderer: CanvasBufferedRenderer? = null
    private val renderNode = RenderNode(tag)
    private var texture: Int = 0
    var preserveContent: Boolean = true

    val node: NGLNode
        get() = customTextureNode

    private val customTextureNode: NGLCustomTexture = object : NGLCustomTexture() {
        override fun init() {
            renderNode.setPosition(0, 0, width, height)
            renderer = CanvasBufferedRenderer.Builder(width, height)
                .setBufferFormat(HardwareBuffer.RGBA_8888)
                .setUsageFlags(USAGE_FLAGS)
                .setMaxBuffers(1)
                .build()

            renderer?.setContentRoot(renderNode)
            callback.onInit()
        }

        override fun prepare() {
            /* no-op */
        }

        override fun prefetch() {
            GLES32.glGenTextures(1, values, 0)
            texture = values[0]
            GLES32.glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture)
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_MIN_FILTER,
                GLES32.GL_NEAREST,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_MAG_FILTER,
                GLES32.GL_LINEAR,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_WRAP_S,
                GLES32.GL_CLAMP_TO_EDGE,
            )
            GLES32.glTexParameteri(
                GL_TEXTURE_EXTERNAL_OES,
                GLES32.GL_TEXTURE_WRAP_T,
                GLES32.GL_CLAMP_TO_EDGE,
            )
            setTextureInfo(
                Info(
                    width = width,
                    height = height,
                    texture = texture,
                    target = GL_TEXTURE_EXTERNAL_OES
                )
            )
            callback.onPrefetch()
        }

        override fun update(time: Double) {
            renderer ?: return
            callback.onUpdate(time)
        }

        private var image: EGLImageKHR? = null
        private var hardwareBuffer: HardwareBuffer? = null

        override fun draw() {
            releaseHardwareBuffer()

            val renderer = renderer ?: return
            val canvas = renderNode.beginRecording()
            callback.onDraw(canvas)
            renderNode.endRecording()
            val request = renderer.obtainRenderRequest()
                .preserveContents(preserveContent)
            val result = request.drawBlocking()

            hardwareBuffer = result.hardwareBuffer
            val image = EGLExt.eglCreateImageFromHardwareBuffer(
                EGL14.eglGetCurrentDisplay(),
                result.hardwareBuffer
            ) ?: return
            GLES32.glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture)
            EGLExt.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image)
            GLES32.glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0)
            this.image = image
        }

        private fun CanvasBufferedRenderer.RenderRequest.drawBlocking(): CanvasBufferedRenderer.RenderResult {
            return runBlocking {
                draw(true)
            }
        }

        private fun releaseHardwareBuffer() {
            image?.let {
                EGLExt.eglDestroyImageKHR(EGL14.eglGetCurrentDisplay(), it)
                image = null
            }

            hardwareBuffer?.let {
                renderer?.releaseBuffer(it)
                hardwareBuffer = null
            }
        }

        override fun release() {
            callback.onRelease()
            GLES32.glDeleteTextures(1, intArrayOf(texture), 0)
            texture = 0
            releaseHardwareBuffer()
        }

        override fun uninit() {
            callback.onDestroy()
            renderer?.close()
            renderer = null
        }

    }

    interface Callback {
        fun onInit() = Unit
        fun onPrefetch() = Unit
        fun onUpdate(time: Double) = Unit
        fun onDraw(canvas: Canvas) = Unit
        fun onRelease() = Unit
        fun onDestroy() = Unit
    }

    companion object {
        private const val USAGE_FLAGS = HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE +
                HardwareBuffer.USAGE_GPU_COLOR_OUTPUT

        val values = IntArray(1)

    }
}
