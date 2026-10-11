package dev.snowflake.vulkan

import android.content.Context
import android.os.SystemClock
import org.json.JSONObject
import org.json.JSONTokener

/**
 * Reads opt-in engine telemetry for the exact foreground Android user and package.
 * Engine-reported internal pixels never fall back to display or compositor dimensions.
 */
class EngineResolutionCollector(
    @Suppress("UNUSED_PARAMETER") context: Context,
    private val adapters: List<EngineResolutionAdapter> = emptyList()
) : MetricCollector {
    private val root = RootCommand()
    private var lastProbeKey = ""
    private var lastProbeMs = 0L
    private var lastProbe: EngineProcess? = null

    /**
     * 在宿主采样线程读取当前配置所需的指标，返回独立快照。
     */
    override fun sample(config: HudConfig, previous: Map<String, String>): Map<String, String> {
        if (!FIELDS.any { config.isEnabled(it) }) return emptyMap()
        if (!config.rootEnabled) return unavailable("enable root for engine inspection")
        if (previous["root_status"] != "granted") return unavailable("root authorization unavailable")
        val packageName = previous["foreground"].orEmpty()
        if (!PACKAGE_NAME.matches(packageName)) return unavailable("foreground package unavailable")
        val user = previous["foreground_user"]?.toIntOrNull()
            ?.takeIf { it in 0..21474 } ?: return unavailable("foreground Android user unavailable")
        val internal = "/data/user/$user/$packageName/files/$FILE_NAME"
        val external = "/storage/emulated/$user/Android/data/$packageName/files/$FILE_NAME"
        val read = root.run(
            "if [ -f ${RootCommand.quote(internal)} ]; then head -c 4097 ${RootCommand.quote(internal)}; " +
                "elif [ -f ${RootCommand.quote(external)} ]; then head -c 4097 ${RootCommand.quote(external)}; " +
                "else printf 'VULKANSCOPE_NO_ENGINE_REPORT'; fi", 1500
        )
        if (!read.success) return unavailable("engine report unreadable: ${read.error}")
        if (read.output == "VULKANSCOPE_NO_ENGINE_REPORT") return readInstalledGame(packageName, user)
        val report = try {
            EngineTelemetry.parse(read.output, packageName, SystemClock.elapsedRealtime())
        } catch (exception: IllegalArgumentException) {
            return unavailable(exception.message ?: "invalid engine report")
        }
        val process = root.run(
            "head -c 256 /proc/${report.pid}/cmdline; printf '\\n'; " +
                "sed -n '/^Uid:/p' /proc/${report.pid}/status", 1500
        )
        if (!process.success) return unavailable("engine process unavailable")
        val processName = process.output.substringBefore('\u0000').substringBefore('\n')
        val processUid = Regex("(?m)^Uid:\\s+(\\d+)").find(process.output)
            ?.groupValues?.get(1)?.toLongOrNull()
        if ((processName != packageName && !processName.startsWith("$packageName:")) ||
            processUid == null || processUid / 100000 != user.toLong()) {
            return unavailable("engine process does not match foreground app")
        }
        if (SystemClock.elapsedRealtime() - report.elapsedRealtimeMs > EngineTelemetry.MAX_AGE_MS) {
            return unavailable("engine report expired")
        }
        return mapOf(
            "engine_resolution" to "${report.width} x ${report.height} px",
            "engine_output" to if (report.outputWidth != null) {
                "${report.outputWidth} x ${report.outputHeight} px"
            } else "N/A (not reported by engine)",
            "engine_source" to "${report.engine} / ${report.source} (engine-reported)"
        )
    }

    private fun readInstalledGame(packageName: String, user: Int): Map<String, String> {
        val target = probeInstalledGame(packageName, user)
            ?: return unavailable("no readable Unity/Unreal process found")
        val adapter = adapters.firstOrNull { it.supports(target) }
        val reading = adapter?.read(target, root)
        val game = when (target.packageName) {
            "com.tencent.tmgp.sgame" -> "Honor of Kings"
            "com.tencent.tmgp.pubgmhd" -> "Game for Peace"
            else -> target.packageName
        }
        val source = "$game / ${target.engine} / versionCode=${target.versionCode ?: "unknown"}" +
            " / pid=${target.pid} / user=${target.androidUser}"
        if (target.engine == "Unknown") return unavailable("engine library unreadable or unsupported") +
            ("engine_source" to source)
        if (reading == null) return unavailable("no compatible ${target.engine} resolution reader") +
            ("engine_source" to source)
        if (reading.width !in 1..32768 || reading.height !in 1..32768 ||
            reading.elapsedRealtimeMs > SystemClock.elapsedRealtime() ||
            SystemClock.elapsedRealtime() - reading.elapsedRealtimeMs > EngineTelemetry.MAX_AGE_MS) {
            return unavailable("invalid or expired engine reader sample") + ("engine_source" to source)
        }
        return mapOf(
            "engine_resolution" to "${reading.width} x ${reading.height} px",
            "engine_output" to "N/A (not reported by engine reader)",
            "engine_source" to "$source / ${reading.source}"
        )
    }

    /**
     * Detects engine libraries without exposing memory maps or guessing engine object offsets.
     */
    private fun probeInstalledGame(packageName: String, user: Int): EngineProcess? {
        val key = "$user:$packageName"
        val now = SystemClock.elapsedRealtime()
        if (key == lastProbeKey && now - lastProbeMs < 3000) return lastProbe
        lastProbeKey = key
        lastProbeMs = now
        lastProbe = null
        val script = EngineProcess.probeCommand(packageName, user)
        val result = root.run(script, 2000)
        if (!result.success) return null
        return EngineProcess.parseProbe(packageName, user, result.output).also { lastProbe = it }
    }

    private fun unavailable(reason: String) = FIELDS.associateWith { "N/A ($reason)" }

    /**
     * 取消当前 root 请求并禁止新的查询，可从服务销毁线程调用。
     */
    override fun close() = root.close()

    companion object {
        private const val FILE_NAME = "vulkanscope-render.json"
        private val PACKAGE_NAME = Regex("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")
        private val FIELDS = listOf("engine_resolution", "engine_output", "engine_source")
    }
}

/**
 * Validates bounded, fresh, versioned engine reports without coercing malformed JSON fields.
 */
internal data class EngineTelemetry(
    val engine: String,
    val source: String,
    val pid: Int,
    val elapsedRealtimeMs: Long,
    val width: Int,
    val height: Int,
    val outputWidth: Int?,
    val outputHeight: Int?
) {
    companion object {
        const val MAX_AGE_MS = 5000L

        fun parse(json: String, foregroundPackage: String, nowMs: Long): EngineTelemetry {
            require(json.toByteArray(Charsets.UTF_8).size <= 4096) { "engine report exceeds 4 KiB" }
            val value = try {
                val tokenizer = JSONTokener(json)
                val parsed = tokenizer.nextValue() as? JSONObject
                    ?: throw IllegalArgumentException("engine report is not an object")
                require(tokenizer.nextClean() == '\u0000')
                parsed
            } catch (_: Exception) {
                throw IllegalArgumentException("malformed engine report")
            }
            require(integer(value, "schemaVersion") == 1L) { "unsupported engine report version" }
            require(value.opt("packageName") == foregroundPackage) { "engine report is for a different app" }
            val engine = value.opt("engine") as? String
            require(engine == "Unity" || engine == "Unreal") { "unsupported engine report source" }
            val source = value.opt("source") as? String
            require(source != null && source.isNotBlank() && source.length <= 128 && source.all { it.code in 32..126 }) {
                "invalid engine measurement source"
            }
            val stamp = integer(value, "elapsedRealtimeMs")
            require(stamp >= 0 && stamp <= nowMs && nowMs - stamp <= MAX_AGE_MS) { "engine report expired or clock invalid" }
            val pid = integer(value, "pid")
            require(pid in 1..Int.MAX_VALUE.toLong()) { "invalid engine process ID" }
            val width = dimension(value, "renderWidth")
            val height = dimension(value, "renderHeight")
            val hasOutput = value.has("outputWidth") || value.has("outputHeight")
            return EngineTelemetry(
                engine, source, pid.toInt(), stamp, width, height,
                if (hasOutput) dimension(value, "outputWidth") else null,
                if (hasOutput) dimension(value, "outputHeight") else null
            )
        }

        private fun integer(value: JSONObject, name: String): Long {
            val number = value.opt(name)
            require(number is Int || number is Long) { "invalid engine field: $name" }
            return (number as Number).toLong()
        }

        private fun dimension(value: JSONObject, name: String): Int {
            val pixels = integer(value, name)
            require(pixels in 1..32768) { "invalid engine dimensions: $name" }
            return pixels.toInt()
        }
    }
}
