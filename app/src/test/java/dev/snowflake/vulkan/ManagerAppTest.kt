package dev.snowflake.vulkan

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.activity.ComponentActivity
import android.graphics.Canvas
import org.json.JSONObject
import org.junit.Rule
import org.junit.Test
import org.junit.Assert.assertTrue
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import android.graphics.Bitmap
import java.io.File

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "zh-rCN-w411dp-h891dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ManagerAppTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun navigationAndNativeCommands() {
        val commands = mutableListOf<Pair<String, String>>()
        val data = JSONObject("""{"rootAccess":"granted","system":"Device: OnePlus PJZ110\nAndroid: 16\nDisplay: 3168 × 1440\nRefresh: 120 Hz","config":{"language":"zh-Hans","root":true,"autoRoot":true},"plugins":[{"id":"device-info","label":"设备信息","description":"系统屏幕和硬件信息","enabled":true},{"id":"gpu-info","label":"GPU 信息","description":"Vulkan 设备信息","enabled":true},{"id":"performance-hud","label":"性能显示","description":"游戏帧率与耗时监测","enabled":true}],"values":{"gpu_model":"Adreno (TM) 830","gpu_api":"1.3.284"}}""")
        compose.setContent { ManagerApp(ManagerState(data, loading = false), { key, value -> commands += key to value }, { _, _ -> }, {}) }
        compose.onAllNodesWithText("VulkanScope").onFirst().assertExists()
        val output = File("build/previews/miuix-home.png")
        output.parentFile!!.mkdirs()
        compose.runOnIdle {
            val view = compose.activity.window.decorView
            val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(bitmap))
            output.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
        compose.onNodeWithText("插件列表").performClick()
        compose.onNodeWithText("系统屏幕和硬件信息").assertExists()
        compose.onNodeWithText("设备信息").performClick()
        assertTrue(commands.contains("plugin:device-info" to "false"))
        compose.onNodeWithContentDescription("设置").performClick()
        compose.onNodeWithText("启动时自动申请 Root").assertExists()
    }
}
