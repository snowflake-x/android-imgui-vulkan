package dev.snowflake.vulkan

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import top.yukonga.miuix.kmp.basic.*
import top.yukonga.miuix.kmp.extra.SuperDropdown

/** Form and document-export bridge only; native plugin workers own process discovery, reads and parsing. */
@Composable
internal fun MemoryScannerPage(
    state: ManagerState, change: (String, String) -> Unit, chinese: Boolean,
    modifier: Modifier, padding: PaddingValues,
) {
    fun tr(en: String, zh: String) = if (chinese) zh else en
    val memory = state.data.optJSONObject("memory") ?: JSONObject()
    val array = memory.optJSONArray("processes")
    val processes = List(array?.length() ?: 0) { array!!.getJSONObject(it) }
    val enabled = state.config.optInt("managerPlugins", 7) and 8 != 0
    val root = state.data.optString("rootAccess") == "granted"
    val busy = memory.optBoolean("busy")
    var target by rememberSaveable { mutableStateOf("") }
    var kind by rememberSaveable { mutableIntStateOf(0) }
    var query by rememberSaveable { mutableStateOf("") }
    var address by rememberSaveable { mutableStateOf("") }
    var viewSize by rememberSaveable { mutableIntStateOf(0) }
    var exportText by remember { mutableStateOf("") }
    var exportStatus by remember { mutableStateOf("") }
    val selected = processes.indexOfFirst { "${it.optInt("pid")}:${it.optString("start")}" == target }
    val process = processes.getOrNull(selected)
    val report = memory.optString("report")
    val sameTarget = target.isNotEmpty() && target == memory.optString("targetIdentity")
    val viewHex = if (sameTarget) memory.optString("viewHex") else ""
    val matchesArray = memory.optJSONArray("matches")
    val matches = if (sameTarget) List((matchesArray?.length() ?: 0).coerceAtMost(128)) { matchesArray!!.getString(it) } else emptyList()
    val pointersArray = memory.optJSONArray("pointers")
    val pointers = List(if (sameTarget) pointersArray?.length() ?: 0 else 0) { pointersArray!!.getJSONObject(it) }
    val changesArray = memory.optJSONArray("changed")
    val changed = List(changesArray?.length() ?: 0) { changesArray!!.getInt(it) }.toSet()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri ->
        if (uri != null) {
            val captured = exportText
            scope.launch {
                exportStatus = try {
                    withContext(Dispatchers.IO) {
                        context.contentResolver.openOutputStream(uri, "wt")?.use {
                            it.write(captured.toByteArray(Charsets.UTF_8))
                        } ?: error("Cannot open document")
                    }
                    tr("Report exported", "报告已导出")
                } catch (error: Exception) {
                    if (error is kotlinx.coroutines.CancellationException) throw error
                    tr("Export failed: ", "导出失败：") + error.message
                }
                exportText = ""
            }
        } else exportText = ""
    }
    fun request(operation: String) {
        val p = process ?: return
        if (operation == "view") {
            change("memory:view", "${p.getInt("pid")} ${p.getString("start")} $address ${listOf(256, 1024, 4096)[viewSize]}")
            return
        }
        val kinds = listOf("utf8", "hex", "i32", "i64", "f32")
        val encoded = if (operation == "scan") query.toByteArray(Charsets.UTF_8)
            .joinToString("") { "%02x".format(it.toInt() and 255) } else "00"
        change("memory:$operation", "${p.getInt("pid")} ${p.getString("start")} ${kinds[kind]} $encoded")
    }
    LazyColumn(modifier, contentPadding = padding, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            Card {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(tr("Process memory / Unity", "进程内存 / Unity"), fontSize = 20.sp)
                    Text(tr("Exact search reads the selected process. Unity export validates live IL2CPP metadata type names; it does not enumerate instantiated objects. Mono and encrypted metadata are currently unsupported.",
                        "精确搜索读取所选进程。Unity 导出会校验运行中 IL2CPP 元数据的类型名称，不表示已实例化对象列表。暂不支持 Mono 或加密元数据。"))
                    Text(tr("Each operation is bounded to 15 s of native work and 512 MiB of scan reads, with at most 1,024 matches or 10,000 exported type names. Limits and unreadable regions are reported as partial results.",
                        "每次原生任务最多运行 15 秒、扫描读取 512 MiB，最多显示 1,024 个匹配或导出 10,000 个类型名称。达到上限或区域不可读时会标记结果不完整。"))
                    if (!enabled) TextButton(tr("Enable plugin", "启用插件"), { change("plugin:memory-scanner", "true") }, Modifier.fillMaxWidth())
                    if (!root) TextButton(tr("Request root", "申请 Root 权限"), { change("retryRoot", "true") }, Modifier.fillMaxWidth())
                    TextButton(tr("Refresh running app processes", "刷新正在运行的应用进程"),
                        { change("memory:list", "") }, Modifier.fillMaxWidth(), enabled = enabled && root && !busy)
                }
            }
        }
        item {
            Card {
                SuperDropdown(title = tr("Target process", "目标进程"),
                    items = listOf(tr("Select a process", "请选择进程")) + processes.map {
                        "${it.optString("name")} · PID ${it.optInt("pid")} · u${it.optInt("uid") / 100000}"
                    }, selectedIndex = selected + 1, onSelectedIndexChange = {
                        target = processes.getOrNull(it - 1)?.let { p -> "${p.optInt("pid")}:${p.optString("start")}" } ?: ""
                    })
            }
        }
        item {
            Card {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (process != null) Text(tr("Target remains selected when switching apps.", "切换应用不会自动更改所选目标进程。"))
                    SuperDropdown(title = tr("Search type", "搜索类型"), selectedIndex = kind,
                        items = listOf("UTF-8", tr("Hex bytes", "十六进制字节"), "Int32", "Int64", "Float32"),
                        onSelectedIndexChange = { kind = it })
                    TextField(value = query, onValueChange = { if (it.toByteArray(Charsets.UTF_8).size <= 128) query = it },
                        modifier = Modifier.fillMaxWidth().semantics { contentDescription = tr("Search value", "搜索值") })
                    Text(tr("Up to 128 input bytes. Hex example: 01 02 FF. Numbers use exact little-endian representation.",
                        "输入最多 128 字节。十六进制示例：01 02 FF。数值按小端字节精确匹配。"), fontSize = 13.sp)
                    TextButton(tr("Scan memory", "扫描内存"), { request("scan") }, Modifier.fillMaxWidth(),
                        enabled = enabled && root && !busy && process != null && query.isNotEmpty())
                    TextButton(tr("Attempt Unity class-name dump", "尝试导出 Unity 类名"), { request("unity") }, Modifier.fillMaxWidth(),
                        enabled = enabled && root && !busy && process != null)
                    TextButton(tr("Cancel", "取消任务"), { change("memory:cancel", "") }, Modifier.fillMaxWidth(), enabled = busy)
                }
            }
        }
        item {
            Card {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(tr("Memory view / inferred layout", "内存查看 / 布局推断"), fontSize = 20.sp)
                    Text(tr("Enter a hexadecimal address from scan results. The inspector shows bytes, numeric interpretations, readable-pointer candidates, strings, possible Unity object headers and FieldInfo-based field offsets. These hints do not prove a field's type.",
                        "输入扫描结果中的十六进制地址。查看字节、数值解释、可读指针候选、字符串、可能的 Unity 对象头及基于 FieldInfo 推断的字段偏移。推断提示不代表字段类型已被确认。"))
                    if (matches.isNotEmpty()) SuperDropdown(title = tr("Open a search match (first 128)", "打开搜索匹配（前 128 个）"),
                        selectedIndex = 0, items = listOf(tr("Choose address", "选择地址")) + matches,
                        onSelectedIndexChange = { index -> matches.getOrNull(index - 1)?.let { address = it; request("view") } })
                    TextField(value = address, onValueChange = { if (it.length <= 18) address = it },
                        modifier = Modifier.fillMaxWidth().semantics { contentDescription = tr("Memory address", "内存地址") })
                    SuperDropdown(title = tr("Read length", "读取长度"), selectedIndex = viewSize,
                        items = listOf("256 B", "1 KiB", "4 KiB"), onSelectedIndexChange = { viewSize = it })
                    TextButton(tr("Read / refresh memory", "读取 / 刷新内存"), { request("view") }, Modifier.fillMaxWidth(),
                        enabled = enabled && root && !busy && process != null && address.isNotBlank())
                    if (pointers.isNotEmpty()) SuperDropdown(title = tr("Follow a pointer candidate", "跟随指针候选"),
                        selectedIndex = 0, items = listOf(tr("Choose pointer", "选择指针")) +
                            pointers.map { "${it.optString("label")} → ${it.optString("address")}" },
                        onSelectedIndexChange = { index ->
                            pointers.getOrNull(index - 1)?.let {
                                address = it.getString("address")
                                request("view")
                            }
                        })
                    if (viewHex.isNotEmpty()) {
                        Text(tr("Captured address: ", "已读取地址：") + memory.optString("viewAddress"))
                        Text(tr("Orange bytes changed since the previous read of this same process and address.",
                            "橙色字节表示与同一进程、同一地址的上次读取相比发生变化。"), fontSize = 13.sp)
                        Column(Modifier.horizontalScroll(rememberScrollState())) {
                            viewHex.chunked(32).forEachIndexed { row, line ->
                                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    Text("+%04X".format(row * 16), fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                                    line.chunked(2).forEachIndexed { column, byte ->
                                        Text(byte.uppercase(), fontFamily = FontFamily.Monospace, fontSize = 12.sp,
                                            color = if (row * 16 + column in changed) Color(0xFFFF9B45) else top.yukonga.miuix.kmp.theme.MiuixTheme.colorScheme.onSurface)
                                    }
                                    Text(line.chunked(2).map { it.toInt(16).let { b -> if (b in 32..126) b.toChar() else '.' } }.joinToString(""),
                                        fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                                }
                            }
                        }
                    }
                }
            }
        }
        item {
            Card {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    val status = when (memory.optString("status")) {
                        "running" -> tr("Running…", "执行中…")
                        "finished" -> tr("Finished — check coverage in the report", "已结束，请查看报告中的覆盖范围")
                        "failed" -> tr("Failed", "失败")
                        "cancelled" -> tr("Cancelled", "已取消")
                        else -> tr("Idle", "空闲")
                    }
                    Text(status, fontSize = 18.sp)
                    state.error?.let { Text(it) }
                    TextButton(tr("Export full result as .txt", "导出完整结果为 .txt"), {
                        exportText = report
                        export.launch("VulkanScope-memory.txt")
                    }, Modifier.fillMaxWidth(), enabled = !busy && report.isNotEmpty())
                    if (exportStatus.isNotEmpty()) Text(exportStatus)
                    Text(tr("Preview shows the first 150 lines. Export includes all retained results.",
                        "预览显示前 150 行，导出包含本次保留的全部结果。"), fontSize = 13.sp)
                }
            }
        }
        itemsIndexed(report.lineSequence().take(150).toList()) { _, line ->
            SelectionContainer { Text(line, fontFamily = FontFamily.Monospace, fontSize = 12.sp, modifier = Modifier.padding(horizontal = 8.dp)) }
        }
    }
}
