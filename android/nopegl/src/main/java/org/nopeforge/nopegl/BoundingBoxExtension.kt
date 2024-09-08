package org.nopeforge.nopegl

import android.graphics.RectF

fun BoundingBox.toRectF(rectF: RectF = RectF()): RectF {
    return rectF.apply {
        set(
            centerX - extentWidth,
            centerY - extentHeight,
            centerX + extentWidth,
            centerY + extentHeight,
        )
    }
}
