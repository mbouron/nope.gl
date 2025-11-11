package org.nopeforge.nopegl

import android.hardware.HardwareBuffer

class EGLHelper private constructor() {
    companion object {
        @JvmStatic
        external fun nativeCreateEGLImage(hardwareBuffer: HardwareBuffer) : Long
        @JvmStatic
        external fun nativeUploadEGLImage(image: Long, texture: Int)
        @JvmStatic
        external fun nativeReleaseEGLImage(image: Long)
    }
}
