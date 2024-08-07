package org.nopeforge.nopegl

import android.graphics.RectF

fun BoundingBox.toRectF(): RectF {
    return RectF(
        centerX - extentWidth,
        centerY - extentHeight,
        centerX + extentWidth,
        centerY + extentHeight,
    )
}
