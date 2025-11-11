package org.nopeforge.nopegl

data class OrientedBoundingBox(
    val boundingBox: BoundingBox,
    val rotation: Float,
) {
    constructor(
        centerX: Float,
        centerY: Float,
        extentWidth: Float,
        extentHeight: Float,
        rotation: Float,
    ) : this(
        boundingBox = BoundingBox(
            centerX = centerX,
            centerY = centerY,
            extentWidth = extentWidth,
            extentHeight = extentHeight
        ),
        rotation = rotation,
    )
}
