package dev.snowflake.vulkan

import androidx.activity.ComponentActivity
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.basic.Scaffold

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "en-rUS-w411dp-h891dp-xhdpi")
class MemoryScannerPageTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun explicitTargetSearchAndViewCommands() {
        val commands = mutableListOf<Pair<String, String>>()
        val data = JSONObject("""{"rootAccess":"granted","config":{"managerPlugins":15},"memory":{"status":"idle","processes":[{"pid":123,"start":"456","uid":10123,"name":"com.example.game"}]}}""")
        compose.setContent {
            MiuixTheme {
                Scaffold { padding ->
                    MemoryScannerPage(ManagerState(data, loading = false), { key, value -> commands += key to value },
                        false, Modifier, padding)
                }
            }
        }
        compose.onNodeWithText("Refresh running app processes").performClick()
        assertEquals("memory:list" to "", commands.last())
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Target process"))
        compose.onNodeWithText("Select a process").performClick()
        compose.onNodeWithText("com.example.game · PID 123 · u0").performClick()
        compose.onNode(hasScrollAction()).performScrollToNode(hasContentDescription("Search value"))
        compose.onNodeWithContentDescription("Search value").performTextInput("42")
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Scan memory"))
        compose.onNodeWithText("Scan memory").performClick()
        assertEquals("memory:scan" to "123 456 utf8 3432", commands.last())
        compose.onNode(hasScrollAction()).performScrollToNode(hasContentDescription("Memory address"))
        compose.onNodeWithContentDescription("Memory address").performTextInput("0x1000")
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Read / refresh memory"))
        compose.onNodeWithText("Read / refresh memory").performClick()
        assertEquals("memory:view" to "123 456 0x1000 256", commands.last())
    }
}
