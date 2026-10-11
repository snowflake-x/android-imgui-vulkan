package dev.snowflake.vulkan

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import java.util.concurrent.Executors

/** All manager JNI calls run on one process-owned worker; C++ owns collection and persistence. */
internal object NativeManager {
    init { System.loadLibrary("vulkanscope") }
    val dispatcher = Executors.newSingleThreadExecutor { task -> Thread(task, "NativeManager") }.asCoroutineDispatcher()
    val scope = CoroutineScope(SupervisorJob() + dispatcher)

    /** Attaches the application context and starts cancellable native startup work off the UI thread. */
    external fun initialize(context: Context, directory: String)
    /** Returns a bounded, standard UTF-8 snapshot. Does not run shell commands. */
    external fun snapshot(): ByteArray
    /** Updates one native field atomically or starts a separately owned root worker. */
    external fun command(key: String, value: String): Boolean
    /** Selects retained diagnostics by package and Android user, not by current foreground. */
    external fun diagnostic(packageName: String, user: Int): ByteArray
    /** Cancels and joins manager-only workers; the foreground HUD service remains running. */
    external fun pause()
}
