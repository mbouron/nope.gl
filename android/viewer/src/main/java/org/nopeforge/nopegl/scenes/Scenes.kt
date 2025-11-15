import org.nopeforge.nopegl.NGLAnimKeyFrameVec3
import org.nopeforge.nopegl.NGLAnimatedVec3
import org.nopeforge.nopegl.NGLDrawGradient4
import org.nopeforge.nopegl.NGLEasing
import org.nopeforge.nopegl.NGLEvalVec3
import org.nopeforge.nopegl.NGLGroup
import org.nopeforge.nopegl.NGLNode
import org.nopeforge.nopegl.NGLNodeOrValue
import org.nopeforge.nopegl.NGLQuad
import org.nopeforge.nopegl.NGLRotate
import org.nopeforge.nopegl.NGLScale
import org.nopeforge.nopegl.NGLScene
import org.nopeforge.nopegl.NGLText
import org.nopeforge.nopegl.NGLTranslate
import org.nopeforge.nopegl.NGLUniformColor
import org.nopeforge.nopegl.NGLUniformVec2
import org.nopeforge.nopegl.NGLVec2
import org.nopeforge.nopegl.NGLVec3
import org.nopeforge.nopegl.NGLVec4

fun defaultScene(): NGLScene {
    val duration = 5.0

    val gradient = NGLDrawGradient4(
        colorTl = NGLNodeOrValue.node(NGLUniformColor(_value = NGLVec3(1f, 0.5f, 0f))) ,
        colorTr = NGLNodeOrValue.node(NGLUniformColor(_value = NGLVec3(0f, 1f, 0f))) ,
        colorBr = NGLNodeOrValue.node(NGLUniformColor(_value = NGLVec3(0f, 0.5f, 1f))) ,
        colorBl = NGLNodeOrValue.node(NGLUniformColor(_value = NGLVec3(1f, 0f, 1f))) ,
        linear= NGLNodeOrValue.value(true),
        geometry=NGLQuad(),
    )

    val text = NGLText(
        "Hello",
        bgColor = NGLVec3(0f, 0f, 0f),
        bgOpacity = 0f,
        box= NGLVec4(-0.5f, -0.5f, 1.0f, 1.0f),
    )
    val group = NGLGroup(children = listOf(gradient, text))

    var scene: NGLNode = NGLRotate(group, angle = NGLNodeOrValue.value(15f))

    val scaleAnimkf = listOf(
        NGLAnimKeyFrameVec3(0.0, NGLVec3(0.7f, 0.7f, 0.7f)),
        NGLAnimKeyFrameVec3(duration / 2.0, NGLVec3(1.3f, 1.3f, 1.3f), NGLEasing.ExpOut),
        NGLAnimKeyFrameVec3(duration, NGLVec3(0.7f, 0.7f, 0.7f), NGLEasing.ExpIn),
    )

    scene = NGLScale(scene, factors = NGLNodeOrValue.node(NGLAnimatedVec3(scaleAnimkf)))

    val translate = NGLUniformVec2(
        liveId = "translate",
        liveMin = NGLVec2(-1.5f, -1.5f),
        liveMax = NGLVec2(1.5f, 1.5f)
    )
    val translate3 = NGLEvalVec3("t.x", "t.y", "0", resources = mapOf("t" to translate))

    scene = NGLTranslate(scene, vector = NGLNodeOrValue.node(translate3))
    return NGLScene(scene, duration = duration)
}
