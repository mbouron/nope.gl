package org.nopeforge.nopegl

import android.graphics.RectF

fun OrientedBoundingBox.toRectF(rectF: RectF = RectF()): RectF {
    return boundingBox.toRectF(rectF)
}
