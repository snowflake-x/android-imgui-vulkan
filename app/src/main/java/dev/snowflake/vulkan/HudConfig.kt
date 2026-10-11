package dev.snowflake.vulkan

import android.content.Context

/**
 * 保存性能插件的字段选择、显示顺序和用户明确启用的采样能力。
 *
 * @property rootEnabled 用户是否明确允许只读 root 采样，默认关闭。
 * @property intervalMs 一轮采样完成至下一轮开始之间的毫秒数。
 * @property enabled 需要采集和显示的稳定字段标识集合。
 * @property order 悬浮窗字段顺序，允许包含未启用的字段。
 * @property surfaceOverride 需要精确匹配的前台 Surface 名称，留空时自动选择。
 * @property cpuPowerPath 用户配置的只读 CPU 功耗传感器路径。
 * @property cpuPowerScale 将传感器原始读数转换为瓦特的正数乘数。
 * @property textSp 悬浮窗字号，以 sp 为单位。
 * @property opacity 背景不透明度百分比，不改变窗口触摸穿透上限。
 * @property corner 悬浮窗位置，取值来自 [CORNERS]。
 */
data class HudConfig(
    val rootEnabled: Boolean = false,
    val intervalMs: Long = 1000,
    val enabled: Set<String> = DEFAULT_ENABLED,
    val order: List<String> = METRICS.map { it.id },
    val surfaceOverride: String = "",
    val cpuPowerPath: String = "",
    val cpuPowerScale: Double = 0.000001,
    val textSp: Int = 14,
    val opacity: Int = 80,
    val corner: String = "top_left"
) {
    /** 返回稳定字段标识 [id] 是否被用户启用。 */
    fun isEnabled(id: String): Boolean = id in enabled

    /**
     * 更新进程内配置并异步持久化，不启动采样或请求 root 权限。
     *
     * @param context 用于访问应用私有首选项的上下文。
     */
    fun save(context: Context) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putBoolean("root_enabled", rootEnabled)
            .putLong("interval_ms", intervalMs)
            .putStringSet("enabled", enabled.toSet())
            .putString("order", order.joinToString(","))
            .putString("surface_override", surfaceOverride)
            .putString("cpu_power_path", cpuPowerPath)
            .putString("cpu_power_scale", cpuPowerScale.toString())
            .putInt("text_sp", textSp)
            .putInt("opacity", opacity)
            .putString("corner", corner)
            .apply()
    }

    companion object {
        /** Android 设置、采集器与 JNI 设备信息缓存共用的首选项名称。 */
        const val PREFERENCES = "performance_hud"
        /** 受支持字段的稳定标识与显示标题，列表顺序也是初始显示顺序。 */
        val METRICS = listOf(
            HudMetric("foreground", "前台应用", "Foreground"),
            HudMetric("fps", "前台 Surface FPS", "Foreground FPS"),
            HudMetric("frame_time", "帧时间", "Frame time"),
            HudMetric("frame_p95", "P95 帧时间", "P95 frame time"),
            HudMetric("fps_source", "FPS 数据来源", "FPS source"),
            HudMetric("surface", "采样 Surface", "Surface"),
            HudMetric("gpu_model", "GPU 型号", "GPU model"),
            HudMetric("gpu_driver", "GPU 驱动", "GPU driver"),
            HudMetric("gpu_api", "GPU API", "GPU API"),
            HudMetric("cpu_freq", "CPU 频率", "CPU clock"),
            HudMetric("cpu_load", "CPU 使用率", "CPU load"),
            HudMetric("cpu_power", "CPU 功耗", "CPU power"),
            HudMetric("gpu_freq", "GPU 频率", "GPU clock"),
            HudMetric("gpu_load", "GPU 使用率", "GPU load"),
            HudMetric("memory", "系统内存", "System RAM"),
            HudMetric("battery_power", "电池功率（整机）", "Battery power (device)"),
            HudMetric("battery_temp", "电池温度", "Battery temperature"),
            HudMetric("thermal", "温控状态", "Thermal state"),
            HudMetric("display_resolution", "屏幕分辨率", "Display resolution"),
            HudMetric("display_refresh", "屏幕刷新率", "Display refresh"),
            HudMetric("engine_resolution", "引擎内部渲染分辨率", "Engine render size"),
            HudMetric("engine_output", "引擎输出分辨率", "Engine output size"),
            HudMetric("engine_source", "引擎数据来源", "Engine source")
        )
        /** 默认精简字段，包含前台 FPS 来源与引擎内部渲染尺寸。 */
        val DEFAULT_ENABLED = linkedSetOf(
            "foreground", "fps", "frame_time", "fps_source", "gpu_model", "cpu_freq", "engine_resolution", "battery_temp"
        )
        /** 可持久化的悬浮窗角落标识。 */
        val CORNERS = listOf("top_left", "top_right", "bottom_left", "bottom_right")
        /** 受支持的采样完成后等待时长，单位为毫秒。 */
        val INTERVALS = listOf(500L, 1000L, 2000L)

        /**
         * 读取配置并为未知字段、无效取值和新增字段应用兼容默认值。
         *
         * @param context 用于访问应用私有首选项的上下文。
         * @return 与可变首选项集合独立的本次配置快照。
         */
        fun load(context: Context): HudConfig {
            val prefs = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            val known = METRICS.map { it.id }
            val savedOrder = prefs.getString("order", "").orEmpty().split(",")
            val order = (savedOrder.filter { it in known } + known).distinct()
            val scale = prefs.getString("cpu_power_scale", "0.000001")?.toDoubleOrNull()
            return HudConfig(
                rootEnabled = prefs.getBoolean("root_enabled", false),
                intervalMs = prefs.getLong("interval_ms", 1000).takeIf { it in INTERVALS } ?: 1000,
                enabled = prefs.getStringSet("enabled", DEFAULT_ENABLED).orEmpty().filter { it in known }.toSet(),
                order = order,
                surfaceOverride = prefs.getString("surface_override", "").orEmpty().take(512),
                cpuPowerPath = prefs.getString("cpu_power_path", "").orEmpty().take(512),
                cpuPowerScale = scale?.takeIf { it.isFinite() && it > 0.0 } ?: 0.000001,
                textSp = prefs.getInt("text_sp", 14).coerceIn(10, 24),
                opacity = prefs.getInt("opacity", 80).coerceIn(20, 100),
                corner = prefs.getString("corner", "top_left").takeIf { it in CORNERS } ?: "top_left"
            )
        }
    }
}

/**
 * 使用稳定字段标识保存配置，显示标题不参与采样标识或持久化键的匹配。
 *
 * @property id 持久化配置及采集结果使用的稳定标识。
 * @property label Android 悬浮窗和设置页面使用的中文标题。
 * @property englishLabel 原生 ImGui 默认字体能够显示的英文标题。
 */
data class HudMetric(val id: String, val label: String, val englishLabel: String)
