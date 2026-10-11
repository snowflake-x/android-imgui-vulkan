package dev.snowflake.vulkan

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.display.DisplayManager
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.os.SystemClock
import android.view.Display
import java.io.File
import java.util.Collections
import java.util.Locale
import kotlin.math.abs

/**
 * Samples device-wide hardware metrics for the existing native HUD plugin.
 * sample runs on one worker; close may run concurrently to cancel privileged reads.
 */
class HardwareCollector(context: Context) : MetricCollector {
    private val app = context.applicationContext
    private val preferences = app.getSharedPreferences("performance_hud", Context.MODE_PRIVATE)
    private val root = RootCommand()
    @Volatile private var closed = false
    private var cpuPrevious: LongArray? = null
    private var rootRetryAfter = 0L
    private var rootFailure = ""

    /**
     * 在宿主采样线程读取当前配置所需的指标，返回独立快照。
     */
    override fun sample(config: HudConfig, previous: Map<String, String>): Map<String, String> {
        if (closed) return emptyMap()
        val result = linkedMapOf<String, String>()
        val files = readHardware(config, previous["root_status"])
        for (id in listOf("gpu_model", "gpu_driver", "gpu_api")) {
            if (config.isEnabled(id)) {
                result[id] = preferences.getString(id, null)?.takeIf { it.isNotBlank() }
                    ?: "N/A · open native renderer to identify GPU"
            }
        }
        if (config.isEnabled("cpu_freq")) result["cpu_freq"] = cpuFrequencies(files, config)
        if (config.isEnabled("cpu_load")) {
            val current = HardwareUnits.cpuTicks(files[CPU_STAT])
            val before = cpuPrevious
            cpuPrevious = current
            result["cpu_load"] = when {
                current == null -> unavailable(config, "CPU counters inaccessible")
                before == null -> "N/A · waiting for next CPU sample"
                else -> HardwareUnits.cpuLoad(before, current)?.let { format("%.1f%% · all CPUs", it) }
                    ?: "N/A · CPU counters unchanged or reset"
            }
        } else {
            cpuPrevious = null
        }
        if (config.isEnabled("cpu_power")) {
            result["cpu_power"] = when {
                config.cpuPowerPath.isBlank() -> "N/A · configure a measured CPU power sensor"
                !validPowerPath(config.cpuPowerPath) -> "N/A · sensor must be a /sys/ path"
                !config.cpuPowerScale.isFinite() || config.cpuPowerScale <= 0 -> "N/A · invalid watts multiplier"
                else -> HardwareUnits.powerWatts(files[config.cpuPowerPath], config.cpuPowerScale)
                    ?.let { format("%.3f W · configured CPU sensor", it) }
                    ?: unavailable(config, "CPU power sensor unreadable or invalid")
            }
        }
        if (config.isEnabled("gpu_freq")) {
            val frequency = files[GPU_CLOCK]?.toLongOrNull()
                ?: files.entries.firstOrNull { isGpuDevfreq(it.key) && it.key.endsWith("/cur_freq") }
                    ?.value?.toLongOrNull()
            result["gpu_freq"] = frequency?.takeIf { it >= 0L }
                ?.let { format("%.0f MHz · GPU clock", it / 1_000_000.0) }
                ?: unavailable(config, "GPU clock sensor unsupported or inaccessible")
        }
        if (config.isEnabled("gpu_load")) {
            result["gpu_load"] = HardwareUnits.gpuBusy(files[GPU_BUSY])
                ?.let { format("%.1f%% · Adreno busy/total", it) }
                ?: unavailable(config, "GPU busy counters unsupported or inactive")
        }
        if (config.isEnabled("memory")) {
            result["memory"] = runCatching {
                val info = ActivityManager.MemoryInfo()
                (app.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(info)
                if (info.totalMem <= 0) "N/A · RAM totals unavailable" else format(
                    "%.2f / %.2f GiB · device used/total",
                    (info.totalMem - info.availMem) / GIB, info.totalMem / GIB
                )
            }.getOrDefault("N/A · RAM query unavailable")
        }
        if (config.isEnabled("battery_power") || config.isEnabled("battery_temp")) {
            sampleBattery(config, result)
        }
        if (config.isEnabled("thermal")) result["thermal"] = thermalStatus()
        if (config.isEnabled("display_resolution") || config.isEnabled("display_refresh")) {
            sampleDisplay(config, result)
        }
        return Collections.unmodifiableMap(result)
    }

    /**
     * Reads ordinary files first; one bounded root batch fills inaccessible enabled metrics.
     */
    private fun readHardware(config: HudConfig, rootStatus: String?): Map<String, String> {
        val values = linkedMapOf<String, String>()
        val paths = linkedSetOf<String>()
        if (config.isEnabled("cpu_load")) paths += CPU_STAT
        if (config.isEnabled("cpu_freq")) {
            val policies = File(CPU_POLICIES).listFiles()?.filter { it.name.matches(Regex("policy[0-9]+")) }.orEmpty()
            for (policy in policies) {
                paths += "${policy.path}/scaling_cur_freq"
                paths += "${policy.path}/cpuinfo_cur_freq"
            }
            if (policies.isEmpty()) {
                File(CPU_ROOT).listFiles()?.filter { it.name.matches(Regex("cpu[0-9]+")) }?.forEach {
                    paths += "${it.path}/cpufreq/scaling_cur_freq"
                    paths += "${it.path}/cpufreq/cpuinfo_cur_freq"
                }
            }
        }
        if (config.isEnabled("cpu_power") && validPowerPath(config.cpuPowerPath) &&
            config.cpuPowerScale.isFinite() && config.cpuPowerScale > 0) paths += config.cpuPowerPath
        if (config.isEnabled("gpu_freq")) {
            paths += GPU_CLOCK
            File(DEVFREQ).listFiles()?.filter { isGpuDevfreq(it.path) }?.forEach { paths += "${it.path}/cur_freq" }
        }
        if (config.isEnabled("gpu_load")) paths += GPU_BUSY
        for (path in paths) readLine(path)?.let { values[path] = it }
        if (!config.rootEnabled || closed) {
            rootRetryAfter = 0L
            rootFailure = ""
            return values
        }
        if (rootStatus != "granted") {
            rootFailure = "root ${rootStatus ?: "authorization pending"}"
            return values
        }

        val missing = paths.filter { it !in values }.filterNot {
            (it == GPU_CLOCK && values.keys.any { key -> isGpuDevfreq(key) && key.endsWith("/cur_freq") }) ||
                (it.endsWith("/cpuinfo_cur_freq") && it.replace("cpuinfo_cur_freq", "scaling_cur_freq") in values)
        }
        val discoverCpu = config.isEnabled("cpu_freq") && values.keys.none { it.endsWith("/scaling_cur_freq") || it.endsWith("/cpuinfo_cur_freq") }
        val discoverGpu = config.isEnabled("gpu_freq") && values[GPU_CLOCK] == null &&
            values.keys.none { isGpuDevfreq(it) && it.endsWith("/cur_freq") }
        if (missing.isEmpty() && !discoverCpu && !discoverGpu) return values
        if (SystemClock.elapsedRealtime() < rootRetryAfter) return values
        val script = StringBuilder(
            "hud_read() { [ -r \"\$1\" ] || return; " +
                "hud_v=\$(head -c 1024 \"\$1\" 2>/dev/null | head -n 1); " +
                "[ -n \"\$hud_v\" ] && printf '%s\\t%s\\n' \"\$1\" \"\$hud_v\"; };\n"
        )
        for (path in missing) script.append("hud_read ").append(RootCommand.quote(path)).append('\n')
        if (discoverCpu) {
            script.append("for hud_p in /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq /sys/devices/system/cpu/cpufreq/policy*/cpuinfo_cur_freq /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do hud_read \"\$hud_p\"; done\n")
        }
        if (discoverGpu) {
            script.append("for hud_p in /sys/class/devfreq/*; do case \"\$hud_p\" in *gpu*|*mali*|*kgsl*) hud_read \"\$hud_p/cur_freq\";; esac; done\n")
        }
        script.append("exit 0\n")
        val answer = root.run(script.toString(), 2000)
        if (answer.success) {
            rootFailure = ""
            for (line in answer.output.lineSequence()) {
                val parts = line.split('\t', limit = 2)
                if (parts.size == 2 && (parts[0].startsWith("/sys/") || parts[0] == CPU_STAT)) {
                    values[parts[0]] = parts[1].trim()
                }
            }
        } else {
            rootFailure = answer.error.take(100)
            rootRetryAfter = SystemClock.elapsedRealtime() + 15_000
        }
        return values
    }

    private fun cpuFrequencies(files: Map<String, String>, config: HudConfig): String {
        val groups = linkedMapOf<String, Double>()
        val frequencyFiles = files.filterKeys { it.endsWith("/scaling_cur_freq") || it.endsWith("/cpuinfo_cur_freq") }
        val policyFiles = frequencyFiles.filterKeys { it.startsWith("$CPU_POLICIES/policy") }
        val source = policyFiles.ifEmpty { frequencyFiles.filterKeys { it.startsWith("$CPU_ROOT/cpu") && "/cpufreq/" in it } }
        for ((path, value) in source.entries.sortedBy { if (it.key.endsWith("/scaling_cur_freq")) 0 else 1 }) {
            val khz = value.toLongOrNull()?.takeIf { it > 0 } ?: continue
            val name = path.removePrefix("$CPU_ROOT/").substringBefore('/').let {
                if (it == "cpufreq") path.removePrefix("$CPU_POLICIES/").substringBefore('/') else it
            }
            groups.putIfAbsent(name, khz / 1000.0)
        }
        return groups.entries.sortedBy { it.key.filter(Char::isDigit).toIntOrNull() ?: 0 }
            .joinToString(" · ") { (name, mhz) -> format("%s %.0f MHz", name, mhz) }
            .ifEmpty { unavailable(config, "CPU frequency sensors inaccessible or offline") }
    }

    private fun sampleBattery(config: HudConfig, result: MutableMap<String, String>) {
        val intent = runCatching { app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) }.getOrNull()
        if (config.isEnabled("battery_temp")) {
            val temperature = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            result["battery_temp"] = if (temperature == null || temperature == Int.MIN_VALUE)
                "N/A · battery temperature unavailable" else format("%.1f °C · battery", temperature / 10.0)
        }
        if (config.isEnabled("battery_power")) {
            val microAmps = runCatching {
                (app.getSystemService(Context.BATTERY_SERVICE) as BatteryManager)
                    .getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
            }.getOrDefault(Int.MIN_VALUE)
            val milliVolts = intent?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1) ?: -1
            result["battery_power"] = if (microAmps == Int.MIN_VALUE || milliVolts <= 0) {
                "N/A · battery current/voltage unavailable"
            } else {
                val direction = when {
                    microAmps > 0 -> "charging"
                    microAmps < 0 -> "discharging"
                    else -> "idle"
                }
                format("%.2f W · whole-device battery estimate (%s)", abs(microAmps.toDouble()) * milliVolts / 1_000_000_000.0, direction)
            }
        }
    }

    private fun thermalStatus(): String {
        if (Build.VERSION.SDK_INT < 29) return "N/A · thermal status requires Android 10+"
        return runCatching {
            val status = (app.getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus
            val label = when (status) {
                PowerManager.THERMAL_STATUS_NONE -> "None"
                PowerManager.THERMAL_STATUS_LIGHT -> "Light"
                PowerManager.THERMAL_STATUS_MODERATE -> "Moderate"
                PowerManager.THERMAL_STATUS_SEVERE -> "Severe"
                PowerManager.THERMAL_STATUS_CRITICAL -> "Critical"
                PowerManager.THERMAL_STATUS_EMERGENCY -> "Emergency"
                PowerManager.THERMAL_STATUS_SHUTDOWN -> "Shutdown"
                else -> "Unknown ($status)"
            }
            "$label · Android thermal pressure"
        }.getOrDefault("N/A · thermal service unavailable")
    }

    private fun sampleDisplay(config: HudConfig, result: MutableMap<String, String>) {
        val display = runCatching {
            (app.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).getDisplay(Display.DEFAULT_DISPLAY)
        }.getOrNull()
        if (config.isEnabled("display_resolution")) {
            result["display_resolution"] = display?.mode?.let { "${it.physicalWidth} × ${it.physicalHeight} · display mode" }
                ?: "N/A · display unavailable"
        }
        if (config.isEnabled("display_refresh")) {
            result["display_refresh"] = display?.refreshRate?.takeIf { it.isFinite() && it > 0 }
                ?.let { format("%.2f Hz · display mode", it) } ?: "N/A · display refresh unavailable"
        }
    }

    private fun unavailable(config: HudConfig, reason: String): String = when {
        !config.rootEnabled -> "N/A · $reason (root disabled)"
        rootFailure.isNotBlank() -> "N/A · $reason ($rootFailure)"
        else -> "N/A · $reason"
    }

    /**
     * 取消当前 root 请求并禁止新的查询，可从服务销毁线程调用。
     */
    override fun close() {
        closed = true
        root.close()
    }

    companion object {
        private const val CPU_ROOT = "/sys/devices/system/cpu"
        private const val CPU_POLICIES = "$CPU_ROOT/cpufreq"
        private const val CPU_STAT = "/proc/stat"
        private const val DEVFREQ = "/sys/class/devfreq"
        private const val GPU_CLOCK = "/sys/class/kgsl/kgsl-3d0/gpuclk"
        private const val GPU_BUSY = "/sys/class/kgsl/kgsl-3d0/gpubusy"
        private const val GIB = 1_073_741_824.0

        private fun format(pattern: String, vararg values: Any): String = String.format(Locale.US, pattern, *values)

        private fun readLine(path: String): String? = runCatching {
            File(path).bufferedReader().use { it.readLine()?.take(1024)?.trim()?.takeIf(String::isNotEmpty) }
        }.getOrNull()

        private fun validPowerPath(path: String): Boolean = path.startsWith("/sys/") &&
            path.matches(Regex("/sys/[A-Za-z0-9_./:-]+")) && path.split('/').none { it == ".." }

        /**
         * Only GPU-named Linux devfreq nodes have known Hz units; vendor clock aliases are excluded.
         */
        private fun isGpuDevfreq(path: String): Boolean = path.startsWith("$DEVFREQ/") &&
            listOf("gpu", "mali", "kgsl").any { path.lowercase(Locale.ROOT).contains(it) }
    }
}

/**
 * Pure parsers keep counter deltas and physical-unit conversions independently verifiable.
 */
internal object HardwareUnits {
    /**
     * Linux guest fields are already included in user/nice and must not be added twice.
     */
    fun cpuTicks(line: String?): LongArray? {
        val parts = line?.trim()?.split(Regex("\\s+")) ?: return null
        if (parts.firstOrNull() != "cpu" || parts.size < 5) return null
        val ticks = LongArray(8)
        for (index in 0 until minOf(8, parts.size - 1)) {
            ticks[index] = parts[index + 1].toLongOrNull()?.takeIf { it >= 0 } ?: return null
        }
        return ticks
    }

    /**
     * Rejects reset/unchanged counters instead of fabricating a percentage.
     */
    fun cpuLoad(before: LongArray, after: LongArray): Double? {
        if (before.size != 8 || after.size != 8) return null
        val delta = LongArray(8)
        for (index in delta.indices) {
            if (before[index] < 0 || after[index] < before[index]) return null
            delta[index] = after[index] - before[index]
        }
        val total = delta.sumOf { it.toDouble() }
        if (total <= 0) return null
        val idle = delta[3].toDouble() + delta[4].toDouble()
        return ((total - idle) * 100.0 / total).takeIf { it.isFinite() && it in 0.0..100.0 }
    }

    /**
     * Qualcomm gpubusy exports busy/total time in the same units for its accounting window.
     */
    fun gpuBusy(line: String?): Double? {
        val parts = line?.trim()?.split(Regex("\\s+")) ?: return null
        if (parts.size != 2) return null
        val busy = parts[0].toLongOrNull() ?: return null
        val total = parts[1].toLongOrNull() ?: return null
        if (total <= 0 || busy < 0 || busy > total) return null
        return busy.toDouble() * 100.0 / total
    }

    /**
     * Uses the user-provided sensor unit multiplier; battery power is never a CPU substitute.
     */
    fun powerWatts(raw: String?, scale: Double): Double? {
        if (!scale.isFinite() || scale <= 0) return null
        val measured = raw?.toDoubleOrNull()?.takeIf { it.isFinite() && it >= 0 } ?: return null
        return (measured * scale).takeIf { it.isFinite() }
    }
}
