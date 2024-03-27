package org.nopeforge.nopegl

import android.content.ContentResolver
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ProviderInfo
import android.test.mock.MockContentResolver
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.nopeforge.nopegl.Config
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import org.nopeforge.nopegl.Config as NGLConfig
import org.nopeforge.nopegl.Context as NGLContext
import org.nopeforge.nopegl.Scene as NGLScene


class MockContext(private val contentResolver: ContentResolver, base: Context?) :
    ContextWrapper(base) {
    override fun getContentResolver(): ContentResolver {
        return contentResolver
    }
}

@RunWith(AndroidJUnit4::class)
class NopeGLTest {
    private fun createContext(backend: Int): NGLContext {
        val config = NGLConfig()
        config.backend = backend
        config.offscreen = true
        config.width = 256
        config.height = 256
        config.clearColor = floatArrayOf(1.0F, 1.0F, 0.0F, 1.0F)

        val ctx = NGLContext()
        val ret = ctx.configure(config)
        assertEquals(ret, 0)

        return ctx
    }

    private fun offscreenCtx(backend: Int) {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.nativeInit(appContext)

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

        ctx.finalize()
    }

    @Test
    fun offscreenOpenGLCtx() {
        offscreenCtx(Config.BACKEND_OPENGLES)
    }

    @Test
    fun offscreenVulkanCtx() {
        offscreenCtx(Config.BACKEND_VULKAN)
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
        NGLContext.nativeInit(fakeCtx)

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
        val ctx = createContext(Config.BACKEND_OPENGLES)
        ctx.setScene(scene)
        var i = 0
        val nbFrames = 10
        while (i < nbFrames) {
            ctx.draw(i * 1.0 / 60.0)
            i++
        }

        ctx.finalize()
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
        NGLContext.nativeInit(fakeCtx)

        val duration = 403.9
        val keyFrames = listOf(
            AnimKeyFrameFloat(0.0, 0.0),
            AnimKeyFrameFloat(duration, duration),
        )
        val mediaNode = Media(
            filename = "content://%s/medias%s".format(
                NGLContentProvider.AUTHORITY,
                getAssetPath(appContext, "cat.mp4")
            ),
            timeAnim = AnimatedTime(keyFrames),
        )
        val texture2DNode =
            Texture2D(dataSrc = mediaNode, minFilter = Filter.Nearest, magFilter = Filter.Nearest)
        val transform = Transform(
            child = texture2DNode,
            matrix = NodeOrValue.node(UniformMat4(liveId = "reframing_matrix"))
        )
        val draw = DrawTexture(transform)
        val transform2 = Transform(
            child = draw,
            matrix = NodeOrValue.node(UniformMat4(liveId = "geometry_matrix"))
        )
        val timeRangeFilter = TimeRangeFilter(
            child = transform2,
            start = 1.0,
            end = 2.0,
            prefetchTime = 3.0,
        )


        val scene = NGLScene(rootNode = timeRangeFilter, duration = duration)
        val ctx = createContext(Config.BACKEND_OPENGLES)
        ctx.setScene(scene)
        var i = 0
        val nbFrames = 10
        while (i < nbFrames) {
            ctx.draw(i * 1.0 / 60.0)
            i++
        }

        ctx.finalize()

        println(scene.serialize())
    }

    @Test
    fun liveControls() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.nativeInit(appContext)

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
        var ret = color.setVec3("value", Vec3(1f, 1f, 1f))
        assertEquals(ret, true)

        val matrix = scene.getLiveControl("matrix")!!
        ret = matrix.setMat4(
            "value", floatArrayOf(
                1f, 0f, 0f, 0f,
                0f, 1f, 0f, 0f,
                0f, 0f, 1f, 0f,
                0f, 0f, 1f, 1f,
            )
        )
        assertEquals(ret, true)
    }

}