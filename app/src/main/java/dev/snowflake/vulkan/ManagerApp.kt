package dev.snowflake.vulkan

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONArray
import org.json.JSONObject
import top.yukonga.miuix.kmp.basic.*
import top.yukonga.miuix.kmp.extra.SuperDropdown
import top.yukonga.miuix.kmp.extra.SuperSwitch
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.darkColorScheme
import top.yukonga.miuix.kmp.theme.lightColorScheme
import kotlin.math.roundToInt

private val LocalChinese = staticCompositionLocalOf { false }
@Composable private fun tr(english: String, chinese: String) = if (LocalChinese.current) chinese else english
private fun JSONArray.objects() = List(length()) { getJSONObject(it) }
private fun JSONObject.array(key: String) = optJSONArray(key)?.objects().orEmpty()

/** Miuix provides native text fallback, accessibility semantics, touch scrolling and fling animations. */
@Composable
internal fun ManagerApp(
    state: ManagerState,
    change: (String, String) -> Unit,
    loadDiagnostic: (String, Int) -> Unit,
    action: (Int) -> Unit,
) {
    val preference = state.config.optString("language", "system")
    val chinese = preference == "zh-Hans" || (preference == "system" && LocalConfiguration.current.locales[0].language == "zh")
    val colors = if (isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFF82AFFF))
                 else lightColorScheme(primary = Color(0xFF3482FF))
    CompositionLocalProvider(LocalChinese provides chinese) {
        MiuixTheme(colors = colors) {
            var page by rememberSaveable { mutableIntStateOf(0) }
            var screen by rememberSaveable { mutableStateOf("main") }
            val homeScroll = rememberLazyListState()
            val pluginScroll = rememberLazyListState()
            val settingsScroll = rememberLazyListState()
            val performanceScroll = rememberLazyListState()
            val helpScroll = rememberLazyListState()
            var helpReturn by rememberSaveable { mutableStateOf("main") }
            val scrollBehavior = MiuixScrollBehavior()
            BackHandler(screen != "main") { screen = if (screen == "metric_help") helpReturn else "main" }
            val title = when (screen) {
                "settings" -> tr("Global settings", "全局设置")
                "performance_settings" -> tr("Performance settings", "性能插件设置")
                "diagnostics" -> tr("Diagnostics", "诊断")
                "metric_help" -> tr("Metric guide", "指标说明")
                else -> if (page == 0) "VulkanScope" else tr("Plugins", "插件列表")
            }
            Scaffold(
                topBar = {
                    TopAppBar(title = title, scrollBehavior = scrollBehavior,
                        navigationIcon = {
                            if (screen != "main") IconButton(onClick = { screen = if (screen == "metric_help") helpReturn else "main" }) {
                                Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = tr("Back", "返回"))
                            }
                        }, actions = {
                            if (screen == "main") IconButton(onClick = { screen = "settings" }) {
                                Icon(Icons.Rounded.Settings, contentDescription = tr("Settings", "设置"))
                            }
                        })
                },
                bottomBar = {
                    NavigationBar(showDivider = false) {
                        listOf(Icons.Rounded.Home to tr("Home", "主页"), Icons.Rounded.Extension to tr("Plugins", "插件列表"))
                            .forEachIndexed { index, item ->
                                NavigationBarItem(icon = item.first, label = item.second,
                                    selected = page == index, onClick = { page = index; screen = "main" },
                                    modifier = Modifier.weight(1f))
                            }
                    }
                },
            ) { padding ->
                val contentPadding = PaddingValues(start = 12.dp, end = 12.dp,
                    top = padding.calculateTopPadding() + 12.dp, bottom = padding.calculateBottomPadding() + 24.dp)
                val modifier = Modifier.fillMaxSize().nestedScroll(scrollBehavior.nestedScrollConnection)
                if (screen == "diagnostics") {
                    DiagnosticsPage(state, loadDiagnostic, modifier, contentPadding)
                } else {
                    LazyColumn(modifier = modifier,
                        state = when (screen) {
                            "settings" -> settingsScroll
                            "performance_settings" -> performanceScroll
                            "metric_help" -> helpScroll
                            else -> if (page == 0) homeScroll else pluginScroll
                        },
                        contentPadding = contentPadding, verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        if (state.loading) item { MessageCard(tr("Preparing native monitoring…", "正在初始化原生监控…")) }
                        state.error?.let { error -> item { MessageCard(error) } }
                        if (!state.loading) {
                            if (screen == "metric_help") {
                                items(metricHelp, key = { it.id }) { help ->
                                    Card {
                                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                                            SectionTitle(tr(help.titleEn, help.titleZh))
                                            Text(tr(help.english, help.chinese), fontSize = 15.sp)
                                        }
                                    }
                                }
                            } else if (screen == "settings") {
                                item { InterfaceSettings(state, change) }
                                item { StartupRootSettings(state, change) }
                            } else if (screen == "performance_settings") {
                                item { TextButton(tr("Metric guide", "指标说明"), {
                                    helpReturn = "performance_settings"; screen = "metric_help"
                                }, Modifier.fillMaxWidth()) }
                                item { LiveUpdateSettings(state, change, action) }
                                item { PerformanceRootSettings(state, change) }
                                item { DisplaySettings(state, change) }
                                item { SectionTitle(tr("Fields and order", "字段与顺序")) }
                                item {
                                    Card {
                                        Row(Modifier.padding(12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                            TextButton(tr("Game", "游戏预设"), { change("preset", "game") }, Modifier.weight(1f))
                                            TextButton(tr("Hardware", "硬件预设"), { change("preset", "hardware") }, Modifier.weight(1f))
                                        }
                                    }
                                }
                                items(state.data.array("metrics"), key = { it.getString("id") }) { metric ->
                                    Card {
                                        SuperSwitch(title = metric.getString("label"), checked = metric.getBoolean("enabled"),
                                            onCheckedChange = { change("metric:" + metric.getString("id"), it.toString()) })
                                        Row(Modifier.padding(start = 12.dp, end = 12.dp, bottom = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                            TextButton(tr("Move up", "上移"), { change("up", metric.getString("id")) }, Modifier.weight(1f))
                                            TextButton(tr("Move down", "下移"), { change("down", metric.getString("id")) }, Modifier.weight(1f))
                                        }
                                    }
                                }
                                item { AdvancedSettings(state, change) }
                                item { TextButton(tr("Reset performance settings", "恢复性能插件默认配置"), { change("resetPerformance", "true") }, Modifier.fillMaxWidth()) }
                            } else if (page == 1) {
                                item { Description(tr("Choose the information shown on your home page.", "选择主页中显示的功能，开关会自动保存。")) }
                                items(state.data.array("plugins"), key = { it.getString("id") }) { plugin ->
                                    Card {
                                        SuperSwitch(title = plugin.getString("label"), summary = plugin.getString("description"),
                                            checked = plugin.getBoolean("enabled"),
                                            startAction = { Icon(pluginIcon(plugin.getString("id")), null, tint = MiuixTheme.colorScheme.primary) },
                                            onCheckedChange = { change("plugin:" + plugin.getString("id"), it.toString()) })
                                        if (plugin.getString("id") == "performance-hud") {
                                            TextButton(tr("Performance settings", "性能插件设置"),
                                                { screen = "performance_settings" }, Modifier.fillMaxWidth().padding(12.dp))
                                        }
                                    }
                                }
                            } else {
                                item { StatusCard(state, change) }
                                if (state.pluginEnabled("performance-hud")) item {
                                    HudCard(state, action, { screen = "performance_settings" }, {
                                        screen = "diagnostics"; loadDiagnostic("", 0)
                                    }, { helpReturn = "main"; screen = "metric_help" })
                                }
                                if (state.pluginEnabled("device-info")) item { DeviceCard(state.data.optString("system")) }
                                if (state.pluginEnabled("gpu-info")) item { GpuCard(state) }
                                item {
                                    Card {
                                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                            SectionTitle(tr("Permissions", "系统权限"))
                                            TextButton(tr("Overlay permission", "普通悬浮窗权限"), { action(0) }, Modifier.fillMaxWidth())
                                            TextButton(tr("Notification permission", "通知权限"), { action(4) }, Modifier.fillMaxWidth())
                                        }
                                    }
                                }
                                item { Description(tr("VulkanScope · Native performance monitoring", "VulkanScope · 原生性能监控")) }
                            }
                        }
                    }
                }
            }
        }
    }
}

private fun ManagerState.pluginEnabled(id: String) = data.array("plugins").any { it.optString("id") == id && it.optBoolean("enabled") }
private fun pluginIcon(id: String): ImageVector = when (id) {
    "device-info" -> Icons.Rounded.Smartphone
    "gpu-info" -> Icons.Rounded.Memory
    else -> Icons.Rounded.Speed
}
@Composable private fun SectionTitle(text: String) { Text(text, fontSize = 18.sp, fontWeight = FontWeight.SemiBold) }
@Composable private fun Description(text: String) { Text(text, color = MiuixTheme.colorScheme.onSurfaceVariantSummary, fontSize = 14.sp) }
@Composable private fun MessageCard(text: String) { Card { Text(text, modifier = Modifier.padding(16.dp)) } }

@Composable private fun StatusCard(state: ManagerState, change: (String, String) -> Unit) {
    val status = state.data.optString("rootAccess", "unknown")
    val granted = status == "granted"
    val dark = isSystemInDarkTheme()
    val title = when (status) {
        "granted" -> tr("Root authorized", "Root 已授权")
        "requesting" -> tr("Requesting root", "正在申请 Root")
        "unavailable" -> tr("Root unavailable", "Root 不可用")
        else -> tr("Ready to monitor", "准备就绪")
    }
    Card(colors = CardDefaults.defaultColors(color = if (granted) {
        if (dark) Color(0xFF1A3825) else Color(0xFFDFFAE4)
    } else MiuixTheme.colorScheme.secondaryContainer)) {
        Row(Modifier.fillMaxWidth().padding(20.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(title, fontSize = 24.sp, fontWeight = FontWeight.SemiBold)
                Text(when (status) {
                    "granted" -> tr("Native sampling is available", "原生采样权限已就绪")
                    "requesting" -> tr("Approve the request in your root manager", "请在 Root 管理器中允许本次申请")
                    else -> tr("An existing root provider is required", "需要设备已安装 Root 提供者")
                }, fontSize = 14.sp)
            }
            Icon(if (granted) Icons.Rounded.CheckCircleOutline else Icons.Rounded.Security, null,
                Modifier.size(64.dp), tint = if (granted) Color(0xFF36B961) else MiuixTheme.colorScheme.primary)
        }
        if (status != "requesting" && !granted) TextButton(tr("Retry root", "重新申请 Root"),
            { change("retryRoot", "true") }, Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
    }
}

@Composable private fun HudCard(state: ManagerState, action: (Int) -> Unit, configure: () -> Unit, diagnostics: () -> Unit, explain: () -> Unit) {
    val values = state.data.optJSONObject("values") ?: JSONObject()
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                SectionTitle(tr("Performance HUD", "性能监控"))
                Text(if (state.running) tr("Running", "运行中") else tr("Stopped", "已停止"), color = MiuixTheme.colorScheme.primary, fontSize = 14.sp)
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Column(Modifier.weight(1f)) {
                    Text(if (state.running && state.data.optBoolean("fresh")) values.optString("fps", "—").takeIf { !it.startsWith("N/A") } ?: "—" else "—", fontSize = 28.sp, fontWeight = FontWeight.SemiBold)
                    Description(tr("Foreground FPS", "前台帧率"))
                }
                Column(Modifier.weight(1f)) {
                    Text(if (state.running && state.data.optBoolean("fresh")) values.optString("frame_time", "—").takeIf { !it.startsWith("N/A") } ?: "—" else "—", fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
                    Description(tr("Presentation time", "呈现间隔"))
                }
            }
            if (state.running) Description(if (state.data.optBoolean("rootDisplayActive")) tr("Root display layer", "Root 独立显示图层") else tr("Standard overlay", "普通悬浮窗"))
            val apiStatus = values.optString("graphics_api_status")
            Text(state.data.optString("apiCaption", "API: OFF"), color = MiuixTheme.colorScheme.primary)
            if (apiStatus == "no_calls") Description(tr(
                "No EGL/Vulkan loader calls were captured in 200 ms. Direct driver dispatch or another rendering process may bypass these probes. Root permission alone cannot identify the API.",
                "200 毫秒内未捕获 EGL/Vulkan 加载库的调用。驱动函数指针或其他渲染进程可能绕过探针，Root 授权本身不能确定图形 API。"))
            Description(if (state.running) values.optString("foreground", "") else tr("Start monitoring, then switch to your game.", "启动监控后切换到游戏。"))
            TextButton(if (state.running) tr("Stop HUD", "停止显示") else tr("Start HUD", "启动显示"),
                { action(if (state.running) 2 else 1) }, Modifier.fillMaxWidth(),
                colors = ButtonDefaults.textButtonColorsPrimary())
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(tr("Configure", "配置"), configure, Modifier.weight(1f))
                TextButton(tr("Diagnostics", "游戏诊断"), diagnostics, Modifier.weight(1f))
                TextButton(tr("Metric guide", "指标说明"), explain, Modifier.weight(1f))
            }
            if (!state.running && state.data.optString("report").isNotBlank()) Description(state.data.optString("report"))
        }
    }
}

@Composable private fun DeviceCard(report: String) {
    var expanded by rememberSaveable { mutableStateOf(false) }
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(tr("Device information", "设备信息"))
            val rows = report.lineSequence().filter { it.isNotBlank() }.toList()
            (if (expanded) rows else rows.take(6)).forEach { row ->
                val parts = row.split(':', limit = 2)
                if (parts.size == 2) InfoRow(parts[0], parts[1].trim()) else Text(row)
            }
            if (rows.size > 6) TextButton(if (expanded) tr("Show less", "收起") else tr("Show all", "查看全部"), { expanded = !expanded }, Modifier.fillMaxWidth())
        }
    }
}
@Composable private fun InfoRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(label, modifier = Modifier.weight(1f), color = MiuixTheme.colorScheme.onSurfaceVariantSummary, fontSize = 14.sp)
        Text(value, modifier = Modifier.weight(1.4f), fontSize = 14.sp)
    }
}
@Composable private fun GpuCard(state: ManagerState) {
    val values = state.data.optJSONObject("values") ?: JSONObject()
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(tr("Graphics device", "图形设备"))
            InfoRow(tr("GPU", "GPU 型号"), values.optString("gpu_model", "—"))
            InfoRow(tr("Supported Vulkan", "支持的 Vulkan 版本"), values.optString("gpu_api", "—"))
            InfoRow(tr("Driver", "驱动版本"), values.optString("gpu_driver", "—"))
            Description(tr("Device capabilities are separate from the game's active graphics API.", "设备支持的版本与游戏当前使用的图形 API 分别显示。"))
        }
    }
}
@Composable private fun StartupRootSettings(state: ManagerState, change: (String, String) -> Unit) {
    Card {
        SuperSwitch(title = tr("Request root on startup", "启动时自动申请 Root"),
            summary = tr("Enable native sampling after authorization", "授权成功后自动启用原生采样"),
            checked = state.config.optBoolean("autoRoot", true), onCheckedChange = { change("autoRoot", it.toString()) })
    }
}
@Composable private fun PerformanceRootSettings(state: ManagerState, change: (String, String) -> Unit) {
    Card {
        SuperSwitch(title = tr("Root display layer", "Root 独立显示图层"),
            summary = tr("Use SurfaceFlinger when root is available. Turn off for standard overlay; restart HUD to apply.",
                "有 Root 时直接创建 SurfaceFlinger 图层。关闭后使用普通悬浮窗，重启显示生效。"),
            checked = state.config.optBoolean("rootDisplay", true), onCheckedChange = { change("rootDisplay", it.toString()) })
        SuperSwitch(title = tr("Root sampling", "Root 采样"), checked = state.config.optBoolean("root"),
            onCheckedChange = { change("root", it.toString()) })
    }
}
@Composable private fun DisplaySettings(state: ManagerState, change: (String, String) -> Unit) {
    val config = state.config
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(tr("HUD appearance", "显示样式"))
            RangeSetting(tr("Text size", "文字大小"), "textSize", config.optInt("textSize", 14), 10..24, change)
            RangeSetting(tr("Opacity", "不透明度"), "opacity", config.optInt("opacity", 80), 20..100, change)
        }
        SuperDropdown(title = tr("Position", "显示位置"), selectedIndex = config.optInt("corner"),
            items = listOf(tr("Top left", "左上"), tr("Top right", "右上"), tr("Bottom left", "左下"), tr("Bottom right", "右下")),
            onSelectedIndexChange = { change("corner", it.toString()) })
        SuperDropdown(title = tr("Sample interval", "采样间隔"), selectedIndex = listOf(500, 1000, 2000).indexOf(config.optInt("interval", 1000)).coerceAtLeast(0),
            items = listOf("500 ms", "1000 ms", "2000 ms"), onSelectedIndexChange = { change("interval", listOf(500, 1000, 2000)[it].toString()) })
        SuperSwitch(title = tr("Frame history", "帧时间曲线"), checked = config.optBoolean("hudGraph", true), onCheckedChange = { change("hudGraph", it.toString()) })
        SuperSwitch(title = tr("Additional sensors", "附加传感器行"), checked = config.optBoolean("hudExtras"), onCheckedChange = { change("hudExtras", it.toString()) })
    }
}
@Composable private fun RangeSetting(title: String, key: String, value: Int, range: IntRange, change: (String, String) -> Unit) {
    var current by remember(value) { mutableFloatStateOf(value.toFloat()) }
    Text("$title · ${current.roundToInt()}")
    Slider(value = current, onValueChange = { current = it }, valueRange = range.first.toFloat()..range.last.toFloat(),
        onValueChangeFinished = { change(key, current.roundToInt().toString()) })
}
@Composable private fun InterfaceSettings(state: ManagerState, change: (String, String) -> Unit) {
    Card {
        SuperDropdown(title = tr("Language", "语言"), selectedIndex = listOf("system", "en", "zh-Hans").indexOf(state.config.optString("language", "system")).coerceAtLeast(0),
            items = listOf(tr("Follow system", "跟随系统"), "English", "简体中文"),
            onSelectedIndexChange = { change("language", listOf("system", "en", "zh-Hans")[it]) })
        Text(tr("Uses your system font and follows the system light / dark theme.", "自动使用系统字体，并跟随系统浅色 / 深色模式。"),
            modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp), fontSize = 14.sp,
            color = MiuixTheme.colorScheme.onSurfaceVariantSummary)
    }
}
@Composable private fun AdvancedSettings(state: ManagerState, change: (String, String) -> Unit) {
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle(tr("Advanced", "高级配置"))
            EditSetting(tr("Exact game Surface (optional)", "游戏 Surface 精确名称（可留空）"), "surface", state.config.optString("surface"), change)
            EditSetting(tr("CPU power sensor path", "CPU 功率传感器路径"), "powerPath", state.config.optString("powerPath"), change)
            EditSetting(tr("Sensor to watts multiplier", "传感器数值转瓦特倍率"), "powerScale", state.config.optString("powerScale", "0.000001"), change)
            Description(tr("Graphics API and GPU timing require separate tracing switches. Loaded: identifies libraries in the game process; it does not confirm rendering calls.",
                "图形 API 和 GPU 耗时需要分别开启跟踪。Loaded: 表示游戏进程加载了相关库，尚未确认实际渲染调用。"))
        }
    }
}
@Composable private fun EditSetting(title: String, key: String, value: String, change: (String, String) -> Unit) {
    var draft by rememberSaveable(key, value) { mutableStateOf(value) }
    Text(title, fontSize = 14.sp)
    TextField(value = draft, onValueChange = { if (it.length <= 512) draft = it }, modifier = Modifier.fillMaxWidth())
    TextButton(tr("Save", "保存"), { change(key, draft) }, Modifier.fillMaxWidth(), enabled = draft != value)
}

@Composable private fun DiagnosticsPage(state: ManagerState, load: (String, Int) -> Unit, modifier: Modifier, padding: PaddingValues) {
    var packageName by rememberSaveable { mutableStateOf("") }
    var user by rememberSaveable { mutableIntStateOf(0) }
    val history = state.data.array("history")
    val selected = history.indexOfFirst { it.optString("package") == packageName && it.optInt("user") == user } + 1
    val names = listOf(tr("Latest sample", "最新采样")) + history.map { "${it.optString("package")} (u${it.optInt("user")})" }
    val context = LocalContext.current
    LazyColumn(modifier = modifier, contentPadding = padding, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            Card {
                SuperDropdown(title = tr("Captured application", "已采样应用"), items = names, selectedIndex = selected.coerceAtLeast(0),
                    onSelectedIndexChange = { index ->
                        packageName = if (index == 0) "" else history[index - 1].getString("package")
                        user = if (index == 0) 0 else history[index - 1].getInt("user")
                        load(packageName, user)
                    })
                Row(Modifier.padding(12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(tr("Refresh", "刷新"), { load(packageName, user) }, Modifier.weight(1f))
                    TextButton(tr("Copy", "复制"), {
                        context.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("VulkanScope", state.diagnostics))
                    }, Modifier.weight(1f), enabled = state.diagnostics.isNotBlank())
                }
            }
        }
        item { Description(tr("Each game's last sample is retained when you return to the desktop.", "返回桌面后仍保留各游戏最后一次采样，可选择对应包名查看。")) }
        item { Card { SelectionContainer { Text(state.diagnostics.ifBlank { tr("No sample yet", "暂无采样") }, modifier = Modifier.padding(16.dp), fontSize = 13.sp) } } }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                SectionTitle(tr("Root diagnostics", "Root 诊断"))
                SelectionContainer { Text(state.data.optString("rootReport"), fontSize = 13.sp) }
            } }
        }
    }
}

@Preview(showBackground = true, widthDp = 412, heightDp = 915)
@Composable private fun ManagerPreview() {
    val data = JSONObject("""{"rootAccess":"granted","config":{"language":"zh-Hans","root":true,"autoRoot":true},"running":false,"plugins":[{"id":"performance-hud","enabled":true},{"id":"gpu-info","enabled":true},{"id":"device-info","enabled":true}],"values":{"gpu_model":"Adreno (TM) 830","gpu_api":"1.3.284","gpu_driver":"2150760522"},"system":"设备: OnePlus PJZ110\nAndroid: 16 (API 36)\nCPU 核心数: 8\n显示: 3168 x 1440\n刷新率: 120 Hz"}""")
    ManagerApp(ManagerState(data = data, loading = false), { _, _ -> }, { _, _ -> }, {})
}

/** Performance-only controls. The service verifies recovery notifications before hiding the HUD. */
@Composable
private fun LiveUpdateSettings(state: ManagerState, change: (String, String) -> Unit, action: (Int) -> Unit) {
    val enabled = state.config.optBoolean("liveUpdates")
    val collapsed = state.data.optBoolean("hudCollapsed")
    val status = state.data.optString("liveUpdatesStatus")
    Card {
        SuperSwitch(title = tr("Fluid Cloud / Live Updates", "流体云 / 实时活动"),
            summary = tr("Allow the HUD to collapse into a system live notification", "允许将 HUD 收起到系统实时通知"),
            checked = enabled, onCheckedChange = { change("liveUpdates", it.toString()) })
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Description(tr("Android 16+ requests a system capsule with live FPS. ColorOS decides whether to promote it; otherwise use the notification card to expand the HUD. Sampling continues while collapsed.",
                "Android 16 及以上可申请带实时 FPS 的系统胶囊。是否进入流体云由 ColorOS 决定；未提升时可从通知卡片展开 HUD。收起期间继续采样。"))
            Description(when (status) {
                "promoted" -> tr("System reports this notification as promoted", "系统已将通知提升为实时活动")
                "eligible" -> tr("Promotion allowed; capsule display is not confirmed", "允许实时活动；尚未确认系统显示胶囊")
                "blocked" -> tr("Notifications blocked: enable them before collapsing", "通知被禁用：开启通知后才能收起")
                "unsupported" -> tr("This Android version supports the notification card only", "当前 Android 版本仅支持普通通知卡片")
                "not_allowed" -> tr("Live Updates not allowed; ordinary notification remains available", "系统未允许实时活动；仍可使用普通通知卡片")
                else -> tr("System capsule capability could not be confirmed", "尚无法确认系统胶囊支持状态")
            })
            TextButton(tr("System notification settings", "系统通知设置"), { action(5) }, Modifier.fillMaxWidth())
            if (state.running && (enabled || collapsed)) TextButton(
                if (collapsed) tr("Expand HUD", "展开 HUD") else tr("Collapse HUD", "收起 HUD"),
                { action(if (collapsed) 7 else 6) }, Modifier.fillMaxWidth(),
                enabled = collapsed || status != "blocked")
        }
    }
}
