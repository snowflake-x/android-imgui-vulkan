package dev.snowflake.vulkan

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.TextView
import android.widget.Toast
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 用户启动的跨应用性能悬浮窗；原生渲染表面暂停后仍继续当前监控会话。
 *
 * 悬浮窗不可获取焦点或触摸，所有采集任务在同一工作线程串行执行。
 * 服务销毁会取消采集器与工作线程；Activity 销毁不结束用户明确启动的监控会话。
 */
class HudService : Service() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val stopped = AtomicBoolean(false)
    private var worker: ScheduledExecutorService? = null
    private var collectors: List<MetricCollector> = emptyList()
    private var overlay: TextView? = null
    private var layout: WindowManager.LayoutParams? = null
    private var appliedConfig: HudConfig? = null
    private var terminalFailure: String? = null
    private var samplingPaused = false
    private lateinit var windows: WindowManager

    /** 不提供绑定接口，服务只接受用户明确启动或停止的会话操作。 */
    override fun onBind(intent: Intent?): IBinder? = null

    /** 在主线程获取窗口管理器，尚不启动采集或申请权限。 */
    override fun onCreate() {
        super.onCreate()
        windows = getSystemService(WINDOW_SERVICE) as WindowManager
    }

    /**
     * 在主线程处理显式操作，创建前台通知与悬浮窗后异步启动串行采样。
     *
     * @return 不允许系统自动重启的 [START_NOT_STICKY]，确保采样由用户操作发起。
     */
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent?.action != ACTION_START) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (worker != null) return START_NOT_STICKY
        if (!Settings.canDrawOverlays(this)) {
            fail("请先在性能插件设置中授予悬浮窗权限")
            return START_NOT_STICKY
        }
        try {
            val notification = createNotification()
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            createOverlay(HudConfig.load(this))
            collectors = listOf(ForegroundFpsCollector(), HardwareCollector(this), EngineResolutionCollector(this))
            worker = Executors.newSingleThreadScheduledExecutor { task ->
                Thread(task, "performance-hud").apply { isDaemon = true }
            }
            running = true
            latestReport = "性能 HUD 已启动，等待采样…"
            latestNativeReport = "Performance HUD started; waiting for samples..."
            latestGameDiagnostics = ""
            latestDiagnostics = "${deviceDetails()}\nWaiting for the first sample; root authorization may be pending."
            scheduleNext(0)
        } catch (error: Exception) {
            fail("性能 HUD 无法启动：${error.message ?: error.javaClass.simpleName}")
        }
        return START_NOT_STICKY
    }

    private fun createNotification(): Notification {
        val manager = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "性能悬浮窗", NotificationManager.IMPORTANCE_LOW))
        val settings = PendingIntent.getActivity(
            this, 0, Intent(this, HudSettingsActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, HudService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("性能 HUD 正在运行")
            .setContentText("点击配置显示字段；停止按钮可结束采样")
            .setContentIntent(settings)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(Notification.Action.Builder(null, "停止", stop).build())
            .build()
    }

    private fun createOverlay(config: HudConfig) {
        val text = TextView(this).apply {
            setTextColor(Color.WHITE)
            typeface = Typeface.MONOSPACE
            setPadding(dp(10), dp(8), dp(10), dp(8))
            textSize = config.textSp.toFloat()
            text = "性能 HUD · 等待采样…"
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = cornerGravity(config.corner)
            x = dp(8)
            y = dp(36)
            /**
             * Android 12+ 按窗口 alpha 计算遮挡；上限 0.8 允许触摸穿透单个悬浮窗。
             */
            alpha = 0.8f
            title = "dev.snowflake.vulkan.PerformanceHUD"
        }
        overlay = text
        layout = params
        applyAppearance(config)
        windows.addView(text, params)
    }

    private fun scheduleNext(delay: Long) {
        val executor = worker ?: return
        if (stopped.get() || executor.isShutdown) return
        try {
            executor.schedule({ sample() }, delay, TimeUnit.MILLISECONDS)
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            /**
             * 停止可能与下一次采样调度同时发生。
             */
        }
    }

    private fun sample() {
        if (stopped.get()) return
        val config = HudConfig.load(this)
        if (!Settings.canDrawOverlays(this)) {
            mainHandler.post { fail("悬浮窗权限已撤销，性能 HUD 已停止") }
            return
        }
        val interactive = (getSystemService(POWER_SERVICE) as PowerManager).isInteractive
        if (!interactive) {
            samplingPaused = true
            mainHandler.post {
                if (!stopped.get()) {
                    latestReport = "屏幕关闭，性能采样已暂停"
                    latestNativeReport = "Screen off; sampling paused"
                    overlay?.visibility = View.GONE
                }
            }
            scheduleNext(config.intervalMs)
            return
        }
        if (samplingPaused) {
            collectors.filterIsInstance<ForegroundFpsCollector>().forEach { it.resetSampling() }
            samplingPaused = false
        }
        val snapshot = linkedMapOf<String, String>()
        collectors.forEach { collector ->
            if (stopped.get() || Thread.currentThread().isInterrupted) return
            try {
                snapshot.putAll(collector.sample(config, snapshot.toMap()))
            } catch (error: Exception) {
                if (stopped.get() || Thread.currentThread().isInterrupted) return
                snapshot["collector_error_${collector.javaClass.simpleName}"] = error.message ?: error.javaClass.simpleName
            }
        }
        if (stopped.get()) return
        val fields = config.order.filter { config.isEnabled(it) }
        val labels = HudConfig.METRICS.associate { it.id to it.label }
        val report = fields.joinToString("\n") { id ->
            "${labels[id] ?: id}: ${snapshot[id] ?: "不可用"}"
        }.ifBlank { "性能 HUD · 未选择显示字段" }
        val errors = snapshot.filterKeys { it.startsWith("collector_error_") }.values
        val fullReport = if (errors.isEmpty()) report else "$report\n采集错误: ${errors.joinToString("；")}"
        val nativeLabels = HudConfig.METRICS.associate { it.id to it.englishLabel }
        val nativeReport = fields.joinToString("\n") { id ->
            "${nativeLabels[id] ?: id}: ${asciiValue(snapshot[id] ?: "Unavailable")}"
        }.ifBlank { "Performance HUD: no fields selected" } +
            if (errors.isEmpty()) "" else "\nCollector error; see Android settings."
        val diagnostics = buildString {
            appendLine(deviceDetails())
            appendLine("Sample captured: ${java.time.OffsetDateTime.now()}")
            appendLine("Root opt-in: ${config.rootEnabled}; interval: ${config.intervalMs} ms")
            appendLine("These values belong to one completed sampling cycle.")
            snapshot.toSortedMap().forEach { (key, value) -> appendLine("$key: $value") }
            if (snapshot["engine_source"] == null) appendLine("Engine diagnostics: select an engine field to collect.")
            if (snapshot["fps_source"] == null) appendLine("Frame diagnostics: select an FPS field to collect.")
        }
        mainHandler.post {
            if (!stopped.get()) {
                latestReport = fullReport
                latestNativeReport = nativeReport
                latestDiagnostics = diagnostics
                if (snapshot["foreground"] in setOf("com.tencent.tmgp.sgame", "com.tencent.tmgp.pubgmhd") ||
                    snapshot["engine_source"].orEmpty().let { " / Unity / " in it || " / Unreal / " in it }) {
                    latestGameDiagnostics = diagnostics
                }
                try {
                    overlay?.visibility = View.VISIBLE
                    overlay?.text = report
                    if (appliedConfig != config) {
                        applyAppearance(config)
                        overlay?.let { windows.updateViewLayout(it, layout) }
                    }
                } catch (error: Exception) {
                    fail("性能悬浮窗不可用：${error.message ?: error.javaClass.simpleName}")
                }
            }
        }
        scheduleNext(config.intervalMs)
    }

    private fun applyAppearance(config: HudConfig) {
        overlay?.apply {
            textSize = config.textSp.toFloat()
            maxWidth = (resources.displayMetrics.widthPixels - dp(16)).coerceAtLeast(dp(100))
            val availableHeight = (resources.displayMetrics.heightPixels - dp(88)).coerceAtLeast(dp(60))
            maxHeight = availableHeight
            val estimatedLineHeight = textSize * 1.3f
            maxLines = ((availableHeight - dp(16)) / estimatedLineHeight).toInt().coerceIn(1, 100)
            ellipsize = TextUtils.TruncateAt.END
            background = GradientDrawable().apply {
                cornerRadius = dp(8).toFloat()
                setColor(Color.argb(config.opacity * 255 / 100, 12, 18, 25))
            }
        }
        layout?.gravity = cornerGravity(config.corner)
        appliedConfig = config
    }

    private fun cornerGravity(corner: String): Int = when (corner) {
        "top_right" -> Gravity.TOP or Gravity.RIGHT
        "bottom_left" -> Gravity.BOTTOM or Gravity.LEFT
        "bottom_right" -> Gravity.BOTTOM or Gravity.RIGHT
        else -> Gravity.TOP or Gravity.LEFT
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    /** 在主线程适配旋转后的可用屏幕尺寸，限制悬浮窗范围而不中断采集。 */
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        if (stopped.get() || overlay == null) return
        try {
            applyAppearance(HudConfig.load(this))
            overlay?.let { windows.updateViewLayout(it, layout) }
        } catch (error: Exception) {
            fail("性能悬浮窗不可用：${error.message ?: error.javaClass.simpleName}")
        }
    }

    private fun fail(reason: String) {
        if (stopped.get()) return
        terminalFailure = reason
        latestReport = reason
        latestNativeReport = "Performance HUD stopped: ${asciiValue(reason)}"
        Toast.makeText(this, reason, Toast.LENGTH_LONG).show()
        stopSelf()
    }

    /**
     * 在主线程取消外部采样和后续回调，移除悬浮窗及前台通知。
     *
     * 不等待工作线程退出；停止标记阻止正在返回的结果覆盖停止状态。
     */
    override fun onDestroy() {
        stopped.set(true)
        running = false
        collectors.forEach { collector -> runCatching { collector.close() } }
        collectors = emptyList()
        worker?.shutdownNow()
        worker = null
        mainHandler.removeCallbacksAndMessages(null)
        overlay?.let { runCatching { windows.removeViewImmediate(it) } }
        overlay = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        latestReport = terminalFailure ?: "性能 HUD 已停止"
        latestNativeReport = terminalFailure?.let { "Performance HUD stopped: ${asciiValue(it)}" } ?: "Performance HUD stopped"
        super.onDestroy()
    }

    companion object {
        /**
         * 保留最近一次游戏诊断，返回设置页时前台身份变化不会覆盖它。
         *
         * 时间戳保留采集时刻，HUD 实时字段始终使用当前应用快照；可跨线程读取。
         */
        @Volatile var latestGameDiagnostics: String = ""
            private set

        /** 用户从可见 Activity 发起的启动操作。 */
        const val ACTION_START = "dev.snowflake.vulkan.hud.START"
        /** 前台通知停止按钮发起的停止操作。 */
        const val ACTION_STOP = "dev.snowflake.vulkan.hud.STOP"
        private const val CHANNEL = "performance_hud"
        private const val NOTIFICATION_ID = 3001

        /** 服务是否已成功建立当前采样会话，可跨线程读取。 */
        @Volatile var running: Boolean = false
            private set
        /** 主线程发布的最新中文状态或完整已选字段快照，可跨线程读取。 */
        @Volatile var latestReport: String = "性能 HUD 未启动；在插件设置中选择字段并启动悬浮窗"
            private set
        /** 原生 ImGui 可显示的 ASCII 状态或字段快照，可跨线程读取。 */
        @Volatile var latestNativeReport: String = "Performance HUD stopped. Open settings to select metrics and start."
            private set
        /** 最近完整采样周期的设备、时间和全部已采集字段，可跨线程复制诊断。 */
        @Volatile var latestDiagnostics: String = "No performance sample captured yet. Start the HUD and switch to the game."
            private set

        private fun deviceDetails(): String =
            "Device: ${Build.MANUFACTURER} ${Build.MODEL}\n" +
                "Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n" +
                "ABI: ${Build.SUPPORTED_ABIS.joinToString(", ")}"

        /**
         * 原生 ImGui 默认字体不包含中文；完整诊断文本保留在 Android 设置页面。
         */
        private fun asciiValue(value: String): String {
            val normalized = value.replace("×", "x").replace("°", " deg ").replace("µ", "u")
                .replace("…", "...").replace("·", "|").replace("–", "-").replace("—", "-")
            return if (normalized.any { it.code > 127 }) "Unavailable (see Android HUD settings)" else normalized
        }

        /**
         * 由可见 Activity 在主线程响应用户操作时调用，异步启动前台服务。
         *
         * 本方法不请求悬浮窗或 root 权限；采集和 root 授权由服务工作线程执行。
         *
         * @param context 当前响应用户操作的可见 Activity 上下文。
         * @return 启动请求已提交时为 true；缺少权限或提交失败时为 false。
         */
        fun start(context: Context): Boolean {
            if (!Settings.canDrawOverlays(context)) {
                latestReport = "缺少悬浮窗权限，请在插件设置中授权"
                latestNativeReport = "Overlay permission missing; open HUD settings to grant it"
                return false
            }
            return try {
                context.startForegroundService(Intent(context, HudService::class.java).setAction(ACTION_START))
                true
            } catch (error: Exception) {
                latestReport = "性能 HUD 无法启动：${error.message ?: error.javaClass.simpleName}"
                latestNativeReport = "Unable to start HUD: ${asciiValue(error.message ?: error.javaClass.simpleName)}"
                false
            }
        }

        /**
         * 从主线程请求停止当前会话，实际资源清理由服务销毁回调执行。
         *
         * @param context 用于提交停止服务请求的宿主上下文。
         */
        fun stop(context: Context) {
            context.stopService(Intent(context, HudService::class.java))
        }
    }
}
