package org.nopeforge.nopegl

import android.content.ContentResolver
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ProviderInfo
import android.graphics.Canvas
import android.graphics.Color
import android.test.mock.MockContentResolver
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.math.abs
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer


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
            # canvas=320x240
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
    fun errorOnSetNotLiveParam() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val duration = 10.0
        val draw = NGLDrawColor()
        val group = NGLGroup(listOf(draw))

        val scene = NGLScene(rootNode = group, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
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
    fun nodeLabel() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val duration = 10.0
        val label = "color-label"
        val draw = NGLDrawColor(label = label)
        val group = NGLGroup(listOf(draw))

        val scene = NGLScene(rootNode = group, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }
        assertEquals(label, draw.getLabel())

        ctx.release()
    }

    @Test
    fun nodeType() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val duration = 10.0
        val draw = NGLDrawColor()
        val group = NGLGroup(listOf(draw))

        val scene = NGLScene(rootNode = group, duration = duration)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            setScene(scene)
        }
        assertEquals(NGLNodeType.DRAWCOLOR, draw.getType())
        assertEquals(NGLNodeType.GROUP, group.getType())

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
            # canvas=1280x720
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
        """.trimIndent()

        val fragment = """
            void main() {
                ngl_out_color = ngl_texvideo(tex0, var_tex0_coord);
            }
        """.trimIndent()

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
            val callback = object : NGLCustomTexture.Callback() {
                override fun init() {}
                override fun prepare() {}
                override fun prefetch() {}
                override fun update(time: Double) {}
                override fun draw() {}
                override fun release() {}
                override fun uninit() {}
            }
            val customTexture = NGLCustomTexture(callback)
            val group = NGLGroup(listOf(
                NGLDrawTexture(customTexture),
                NGLDrawTexture(NGLTexture2D(width = 1U, height = 1U)),
                NGLDrawColor(),
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

    private fun isClose(a: Float, b: Float, tol: Float = 1.0f): Boolean {
        return abs(a - b) <= tol
    }

    @Test
    fun globalTransformGetters() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        val rect = NGLDrawRect(rect = NGLVec4(0.0f, 0.0f, 100.0f, 80.0f), fill = fill, label = "rect")
        val group = NGLGroup2D(
            children = listOf(rect),
            translate = NGLVec2(50.0f, 30.0f),
            rotation = 45.0f,
            scale = NGLVec2(2.0f, 0.5f),
            anchor = NGLVec2(0.0f, 0.0f),
            label = "group",
        )
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }
        ctx.draw(0.0)

        // Group2D position
        val pos = group.getGlobalPosition()
        assertNotNull(pos)
        assertTrue("position_x", isClose(pos!![0], 50.0f))
        assertTrue("position_y", isClose(pos[1], 30.0f))

        // Group2D rotation
        val rot = group.getGlobalRotation()
        assertTrue("rotation", isClose(rot, 45.0f))

        // Group2D scale
        val scl = group.getGlobalScale()
        assertNotNull(scl)
        assertTrue("scale_x", isClose(scl!![0] * 10, 20.0f))
        assertTrue("scale_y", isClose(scl[1] * 10, 5.0f))

        // Transform matrix
        val matrix = group.getGlobalTransformMatrix()
        assertNotNull(matrix)
        assertEquals(16, matrix!!.size)

        ctx.release()
    }

    @Test
    fun globalRotationStability() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        val rect = NGLDrawRect(rect = NGLVec4(0.0f, 0.0f, 100.0f, 80.0f), fill = fill)
        val group = NGLGroup2D(children = listOf(rect), anchor = NGLVec2(0.0f, 0.0f))
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }

        for (angle in -180..180 step 15) {
            group.setRotation(angle.toFloat())
            ctx.draw(0.0)

            val rotation = group.getGlobalRotation()
            val diff = ((rotation - angle + 180) % 360) - 180
            assertTrue(
                "angle=$angle: got rotation=$rotation (diff=$diff)",
                abs(diff) < 0.1f
            )
        }

        ctx.release()
    }

    @Test
    fun boundingBox() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        val rect = NGLDrawRect(rect = NGLVec4(0.0f, 0.0f, 100.0f, 80.0f), fill = fill, label = "rect")
        val group = NGLGroup2D(children = listOf(rect), translate = NGLVec2(50.0f, 30.0f), label = "group")
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }
        ctx.draw(0.0)

        // DrawRect: translated by (50,30), center=(100,70), extent=(50,40)
        val box = rect.getBoundingBox()
        assertNotNull(box)
        assertTrue("center_x", isClose(box!!.centerX, 100.0f))
        assertTrue("center_y", isClose(box.centerY, 70.0f))
        assertTrue("extent_w", isClose(box.extentWidth, 50.0f))
        assertTrue("extent_h", isClose(box.extentHeight, 40.0f))

        // Group2D should have the same AABB
        val gbox = group.getBoundingBox()
        assertNotNull(gbox)
        assertTrue("group center_x", isClose(gbox!!.centerX, 100.0f))
        assertTrue("group extent_w", isClose(gbox.extentWidth, 50.0f))

        ctx.release()
    }

    @Test
    fun boundingBoxBeforeDraw() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        val rect = NGLDrawRect(rect = NGLVec4(0.0f, 0.0f, 100.0f, 80.0f), fill = fill)
        val group = NGLGroup2D(children = listOf(rect), translate = NGLVec2(50.0f, 30.0f))
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        // Before set_scene: should return zeros without crashing
        val boxBefore = rect.getBoundingBox()
        assertNotNull(boxBefore)
        assertTrue("before: center_x", isClose(boxBefore!!.centerX, 0.0f))
        assertTrue("before: extent_w", isClose(boxBefore.extentWidth, 0.0f))

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }

        // After set_scene but before draw: still zeros
        val boxAfterScene = rect.getBoundingBox()
        assertNotNull(boxAfterScene)
        assertTrue("after scene: center_x", isClose(boxAfterScene!!.centerX, 0.0f))

        ctx.draw(0.0)

        // After draw: rect (0,0,100,80) translated by (50,30) → center=(100,70), extent=(50,40)
        val box = rect.getBoundingBox()
        assertNotNull(box)
        assertTrue("center_x", isClose(box!!.centerX, 100.0f))
        assertTrue("center_y", isClose(box.centerY, 70.0f))
        assertTrue("extent_w", isClose(box.extentWidth, 50.0f))
        assertTrue("extent_h", isClose(box.extentHeight, 40.0f))

        val gbox = group.getBoundingBox()
        assertNotNull(gbox)
        assertTrue("group center_x", isClose(gbox!!.centerX, 100.0f))
        assertTrue("group center_y", isClose(gbox.centerY, 70.0f))
        assertTrue("group extent_w", isClose(gbox.extentWidth, 50.0f))
        assertTrue("group extent_h", isClose(gbox.extentHeight, 40.0f))

        val pos = group.getGlobalPosition()
        assertNotNull(pos)
        assertTrue("position_x", isClose(pos!![0], 50.0f))
        assertTrue("position_y", isClose(pos[1], 30.0f))

        ctx.release()
    }

    @Test
    fun boundingBoxMultipleChildren() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        // r0 at (10,20,40,30) → center=(30,35), extent=(20,15), spans x=[10,50], y=[20,50]
        // r1 at (100,80,60,40) → center=(130,100), extent=(30,20), spans x=[100,160], y=[80,120]
        val r0 = NGLDrawRect(rect = NGLVec4(10.0f, 20.0f, 40.0f, 30.0f), fill = fill)
        val r1 = NGLDrawRect(rect = NGLVec4(100.0f, 80.0f, 60.0f, 40.0f), fill = fill)
        val group = NGLGroup2D(children = listOf(r0, r1))
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }
        ctx.draw(0.0)

        val box0 = r0.getBoundingBox()
        assertNotNull(box0)
        assertTrue("r0 center_x", isClose(box0!!.centerX, 30.0f))
        assertTrue("r0 center_y", isClose(box0.centerY, 35.0f))
        assertTrue("r0 extent_w", isClose(box0.extentWidth, 20.0f))
        assertTrue("r0 extent_h", isClose(box0.extentHeight, 15.0f))

        val box1 = r1.getBoundingBox()
        assertNotNull(box1)
        assertTrue("r1 center_x", isClose(box1!!.centerX, 130.0f))
        assertTrue("r1 center_y", isClose(box1.centerY, 100.0f))
        assertTrue("r1 extent_w", isClose(box1.extentWidth, 30.0f))
        assertTrue("r1 extent_h", isClose(box1.extentHeight, 20.0f))

        // Union: x=[10,160] → center_x=85, extent_x=75; y=[20,120] → center_y=70, extent_y=50
        val gbox = group.getBoundingBox()
        assertNotNull(gbox)
        assertTrue("group center_x", isClose(gbox!!.centerX, 85.0f))
        assertTrue("group center_y", isClose(gbox.centerY, 70.0f))
        assertTrue("group extent_w", isClose(gbox.extentWidth, 75.0f))
        assertTrue("group extent_h", isClose(gbox.extentHeight, 50.0f))

        val cbox = canvas.getBoundingBox()
        assertNotNull(cbox)
        assertTrue("canvas center_x", isClose(cbox!!.centerX, 85.0f))
        assertTrue("canvas center_y", isClose(cbox.centerY, 70.0f))
        assertTrue("canvas extent_w", isClose(cbox.extentWidth, 75.0f))
        assertTrue("canvas extent_h", isClose(cbox.extentHeight, 50.0f))

        ctx.release()
    }

    @Test
    fun boundingBoxRotation() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val fill = NGLColorFill(color = NGLVec4(1.0f, 0.0f, 0.0f, 1.0f))
        // 200x100 rect rotated 90° around its center (100, 50)
        val rect = NGLDrawRect(rect = NGLVec4(0.0f, 0.0f, 200.0f, 100.0f), fill = fill)
        val group = NGLGroup2D(children = listOf(rect), rotation = 90.0f, anchor = NGLVec2(100.0f, 50.0f))
        val canvas = NGLCanvas(children = listOf(group), width = width, height = height)

        val scene = NGLScene(rootNode = canvas, width = width, height = height)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply { setScene(scene) }
        ctx.draw(0.0)

        // After 90° rotation: AABB swaps width/height → center=(100,50), extent=(50,100)
        val box = rect.getBoundingBox()
        assertNotNull(box)
        assertTrue("center_x", isClose(box!!.centerX, 100.0f))
        assertTrue("center_y", isClose(box.centerY, 50.0f))
        assertTrue("extent_w", isClose(box.extentWidth, 50.0f))
        assertTrue("extent_h", isClose(box.extentHeight, 100.0f))

        val gbox = group.getBoundingBox()
        assertNotNull(gbox)
        assertTrue("group center_x", isClose(gbox!!.centerX, 100.0f))
        assertTrue("group extent_w", isClose(gbox.extentWidth, 50.0f))
        assertTrue("group extent_h", isClose(gbox.extentHeight, 100.0f))

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

    @Test
    fun canvas() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        NGLContext.init(appContext)

        val width = 256
        val height = 256
        val canvas = NGLAndroidCanvas(
            width = width,
            height = height,
            callback = object: NGLAndroidCanvas.Callback {
                override fun onDraw(canvas: Canvas) {
                    canvas.drawColor(Color.WHITE)
                    super.onDraw(canvas)
                }
            },
            tag = "canvas",
        )

        val group = NGLGroup(
            children = listOf(
                NGLDrawTexture(canvas.node)
            )
        )

        val scene = NGLScene(rootNode = group, duration = 2.0)

        val captureBuffer = ByteBuffer.allocateDirect(width * height * 4)
        val ctx = createContext(NGLConfig.BACKEND_OPENGLES).apply {
            val ret = setCaptureBuffer(captureBuffer)
            assertEquals(ret, 0)
            setScene(scene)
        }

        val ret = ctx.draw(0.0)
        assertEquals(ret, 0)

        val buffer = captureBuffer.asIntBuffer()
        assertEquals((0xFFFFFFFF).toUInt(), buffer[0].toUInt())

        ctx.release()
    }
}
