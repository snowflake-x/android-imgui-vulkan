package dev.snowflake.vulkan

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

internal data class ManagerState(
    val data: JSONObject = JSONObject(),
    val loading: Boolean = true,
    val error: String? = null,
    val diagnostics: String = "",
) {
    val config: JSONObject get() = data.optJSONObject("config") ?: JSONObject()
    val running: Boolean get() = data.optBoolean("running")
}

/** Native calls are serialized; Compose observes immutable snapshots and never samples hardware. */
internal class ManagerViewModel(application: Application) : AndroidViewModel(application) {
    private val mutable = MutableStateFlow(ManagerState())
    val state = mutable.asStateFlow()
    private var visible = true
    private var ready = false
    private val refreshSignal = Channel<Unit>(Channel.CONFLATED)

    init {
        viewModelScope.launch {
            try {
                withContext(NativeManager.dispatcher) {
                    NativeManager.initialize(application, application.filesDir.absolutePath)
                }
                ready = true
                while (true) {
                    val start = android.os.SystemClock.uptimeMillis()
                    if (visible) refresh()
                    val interval = mutable.value.config.optInt("interval", 1000).coerceIn(250, 5000)
                    withTimeoutOrNull((interval - (android.os.SystemClock.uptimeMillis() - start)).coerceAtLeast(1)) { refreshSignal.receive() }
                }
            } catch (error: Exception) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                mutable.value = mutable.value.copy(loading = false, error = error.message)
            }
        }
    }

    fun visible(value: Boolean) { visible = value; refreshSignal.trySend(Unit) }

    private suspend fun refresh() {
        val snapshot = withContext(NativeManager.dispatcher) {
            JSONObject(NativeManager.snapshot().toString(Charsets.UTF_8)).put("liveUpdatesStatus", HudNotification.status(getApplication()))
        }
        mutable.value = mutable.value.copy(data = snapshot, loading = false)
    }

    fun change(key: String, value: String) {
        if (!ready) return
        viewModelScope.launch {
            try {
                val ok = withContext(NativeManager.dispatcher) { NativeManager.command(key, value) }
                if (!ok) throw IllegalStateException(if (key.startsWith("memory:")) "Cannot start operation: check root, target, input format, and running task" else "Unable to save this setting")
                mutable.value = mutable.value.copy(error = null)
                refresh()
                refreshSignal.trySend(Unit)
            } catch (error: Exception) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                mutable.value = mutable.value.copy(error = error.message)
            }
        }
    }

    fun diagnostics(packageName: String, user: Int) {
        viewModelScope.launch {
            try {
                val text = withContext(NativeManager.dispatcher) {
                    NativeManager.diagnostic(packageName, user).toString(Charsets.UTF_8)
                }
                mutable.value = mutable.value.copy(diagnostics = text)
            } catch (error: Exception) {
                if (error is kotlinx.coroutines.CancellationException) throw error
                mutable.value = mutable.value.copy(error = error.message)
            }
        }
    }

    fun error(message: String) { mutable.value = mutable.value.copy(error = message) }

    override fun onCleared() {
        NativeManager.scope.launch { NativeManager.pause() }
    }
}
