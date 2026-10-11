package dev.snowflake.vulkan

import android.app.ActivityManager
import android.app.NativeActivity
import android.os.Build
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.WindowManager
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/** @brief 提供原生渲染层通过 JNI 调用的设备信息和 root 查询接口。 */
class MainActivity : NativeActivity() {
    @Volatile
    private var rootReport = "Not requested. Tap the button to request su authorization."
    private val rootBusy = AtomicBoolean(false)

    @Volatile
    private var rootProcess: Process? = null

    @Volatile
    private var destroyed = false

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    @Suppress("DEPRECATION")
    fun getSystemReport(): String {
        val display = windowManager.defaultDisplay
        val metrics = DisplayMetrics()
        display.getRealMetrics(metrics)
        val memory = ActivityManager.MemoryInfo()
        (getSystemService(ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(memory)
        return "Device: ${Build.MANUFACTURER} ${Build.MODEL}" +
            "\nAndroid: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})" +
            "\nABI: ${Build.SUPPORTED_ABIS.joinToString(", ")}" +
            "\nDisplay: ${metrics.widthPixels} x ${metrics.heightPixels} px" +
            "\nDensity: ${metrics.densityDpi} dpi / scale ${metrics.density}" +
            "\nPhysical DPI (reported): ${metrics.xdpi} x ${metrics.ydpi}" +
            "\nRefresh: ${display.refreshRate} Hz" +
            "\nRotation: ${display.rotation * 90} degrees" +
            "\nCPU cores: ${Runtime.getRuntime().availableProcessors()}" +
            "\nRAM available / total: ${memory.availMem / 1048576} / ${memory.totalMem / 1048576} MiB"
    }

    fun getRootReport(): String = rootReport

    fun showError(message: String) {
        runOnUiThread {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            finish()
        }
    }

    fun requestRootInfo() {
        if (destroyed || !rootBusy.compareAndSet(false, true)) return
        rootReport = "Waiting for su authorization (30 second timeout)..."
        thread(name = "root-info", isDaemon = true) {
            var process: Process? = null
            try {
                /** @brief 仅执行固定的只读命令，不接受外部输入或修改系统。 */
                val script = "id; echo '\n--- Kernel ---'; uname -a; " +
                    "echo '\n--- CPU ---'; cat /proc/cpuinfo; " +
                    "echo '\n--- Memory ---'; head -30 /proc/meminfo; " +
                    "echo '\n--- SELinux ---'; getenforce; " +
                    "echo '\n--- Thermal zones (raw units) ---'; " +
                    "for z in /sys/class/thermal/thermal_zone*; do " +
                    "[ -r \"\$z/type\" ] && cat \"\$z/type\"; [ -r \"\$z/temp\" ] && cat \"\$z/temp\"; done"
                val running = ProcessBuilder("su", "-c", script).redirectErrorStream(true).start()
                process = running
                rootProcess = running
                if (destroyed) {
                    running.destroyForcibly()
                    return@thread
                }
                val output = ByteArrayOutputStream()
                val reader = thread(name = "su-output", isDaemon = true) {
                    try {
                        running.inputStream.use { input ->
                            val bytes = ByteArray(2048)
                            while (true) {
                                val count = input.read(bytes)
                                if (count == -1) break
                                synchronized(output) {
                                    val remaining = 32768 - output.size()
                                    if (remaining > 0) output.write(bytes, 0, minOf(count, remaining))
                                }
                            }
                        }
                    } catch (_: Exception) {
                        /** @note 进程终止可能关闭流；保留已读取的输出。 */
                    }
                }
                val finished = running.waitFor(30, TimeUnit.SECONDS)
                if (!finished) running.destroyForcibly()
                reader.join(1000)
                val result = synchronized(output) { output.toByteArray().toString(Charsets.UTF_8) }
                rootReport = (if (finished) "su exit code: ${running.exitValue()}" else "Authorization / command timed out") +
                    "\n$result\n(Output limited to 32 KiB)"
            } catch (e: Exception) {
                rootReport = "su unavailable or denied: ${e.message}"
            } finally {
                process?.destroy()
                rootProcess = null
                rootBusy.set(false)
            }
        }
    }

    override fun onDestroy() {
        destroyed = true
        rootProcess?.destroyForcibly()
        super.onDestroy()
    }
}
