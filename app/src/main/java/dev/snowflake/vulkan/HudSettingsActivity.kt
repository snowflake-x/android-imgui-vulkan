package dev.snowflake.vulkan

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.ClipData
import android.content.ClipboardManager
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * 配置既有性能插件的 Android 采样与悬浮显示能力。
 *
 * 所有界面操作在主线程执行，权限页面返回后不会自动启动服务或发起 root 授权。
 * 离开或销毁 Activity 后，用户已启动的服务继续运行至显式停止。
 */
class HudSettingsActivity : Activity() {
    private val mainHandler = Handler(Looper.getMainLooper())
    private lateinit var content: LinearLayout
    private lateinit var status: TextView
    private lateinit var preview: TextView
    private lateinit var rootToggle: Switch
    private lateinit var interval: Spinner
    private lateinit var fontSize: Spinner
    private lateinit var opacity: Spinner
    private lateinit var corner: Spinner
    private lateinit var surface: EditText
    private lateinit var cpuPath: EditText
    private lateinit var cpuScale: EditText
    private lateinit var metricList: LinearLayout
    private val metricChecks = linkedMapOf<String, CheckBox>()
    private var fieldOrder: MutableList<String> = mutableListOf()
    private var selected: MutableSet<String> = mutableSetOf()
    private val fontSizes = (10..24).toList()
    private val opacityValues = (20..100).toList()
    private val updateStatus = object : Runnable {
        /** 仅在设置页恢复期间读取现有快照，不在主线程触发采集。 */
        override fun run() {
            refreshStatus()
            mainHandler.postDelayed(this, 1500)
        }
    }

    /** 根据持久化配置创建界面，权限和服务操作保留为显式按钮动作。 */
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val config = HudConfig.load(this)
        fieldOrder = config.order.toMutableList()
        selected = config.enabled.toMutableSet()
        content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(28))
        }
        val scroll = ScrollView(this).apply {
            isFillViewport = true
            addView(content)
        }
        setContentView(scroll)
        if (Build.VERSION.SDK_INT >= 35) {
            scroll.setOnApplyWindowInsetsListener { view, insets ->
                val bars = insets.getInsets(android.view.WindowInsets.Type.systemBars())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                insets
            }
        }
        heading("性能 HUD 插件", 24)
        description("监控当前前台应用的 Surface 呈现帧率，支持跨应用悬浮显示。显示器刷新率、系统合成帧率不会用作游戏 FPS。")
        status = description("")
        val actions = row()
        button("启动 HUD", actions) { startHud() }
        button("停止 HUD", actions) {
            HudService.stop(this)
            mainHandler.postDelayed({ refreshStatus() }, 100)
        }
        button("保存配置") { if (saveConfig()) toast("配置已保存，下一轮采样生效") }
        button("授权悬浮窗") {
            runCatching {
                startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
            }.onFailure { toast("无法打开悬浮窗设置：${it.message}") }
        }
        if (Build.VERSION.SDK_INT >= 33) {
            button("授权通知") {
                if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                    requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), NOTIFICATION_REQUEST)
                } else {
                    toast("通知权限已授予")
                }
            }
        }

        heading("采样与数据来源")
        rootToggle = Switch(this).apply {
            text = "允许只读 root 采样（需要设备已 root）"
            isChecked = config.rootEnabled
        }
        content.addView(rootToggle)
        description("默认关闭。启用后启动 HUD 才会请求 su 授权，用于识别前台应用、读取 SurfaceFlinger 帧时间及受限硬件传感器。拒绝或超时后不会循环弹出授权；停止再启动可重试。无权限或无数据会明确显示不可用。")
        interval = spinner("采样间隔", listOf("500 ms", "1 秒", "2 秒"), HudConfig.INTERVALS.indexOf(config.intervalMs))
        surface = edit("Surface 精确名称覆盖（可选）", config.surfaceOverride, "留空自动选择前台游戏 Surface")
        description("自动选择存在歧义时，可填入 root 命令 dumpsys SurfaceFlinger --list 的完整层名称；仅采样属于当前前台应用的 Surface。切换应用后不继续显示旧应用数据。")
        cpuPath = edit("CPU 功耗传感器路径（可选）", config.cpuPowerPath, "/sys/…/power…")
        cpuScale = edit("CPU 功耗原始值 → W 的乘数", config.cpuPowerScale.toString(), "例如 µW 填 0.000001，mW 填 0.001")
        cpuScale.inputType = InputType.TYPE_CLASS_TEXT
        description("仅填写设备真实 CPU 功耗传感器的只读 /sys/ 路径。CPU 功耗无法普遍获取；电池功率属于整机数据，不会代替 CPU 功耗。")
        button("UE / Unity 真实分辨率说明") { showEngineHelp() }

        heading("悬浮窗样式")
        fontSize = spinner("字号", fontSizes.map { "$it sp" }, fontSizes.indexOf(config.textSp))
        opacity = spinner("背景不透明度", opacityValues.map { "$it%" }, opacityValues.indexOf(config.opacity))
        corner = spinner("位置", listOf("左上", "右上", "左下", "右下"), HudConfig.CORNERS.indexOf(config.corner))
        description("悬浮窗不拦截点击。为满足 Android 的触摸穿透规则，整个窗口透明度上限为 80%；此处调整背景填充。屏幕关闭时暂停采样。")

        heading("显示字段与顺序")
        description("勾选字段并使用上下箭头排序；可以自由组合。选择过多字段时建议缩小字号；超出屏幕高度的内容会省略，可在下方查看或复制诊断。")
        val presets = row()
        button("精简", presets) { preset(HudConfig.DEFAULT_ENABLED) }
        button("游戏", presets) {
            preset(setOf("foreground", "fps", "frame_time", "frame_p95", "fps_source", "surface", "gpu_model", "gpu_freq", "cpu_freq", "engine_resolution", "engine_output", "engine_source", "thermal"))
        }
        button("硬件", presets) {
            preset(setOf("gpu_model", "gpu_driver", "gpu_api", "cpu_freq", "cpu_load", "cpu_power", "gpu_freq", "gpu_load", "memory", "battery_power", "battery_temp", "thermal"))
        }
        val selections = row()
        button("全选", selections) { preset(HudConfig.METRICS.map { it.id }.toSet()) }
        button("清空", selections) { preset(emptySet()) }
        metricList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        content.addView(metricList)
        rebuildMetricList()
        button("保存配置") { if (saveConfig()) toast("配置已保存，下一轮采样生效") }
        heading("当前采样")
        preview = description(HudService.latestReport).apply { setTextIsSelectable(true) }
        button("复制最近游戏诊断") {
            val clipboard = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            clipboard.setPrimaryClip(ClipData.newPlainText(
                "VulkanScope HUD diagnostics", HudService.latestGameDiagnostics.ifBlank { HudService.latestDiagnostics }
            ))
            toast("已复制最近游戏采样及其时间；尚未采集游戏时复制当前状态")
        }
        button("返回") {
            if (saveConfig()) finish()
        }
    }

    /** 恢复设置页的快照状态刷新，不恢复或启动采样服务。 */
    override fun onResume() {
        super.onResume()
        mainHandler.removeCallbacks(updateStatus)
        mainHandler.post(updateStatus)
    }

    /** 移除状态刷新回调，避免离开或销毁设置页后继续访问界面。 */
    override fun onPause() {
        mainHandler.removeCallbacks(updateStatus)
        super.onPause()
    }

    private fun refreshStatus() {
        val overlayState = if (Settings.canDrawOverlays(this)) "已授予" else "未授予"
        val notifications = if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            "；通知未授权（可在这里停止 HUD）"
        } else ""
        status.text = "状态：${if (HudService.running) "运行中" else "已停止"} · 悬浮窗权限：$overlayState$notifications"
        preview.text = HudService.latestReport
    }

    private fun startHud() {
        if (!saveConfig()) return
        if (!Settings.canDrawOverlays(this)) {
            toast("请先点击「授权悬浮窗」，授权后返回并启动 HUD")
            return
        }
        if (!HudService.start(this)) toast(HudService.latestReport)
        mainHandler.postDelayed({ refreshStatus() }, 250)
    }

    private fun saveConfig(): Boolean {
        val multiplier = cpuScale.text.toString().trim().toDoubleOrNull()
        if (multiplier == null || !multiplier.isFinite() || multiplier <= 0.0) {
            cpuScale.error = "请输入大于 0 的有效乘数"
            cpuScale.requestFocus()
            return false
        }
        val path = cpuPath.text.toString().trim()
        if (path.isNotEmpty() && (!path.startsWith("/sys/") || path.contains("..") || !path.matches(Regex("[A-Za-z0-9_./:-]+")))) {
            cpuPath.error = "仅允许 /sys/ 下不含空格或 .. 的传感器路径"
            cpuPath.requestFocus()
            return false
        }
        readSelection()
        HudConfig(
            rootEnabled = rootToggle.isChecked,
            intervalMs = HudConfig.INTERVALS[interval.selectedItemPosition],
            enabled = selected.toSet(),
            order = fieldOrder.toList(),
            surfaceOverride = surface.text.toString().trim().take(512),
            cpuPowerPath = path,
            cpuPowerScale = multiplier,
            textSp = fontSizes[fontSize.selectedItemPosition],
            opacity = opacityValues[opacity.selectedItemPosition],
            corner = HudConfig.CORNERS[corner.selectedItemPosition]
        ).save(this)
        return true
    }

    private fun preset(fields: Set<String>) {
        selected = fields.toMutableSet()
        rebuildMetricList()
    }

    private fun readSelection() {
        selected = metricChecks.filterValues { it.isChecked }.keys.toMutableSet()
    }

    private fun rebuildMetricList() {
        metricList.removeAllViews()
        metricChecks.clear()
        val labels = HudConfig.METRICS.associate { it.id to it.label }
        fieldOrder.forEachIndexed { index, id ->
            val line = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val check = CheckBox(this).apply {
                text = labels[id] ?: id
                isChecked = id in selected
                setOnCheckedChangeListener { _, checked -> if (checked) selected.add(id) else selected.remove(id) }
            }
            metricChecks[id] = check
            line.addView(check, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            listOf(-1 to "↑", 1 to "↓").forEach { (delta, arrow) ->
                val next = index + delta
                val move = Button(this).apply {
                    text = arrow
                    minWidth = 0
                    minimumWidth = 0
                    contentDescription = "${labels[id]}${if (delta < 0) "上移" else "下移"}"
                    isEnabled = next in fieldOrder.indices
                    setOnClickListener {
                        readSelection()
                        fieldOrder.removeAt(index)
                        fieldOrder.add(next, id)
                        rebuildMetricList()
                    }
                }
                line.addView(move, LinearLayout.LayoutParams(dp(44), dp(48)))
            }
            metricList.addView(line)
        }
    }

    private fun showEngineHelp() {
        AlertDialog.Builder(this)
            .setTitle("UE / Unity 真实渲染分辨率")
            .setMessage(
                "内部渲染分辨率与屏幕、窗口及交换链尺寸是不同的数据。动态分辨率或升频开启后，它们可能不同。\n\n" +
                    "HUD 面向手机上已安装的第三方游戏，可自动识别可访问的 UE / Unity 进程特征。读取真实内部尺寸仍需要适配具体游戏和版本；Android 没有通用接口访问其他游戏的内部渲染纹理。\n\n" +
                    "尚未适配、无访问权限或报告过期时显示不可用及原因，不把屏幕、窗口或 Surface 尺寸当作内部渲染分辨率。\n\n" +
                    "支持的诊断桥接也可作为可选数据来源，适用于已经提供该报告的游戏或开发环境。详细能力边界见项目文档。"
            )
            .setPositiveButton("知道了", null)
            .show()
    }

    private fun heading(title: String, size: Int = 19) {
        content.addView(TextView(this).apply {
            text = title
            textSize = size.toFloat()
            setTextColor(Color.WHITE)
            setPadding(0, dp(18), 0, dp(8))
        })
    }

    private fun description(message: String): TextView {
        return TextView(this).apply {
            text = message
            textSize = 14f
            setTextColor(Color.LTGRAY)
            setPadding(0, dp(5), 0, dp(8))
            content.addView(this)
        }
    }

    private fun row(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        content.addView(this)
    }

    private fun button(title: String, parent: LinearLayout = content, action: () -> Unit) {
        val button = Button(this).apply {
            text = title
            isAllCaps = false
            setOnClickListener { action() }
        }
        if (parent === content) parent.addView(button)
        else parent.addView(button, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
    }

    private fun spinner(title: String, values: List<String>, selection: Int): Spinner {
        description(title)
        return Spinner(this).apply {
            adapter = ArrayAdapter(this@HudSettingsActivity, android.R.layout.simple_spinner_dropdown_item, values)
            setSelection(selection.coerceAtLeast(0))
            content.addView(this)
        }
    }

    private fun edit(title: String, initial: String, hint: String): EditText {
        description(title)
        return EditText(this).apply {
            setText(initial)
            this.hint = hint
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            isSingleLine = true
            content.addView(this)
        }
    }

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_LONG).show()
    private fun dp(value: Int): Int = (resources.displayMetrics.density * value).toInt()

    companion object {
        private const val NOTIFICATION_REQUEST = 41
    }
}
