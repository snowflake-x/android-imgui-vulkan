package dev.snowflake.vulkan

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/** Kotlin owns the manager UI; service surfaces, monitoring, root workers and HUD rendering remain native. */
class MainActivity : ComponentActivity() {
    private val model: ManagerViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val state = model.state.collectAsStateWithLifecycle().value
            ManagerApp(state, model::change, model::diagnostics, ::hudAction)
        }
    }

    override fun onStart() { super.onStart(); model.visible(true) }
    override fun onStop() { model.visible(false); super.onStop() }

    /** Android permission and service actions only; this method never samples a metric or invokes su. */
    fun hudAction(action: Int) {
        if (isDestroyed) return
        try {
            when (action) {
                0 -> startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
                1 -> if (Settings.canDrawOverlays(this)) startForegroundService(Intent(this, HudService::class.java)) else hudAction(0)
                2 -> stopService(Intent(this, HudService::class.java))
                4 -> if (Build.VERSION.SDK_INT >= 33) requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
            }
        } catch (error: RuntimeException) { model.error(error.message ?: error.toString()) }
    }
}
