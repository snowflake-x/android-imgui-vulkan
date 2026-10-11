package dev.snowflake.vulkan

import android.app.ActivityManager
import android.app.NativeActivity
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.WindowManager
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * 提供 NativeActivity 宿主、设备信息和受限的异步 root 诊断。
 * 公开报告与操作方法由 C++ 通过 JNI 按名称及签名调用，变更时必须同步更新调用方。
 */
class MainActivity : NativeActivity() {
    @Volatile
    private var rootReport = "Not requested. Tap the button to request su authorization."
    private val rootBusy = AtomicBoolean(false)

    @Volatile
    private var rootProcess: Process? = null

    @Volatile
    private var destroyed = false

    /**
     * 创建宿主并在前台保持屏幕点亮。
     * @param state 系统保存的状态，首次启动时为 null。
     */
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    /**
     * 由原生渲染线程定期读取系统信息，不保存可变 Android 对象。
     * @return 设备、屏幕与内存报告；DPI 等数值以系统报告为准。
     */
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

    /** @return 后台线程最近发布的诊断快照，不等待命令完成。 */
    fun getRootReport(): String = rootReport

    /**
     * 读取前台服务发布的快照，渲染线程不会执行 shell 或传感器采样。
     */
    fun getHudReport(): String = HudService.latestNativeReport

    /**
     * 从插件的显式用户动作打开持久化 HUD 配置。
     */
    fun openHudSettings() {
        runOnUiThread {
            if (!destroyed) startActivity(Intent(this, HudSettingsActivity::class.java))
        }
    }

    /**
     * 用户主动启动宿主管理的监控会话；缺少悬浮权限时进入配置页。
     * 会话可跨 Surface 销毁和前后台切换，在停止按钮或通知动作中结束。
     */
    fun startPerformanceHud() {
        runOnUiThread {
            if (!destroyed && !HudService.start(this)) {
                startActivity(Intent(this, HudSettingsActivity::class.java))
            }
        }
    }

    /**
     * 显式停止独立于渲染表面存活的 HUD 会话。
     */
    fun stopPerformanceHud() {
        runOnUiThread { HudService.stop(applicationContext) }
    }

    /**
     * 缓存 Vulkan 实测硬件元数据，使应用进入后台后 HUD 仍可显示。
     * @param report GPU 名称、原始驱动版本和 Vulkan API 版本，按换行分隔。
     */
    fun publishGpuInfo(report: String) {
        val fields = report.split('\n', limit = 3)
        if (fields.size != 3) return
        getSharedPreferences("performance_hud", MODE_PRIVATE).edit()
            .putString("gpu_model", fields[0])
            .putString("gpu_driver", fields[1])
            .putString("gpu_api", fields[2])
            .apply()
    }

    /**
     * 将原生错误投递到 UI 线程显示，然后关闭 Activity。
     * @param message 面向用户的错误说明。
     */
    fun showError(message: String) {
        runOnUiThread {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show()
            finish()
        }
    }

    /**
     * 异步请求已有 su 提供者授权并执行固定只读诊断，可从渲染线程调用。
     * 已销毁或正在执行时忽略请求；命令等待最多 30 秒，保留最多 32 KiB 输出。
     * 达到输出上限后仍排空管道，避免子进程因管道写满而阻塞。
     */
    fun requestRootInfo() {
        if (destroyed || !rootBusy.compareAndSet(false, true)) return
        rootReport = "Waiting for su authorization (30 second timeout)..."
        thread(name = "root-info", isDaemon = true) {
            var process: Process? = null
            try {
                // 仅执行固定的只读命令，不接受外部输入或修改系统。
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
                        // 进程终止可能关闭流；保留已读取的输出。
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

    /**
     * 阻止新诊断任务并终止当前 su 进程，再释放 Activity。
     * 后台任务通过 destroyed 标记处理与进程创建并发的销毁操作。
     */
    override fun onDestroy() {
        destroyed = true
        rootProcess?.destroyForcibly()
        super.onDestroy()
    }
}
