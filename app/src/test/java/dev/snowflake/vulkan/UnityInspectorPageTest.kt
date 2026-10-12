package dev.snowflake.vulkan

import androidx.activity.ComponentActivity
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.theme.MiuixTheme

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "en-rUS-w411dp-h891dp-xhdpi")
class UnityInspectorPageTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @Test fun attachAndExplicitWriteUseTargetAndSession() {
        val commands = mutableListOf<Pair<String, String>>()
        fun state(report: JSONObject? = null): ManagerState {
            val data = JSONObject("""{"rootAccess":"granted","config":{"managerPlugins":15},"memory":{"processes":[{"pid":123,"start":"456","uid":10123,"name":"com.example.game"}]}}""")
            if (report != null) data.getJSONObject("memory").put("runtimeReport", report.toString())
            return ManagerState(data, loading = false)
        }
        val current = mutableStateOf(state())
        compose.setContent { MiuixTheme { Scaffold { padding ->
            UnityInspectorPage(current.value, { k, v -> commands += k to v }, false, Modifier, padding, "123:456") {}
        } } }
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Attach / reconnect"))
        compose.onNodeWithText("Attach / reconnect").performClick()
        fun request(): JSONObject {
            assertEquals("memory:runtime", commands.last().first)
            val args = commands.last().second.split(' ')
            assertEquals(listOf("123", "456", "json"), args.take(3))
            return JSONObject(args[3].chunked(2).map { it.toInt(16).toByte() }.toByteArray().toString(Charsets.UTF_8))
        }
        assertEquals("connect", request().getString("op"))
        compose.runOnIdle { current.value = state(JSONObject("""{"target":"123:456","session":"s1","kind":"members","fields":[{"id":"f1","name":"score","type":"System.Int32","flags":16,"offset":0}],"methods":[]}""")) }
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Field", substring = false))
        compose.onNodeWithText("Field", substring = false).performClick()
        compose.onNodeWithText("score · f1").performClick()
        compose.onNode(hasScrollAction()).performScrollToNode(hasContentDescription("Field value"))
        compose.onNodeWithContentDescription("Field value").performTextReplacement("42")
        compose.onNode(hasScrollAction()).performScrollToNode(hasText("Write field now"))
        compose.onNodeWithText("Write field now").performClick()
        assertEquals("write", request().getString("op"))
        assertEquals("s1", request().getString("session"))
        assertEquals("f1", request().getString("field"))
        assertEquals(42, request().getInt("value"))
    }
}
