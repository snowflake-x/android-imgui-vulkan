package dev.snowflake.vulkan

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import org.json.JSONArray
import org.json.JSONObject
import org.json.JSONTokener
import top.yukonga.miuix.kmp.basic.*
import top.yukonga.miuix.kmp.extra.SuperDropdown

/** Presentation only: the native worker owns root attach, target lifetime and IL2CPP operations. */
@Composable
internal fun UnityInspectorPage(state: ManagerState, change: (String, String) -> Unit, chinese: Boolean,
    modifier: Modifier, padding: PaddingValues, initialTarget: String, back: () -> Unit) {
    fun tr(en: String, zh: String) = if (chinese) zh else en
    fun rows(array: JSONArray?) = List(array?.length() ?: 0) { array!!.getJSONObject(it) }
    val memory = state.data.optJSONObject("memory") ?: JSONObject()
    val processes = rows(memory.optJSONArray("processes"))
    val busy = memory.optBoolean("busy")
    val enabled = state.config.optInt("managerPlugins", 7) and 8 != 0 && state.data.optString("rootAccess") == "granted"
    var target by rememberSaveable { mutableStateOf(initialTarget) }
    val process = processes.firstOrNull { "${it.optInt("pid")}:${it.optString("start")}" == target }
    var session by remember { mutableStateOf("") }
    var assemblies by remember { mutableStateOf(emptyList<JSONObject>()) }
    var classes by remember { mutableStateOf(emptyList<JSONObject>()) }
    var objects by remember { mutableStateOf(emptyList<JSONObject>()) }
    var fields by remember { mutableStateOf(emptyList<JSONObject>()) }
    var methods by remember { mutableStateOf(emptyList<JSONObject>()) }
    var image by remember { mutableStateOf("") }
    var klass by remember { mutableStateOf("") }
    var instance by remember { mutableStateOf("") }
    var field by remember { mutableStateOf("") }
    var method by remember { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf("") }
    var next by remember { mutableIntStateOf(0) }
    var total by remember { mutableIntStateOf(0) }
    var fieldValue by rememberSaveable { mutableStateOf("null") }
    var arguments by rememberSaveable { mutableStateOf("[]") }
    var hookPolicy by rememberSaveable { mutableStateOf("{}") }
    var memoryAddress by rememberSaveable { mutableStateOf("") }
    var memoryBytes by rememberSaveable { mutableStateOf("") }
    var result by remember { mutableStateOf("") }
    val ready = enabled && !busy && process != null && session.isNotEmpty()
    val latestBusy by rememberUpdatedState(busy)
    val latestEnabled by rememberUpdatedState(enabled)
    fun clear() {
        assemblies = emptyList(); classes = emptyList(); objects = emptyList(); fields = emptyList(); methods = emptyList()
        image = ""; klass = ""; instance = ""; field = ""; method = ""; session = ""; next = 0; total = 0
    }
    fun send(op: String, fill: (JSONObject) -> Unit = {}) {
        val p = process ?: return
        try {
            val q = JSONObject().put("op", op)
            if (session.isNotEmpty()) q.put("session", session)
            fill(q)
            val bytes = q.toString().toByteArray(Charsets.UTF_8)
            require(bytes.size <= 8192) { tr("Request exceeds 8 KiB", "请求超过 8 KiB") }
            val encoded = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
            change("memory:runtime", "${p.getInt("pid")} ${p.getString("start")} json $encoded")
        } catch (e: Exception) { result = e.message ?: "Invalid input" }
    }
    val latestSend by rememberUpdatedState<(String) -> Unit>({ send(it) })
    BackHandler { if (ready) send("detach"); back() }
    LaunchedEffect(target) { clear() }
    LaunchedEffect(memory.optString("runtimeReport")) {
        val r = runCatching { JSONObject(memory.optString("runtimeReport")) }.getOrNull() ?: return@LaunchedEffect
        if (r.optString("target") != target) return@LaunchedEffect
        val newSession = r.optString("session")
        if (newSession.isNotEmpty() && newSession != session) { clear(); session = newSession }
        if (r.optBoolean("detached")) clear()
        when (r.optString("kind")) {
            "assemblies" -> assemblies = rows(r.optJSONArray("rows"))
            "classes" -> { classes = rows(r.optJSONArray("rows")); next = r.optInt("next"); total = r.optInt("total") }
            "objects" -> objects = rows(r.optJSONArray("rows"))
            "members" -> { fields = rows(r.optJSONArray("fields")); methods = rows(r.optJSONArray("methods")); field = ""; method = "" }
            "memory" -> memoryBytes = r.optString("bytes")
            "value" -> fieldValue = r.opt("value")?.let { if (it is String) JSONObject.quote(it) else it.toString() } ?: "null"
        }
        val reference = r.optJSONObject("value")
        if (reference != null && reference.has("object")) {
            reference.put("name", reference.optString("type"))
            objects = objects.filter { it.optString("object") != reference.optString("object") } + reference
        }
        if (r.optString("kind") != "ping") result = r.toString(2)
    }
    LaunchedEffect(target, session) {
        if (session.isNotEmpty()) while (true) {
            delay(15000)
            if (!latestBusy && latestEnabled) latestSend("ping")
        }
    }
    @Composable fun picker(title: String, values: List<JSONObject>, selected: String, key: String = "id", choose: (JSONObject) -> Unit) {
        SuperDropdown(title = title, items = listOf(tr("Select", "请选择")) + values.map { row ->
            row.optString("name", row.optString("type")) + row.optJSONArray("params")?.let { p ->
                "(" + rows(p).joinToString { it.optString("type") } + ")"
            }.orEmpty() + " · " + row.optString(key)
        }, selectedIndex = values.indexOfFirst { it.optString(key) == selected } + 1,
            onSelectedIndexChange = { if (!busy) values.getOrNull(it - 1)?.let(choose) })
    }
    LazyColumn(modifier, contentPadding = padding, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Unity Runtime Inspector", fontSize = 22.sp)
                Text(tr("Attach to a running IL2CPP app to browse objects, fields and methods. Writes, calls and hooks change the target immediately. Keep Unity running for main-thread operations; a timed-out call may already have executed.",
                    "附加运行中的 IL2CPP 应用，浏览对象、字段和方法。写入、调用与 Hook 会立即改变目标。主线程操作需要 Unity 保持运行；调用超时不代表没有执行。"))
                TextButton(tr("Back to memory scanner", "返回内存扫描"), { if (ready) send("detach"); back() })
                TextButton(tr("Refresh processes", "刷新进程"), { clear(); change("memory:list", "") }, enabled = enabled && !busy)
                SuperDropdown(title = tr("Target process", "目标进程"),
                    items = listOf(tr("Select", "请选择")) + processes.map { "${it.optString("name")} · ${it.optInt("pid")}" },
                    selectedIndex = processes.indexOf(process) + 1, onSelectedIndexChange = {
                        if (!busy) { if (ready) send("detach"); clear(); target = processes.getOrNull(it - 1)?.let { p -> "${p.optInt("pid")}:${p.optString("start")}" } ?: "" }
                    })
                TextButton(tr("Attach / reconnect", "附加 / 重新连接"), { send("connect") }, enabled = enabled && !busy && process != null)
                TextButton(tr("Disconnect and remove hooks", "断开并移除 Hook"), { send("detach") }, enabled = ready)
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Assemblies / classes", "程序集 / 类"), fontSize = 20.sp)
                picker(tr("Assembly", "程序集"), assemblies, image) { image = it.getString("id"); klass = ""; classes = emptyList(); send("classes") { q -> q.put("image", image).put("filter", filter) } }
                TextField(filter, { if (it.length <= 128) filter = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Class filter" })
                TextButton(tr("Search classes", "搜索类"), { send("classes") { it.put("image", image).put("filter", filter) } }, enabled = ready && image.isNotEmpty())
                picker(tr("Class", "类"), classes, klass) { klass = it.getString("id"); send("members") { q -> q.put("class", klass) } }
                Text(tr("Scanned class entries: ", "已浏览类型条目：") + "$next / $total")
                TextButton(tr("Next class page", "下一页类型"), { send("classes") { it.put("image", image).put("filter", filter).put("offset", next) } }, enabled = ready && next < total)
                TextButton(tr("Find scene objects of this class", "查找此类的场景对象"), { send("objects") { it.put("class", klass).put("inactive", true) } }, enabled = ready && klass.isNotEmpty())
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Live objects / components", "运行中对象 / 组件"), fontSize = 20.sp)
                picker(tr("Object instance", "对象实例"), objects, instance, "object") {
                    memoryAddress = it.optString("address"); instance = it.getString("object"); klass = it.optString("class", klass)
                    send("members") { q -> q.put("class", klass) }
                }
                Text(tr("Selected object handle: ", "当前对象句柄：") + instance)
                TextButton(tr("Browse GameObject components", "浏览 GameObject 的组件"), { send("components") { it.put("object", instance) } }, enabled = ready && instance.isNotEmpty())
                TextButton(tr("Release selected object", "释放所选对象引用"), { send("release") { it.put("object", instance) }; objects = objects.filter { it.optString("object") != instance }; instance = "" }, enabled = ready && instance.isNotEmpty())
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Fields", "字段"), fontSize = 20.sp)
                picker(tr("Field", "字段"), fields, field) { field = it.getString("id") }
                fields.firstOrNull { it.optString("id") == field }?.let {
                    Text("${it.optString("type")} · +0x${it.optLong("offset").toString(16)} · flags 0x${it.optInt("flags").toString(16)}")
                }
                TextButton(tr("Read current value", "读取当前值"), { send("read") { it.put("field", field).put("object", instance) } }, enabled = ready && field.isNotEmpty())
                Text(tr("JSON value: 42, true, \"text\", null, {\"object\":\"id\"}; structs: {\"bytes\":\"hex\"}.", "JSON 值：42、true、\"文本\"、null、{\"object\":\"句柄\"}；结构体：{\"bytes\":\"十六进制\"}。"))
                TextField(fieldValue, { if (it.length <= 8192) fieldValue = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Field value" })
                TextButton(tr("Write field now", "立即写入字段"), { send("write") { it.put("field", field).put("object", instance).put("value", JSONTokener(fieldValue).nextValue()) } }, enabled = ready && field.isNotEmpty())
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Methods / hooks", "方法 / Hook"), fontSize = 20.sp)
                picker(tr("Method and overload", "方法与重载"), methods, method) { method = it.getString("id") }
                methods.firstOrNull { it.optString("id") == method }?.let { Text("${it.optString("return")} · ${it.optString("entry")}") }
                Text(tr("Arguments in signature order, as a JSON array. Result includes return value and ref/out values.", "按签名顺序填写 JSON 参数数组。结果包含返回值及 ref/out 参数。"))
                TextField(arguments, { if (it.length <= 8192) arguments = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Method arguments" })
                TextButton(tr("Invoke now", "立即调用"), { send("invoke") { it.put("method", method).put("object", instance).put("args", JSONArray(arguments)) } }, enabled = ready && method.isNotEmpty())
                Text(tr("Hook policy: {} counts calls. Typed overrides: {\"arguments\":{\"0\":42},\"return\":1}. Add \"skip\":true to bypass the original method. Struct layouts are validated; shared generic code can affect multiple methods.",
                    "Hook 策略：{} 记录调用次数。按类型覆盖：{\"arguments\":{\"0\":42},\"return\":1}。加入 \"skip\":true 可跳过原方法。结构体先校验布局；共享泛型代码可能影响多个方法。"))
                TextField(hookPolicy, { if (it.length <= 4096) hookPolicy = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Hook policy" })
                TextButton(tr("Install hook", "安装 Hook"), { send("hook") { q -> val policy = JSONObject(hookPolicy); policy.keys().forEach { if (it == "return" || it == "arguments" || it == "skip") q.put(it, policy.get(it)) }; q.put("method", method) } }, enabled = ready && method.isNotEmpty())
                TextButton(tr("Remove selected hook", "移除所选 Hook"), { send("unhook") { it.put("method", method) } }, enabled = ready && method.isNotEmpty())
                TextButton(tr("Refresh hook counters", "刷新 Hook 计数"), { send("hooks") }, enabled = ready)
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Raw memory", "原始内存"), fontSize = 20.sp)
                Text(tr("Hex address; reads 64 bytes. Writes accept up to 1,024 bytes in a writable mapping and take effect immediately.", "十六进制地址；每次读取 64 字节。写入最多 1,024 字节，仅限可写映射，立即生效。"))
                TextField(memoryAddress, { if (it.length <= 18) memoryAddress = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Runtime address" })
                TextButton(tr("Read bytes", "读取字节"), { send("readMemory") { it.put("address", memoryAddress).put("size", 64) } }, enabled = ready && memoryAddress.isNotEmpty())
                TextField(memoryBytes, { if (it.length <= 2048) memoryBytes = it }, Modifier.fillMaxWidth().semantics { contentDescription = "Runtime bytes" })
                TextButton(tr("Write bytes now", "立即写入字节"), { send("writeMemory") { it.put("address", memoryAddress).put("bytes", memoryBytes) } }, enabled = ready && memoryAddress.isNotEmpty() && memoryBytes.isNotEmpty())
            } }
        }
        item {
            Card { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(if (busy) tr("Working…", "执行中…") else tr("Runtime result", "运行时结果"), fontSize = 20.sp)
                SelectionContainer { Text(result.take(20000), fontSize = 12.sp) }
            } }
        }
    }
}
