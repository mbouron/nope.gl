package org.nopeforge.nopegl

import android.content.ContentResolver
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ProviderInfo
import android.graphics.PointF
import android.test.mock.MockContentResolver
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import kotlin.math.abs


class MockContext(private val contentResolver: ContentResolver, base: Context?) :
    ContextWrapper(base) {
    override fun getContentResolver(): ContentResolver {
        return contentResolver
    }
}

@RunWith(AndroidJUnit4::class)
class NopeGLTest {
    private fun createContext(backend: Int): NGLContext {
        val config = NGLConfig.Builder()
            .setBackend(backend)
            .setOffscreen(true)
            .setWidth(256)
            .setHeight(256)
            .setClearColor(floatArrayOf(1.0F, 1.0F, 0.0F, 1.0F))
            .build()
        val ctx = NGLContext()
        val ret = ctx.configure(config)
        assertEquals(ret, 0)

        return ctx
    }

    private fun offscreenCtx(backend: Int) {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val ctx = createContext(backend)

        var ret = ctx.draw(0.0)
        assertEquals(ret, 0)

        ret = ctx.resize(256, 256)
        assert(ret != 0)

        val captureBuffer = ByteBuffer.allocateDirect(256 * 256 * 4)
        ret = ctx.setCaptureBuffer(captureBuffer)
        assertEquals(ret, 0)

        ret = ctx.draw(0.0)
        assertEquals(ret, 0)

        val buffer = captureBuffer.asIntBuffer()
        assertEquals((0xFFFF00FF).toUInt(), buffer[0].toUInt())

        ctx.release()
    }

    @Test
    fun offscreenOpenGLCtx() {
        offscreenCtx(NGLConfig.BACKEND_OPENGLES)
    }

    @Test
    fun offscreenVulkanCtx() {
        offscreenCtx(NGLConfig.BACKEND_VULKAN)
    }

    private fun checkAsset(applicationContext: Context, name: String): Boolean {
        return File(getAssetPath(applicationContext, name)).exists()
    }

    private fun copyAsset(applicationContext: Context, name: String) {
        val inputStream = applicationContext.resources.assets.open(name)
        inputStream.copyTo(FileOutputStream(getAssetPath(applicationContext, name)))
    }

    private fun getAssetPath(applicationContext: Context, name: String): String {
        return applicationContext.cacheDir!!.path + "/" + name
    }

    @Before
    fun initAssets() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        if (!checkAsset(context, "cat.mp4"))
            copyAsset(context, "cat.mp4")
    }

    @Test
    fun loadMediaScene() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext

        val providerInfo = ProviderInfo()
        providerInfo.authority = NGLContentProvider.AUTHORITY

        val contentProvider = NGLContentProvider()
        contentProvider.attachInfo(appContext, providerInfo)

        val contentResolver = MockContentResolver(appContext)
        contentResolver.addProvider(NGLContentProvider.AUTHORITY, contentProvider)

        val fakeCtx = MockContext(contentResolver, appContext)
        NGLContext.init(fakeCtx)

        val scene = NGLScene(
            """
            # Nope.GL v0.11.0
            # duration=403Z9000000000000
            # aspect_ratio=320/240
            # framerate=60/1
            Mdia filename:content://%s/medias%s
            Tex2 data_src:1
            Quad
            Dtex texture:2 geometry:1
        """.trimIndent().format(
                NGLContentProvider.AUTHORITY,
                getAssetPath(appContext, "cat.mp4")
            )
        )
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES)
        ctx.setScene(scene)
        var i = 0
        val nbFrames = 10
        while (i < nbFrames) {
            ctx.draw(i * 1.0 / 60.0)
            i++
        }

        ctx.release()
    }

    @Test
    fun craftMediaScene() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext

        val providerInfo = ProviderInfo()
        providerInfo.authority = NGLContentProvider.AUTHORITY

        val contentProvider = NGLContentProvider()
        contentProvider.attachInfo(appContext, providerInfo)

        val contentResolver = MockContentResolver(appContext)
        contentResolver.addProvider(NGLContentProvider.AUTHORITY, contentProvider)

        val fakeCtx = MockContext(contentResolver, appContext)
        NGLContext.init(fakeCtx)

        val duration = 10.0
        val keyFrames = listOf(
            NGLAnimKeyFrameFloat(0.0, 0.0),
            NGLAnimKeyFrameFloat(duration, duration),
        )
        val mediaNode = NGLMedia(
            filename = "content://%s/medias%s".format(
                NGLContentProvider.AUTHORITY,
                getAssetPath(appContext, "cat.mp4")
            ),
            timeAnim = NGLAnimatedTime(keyFrames),
        )
        val texture2DNode = NGLTexture2D(
            dataSrc = mediaNode,
            minFilter = NGLFilter.Nearest,
            magFilter = NGLFilter.Nearest
        )
        val transform = NGLTransform(
            child = texture2DNode,
            matrix = NGLNodeOrValue.node(NGLUniformMat4(liveId = "reframing_matrix"))
        )
        val draw = NGLDrawTexture(transform)
        val transform2 = NGLTransform(
            child = draw,
            matrix = NGLNodeOrValue.node(NGLUniformMat4(liveId = "geometry_matrix"))
        )
        val timeRangeFilter = NGLTimeRangeFilter(
            child = transform2,
            start = 1.0,
            end = 2.0,
            prefetchTime = 3.0,
        )
        val group = NGLGroup(listOf(timeRangeFilter))

        val scene = NGLScene(rootNode = group, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }

        var i = 0
        val nbFrames = 10
        while (i < nbFrames) {
            ctx.draw(i * 1.0 / 60.0)
            i++
        }

        ctx.release()
    }

    @Test
    fun noErrorOnSetLiveParam() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val duration = 10.0
        val draw = NGLDrawColor()
        val group = NGLGroup(listOf(draw))

        val scene = NGLScene(rootNode = group, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }
        draw.setOpacity(NGLNodeOrValue.value(0.5f))

        ctx.release()
    }

    @Test
    fun liveControls() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val scene = NGLScene(
            """
            # Nope.GL v0.11.0
            # duration=402Z4000000000000
            # aspect_ratio=16/9
            # framerate=60/1
            Unc3 value:7Fz0,7Fz0,7Fz0 live_id:color
            Dclr color:!1
            AKFQ quat:0z0,0z0,0z0,7Fz0
            AKFQ time:400Z0 quat:0z0,0z0,-7Ez3504F3,7Ez3504F3
            AKFQ time:401Z0 quat:0z0,7Fz0,0z0,0z0
            AKFQ time:401Z8000000000000 quat:7Fz0,0z0,0z0,0z0
            AKFQ time:402Z0 quat:7Ez3504F3,0z0,0z0,7Ez3504F3
            AKFQ time:402Z4000000000000 quat:0z0,0z0,0z0,7Fz0
            AnmQ keyframes:6,5,4,3,2,1 as_mat4:1
            Trfm child:8 matrix:!1
            UnM4 live_id:matrix
            Trfm child:2 matrix:!1
            Cmra child:1 eye:0z0,0z0,81z0 center:0z0,0z0,0z0 perspective:84z340000,7Fz638E39 clipping:7Fz0,82z200000 label:quaternion
        """.trimIndent()
        )
        val color = scene.getLiveControl("color")!!
        color.setVec3("value", NGLVec3(1f, 1f, 1f))

        val matrix = scene.getLiveControl("matrix")!!
        matrix.setMat4(
            "value", floatArrayOf(
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
                0f, 0f, 1f, 0f,
                0f, 0f, 1f, 1f,
            )
        )
    }

    @Test
    fun customShaders() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val vertex = """
void main() {
    ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * vec4(ngl_position, 1.0);
    var_tex0_coord = (tex0_coord_matrix * vec4(ngl_uvcoord, 0.0, 1.0)).xy;
}
"""
        val fragment = """
void main() {
    ngl_out_color = ngl_texvideo(tex0, var_tex0_coord);
}
"""

        val filename = "content://%s/medias%s".format(
            NGLContentProvider.AUTHORITY,
            getAssetPath(appContext, "cat.mp4")
        )
        val media = NGLMedia(filename)
        val texture = NGLTexture2D(dataSrc = media)
        val quad = NGLQuad(NGLVec3(-1f, -1f, 0f), NGLVec3(2f, 0f, 0f), NGLVec3(0f, 2f, 0f))
        val program = NGLProgram(vertex = vertex, fragment = fragment)
        program.setVertOutVars(mapOf("var_tex0_coord" to NGLIOVec2()))
        val draw = NGLDraw(quad, program)
        draw.setFragResources(mapOf("tex0" to texture))
        val scene = NGLScene(rootNode = draw, duration = 2.0)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }

        val ret = ctx.draw(0.0)
        assertEquals(ret, 0)

        ctx.release()
    }

    private fun allocate(count: Int) {
        var i = 0
        while (i < count) {
            val group = NGLGroup(listOf(
                NGLDrawTexture(NGLTexture2D(width=1U, height = 1U)),
                NGLDrawColor(),
                NGLDrawGradient(
                    color0 = NGLNodeOrValue.value(NGLVec3(0f, 0f, 0f)),
                    color1 = NGLNodeOrValue.value(NGLVec3(1f, 1f, 1f)),
                )
            ))
            NGLScene(group)
            i++
        }
    }

    @Test
    fun memoryManagement() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val ctx = createContext(NGLConfig.BACKEND_OPENGLES)

        allocate(20000)

        var i = 0
        while (i < 10000) {
            allocate(1)
            Thread.sleep(1)
            System.gc()
            i++
        }

        ctx.release()
    }

    @Test
    fun mediaLiveChanges() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext

        val providerInfo = ProviderInfo()
        providerInfo.authority = NGLContentProvider.AUTHORITY

        val contentProvider = NGLContentProvider()
        contentProvider.attachInfo(appContext, providerInfo)

        val contentResolver = MockContentResolver(appContext)
        contentResolver.addProvider(NGLContentProvider.AUTHORITY, contentProvider)

        val fakeCtx = MockContext(contentResolver, appContext)
        NGLContext.init(fakeCtx)

        val duration = 10.0
        val startKf = NGLAnimKeyFrameFloat(0.0, 0.0)
        val endKf = NGLAnimKeyFrameFloat(duration, duration)
        val keyFrames = listOf(startKf, endKf)
        val mediaNode = NGLMedia(
            filename = "content://%s/medias%s".format(
                NGLContentProvider.AUTHORITY,
                getAssetPath(appContext, "cat.mp4")
            ),
            timeAnim = NGLAnimatedTime(keyFrames),
        )
        val texture = NGLTexture2D(dataSrc = mediaNode)
        val draw = NGLDrawTexture(texture)
        val scene = NGLScene(rootNode = draw, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }

        ctx.draw(0.0)
        for (i in 1..5) {
            startKf.setValue(i.toDouble())
            ctx.draw(i.toDouble())
        }

        ctx.release()
    }
}
