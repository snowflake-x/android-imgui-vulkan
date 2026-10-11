package dev.snowflake.vulkan

import java.util.Locale

/**
 * Measures the foreground application's presented buffers through SurfaceFlinger.
 * Requires user-enabled root; display refresh rate and the HUD renderer FPS are never substitutes.
 */
class ForegroundFpsCollector : MetricCollector {
    private val root = RootCommand()
    private val frameStats = FrameStats()
    @Volatile private var closed = false
    private var rootWasEnabled = false
    private var authorization = "unknown"
    private var authorizationError = ""
    private var foreground: Foreground? = null
    private var foregroundReadAt = 0L
    private var surface = ""
    private var surfaceScannedAt = 0L
    private var previousOverride = ""
    private var latencyFailures = 0

    /**
     * 在宿主采样线程读取当前配置所需的指标，返回独立快照。
     */
    override fun sample(config: HudConfig, previous: Map<String, String>): Map<String, String> {
        if (closed) return emptyMap()
        if (!config.rootEnabled) {
            rootWasEnabled = false
            clearSelection()
            return unavailable("Root disabled", "disabled")
        }
        if (!rootWasEnabled) {
            authorization = "unknown"
            rootWasEnabled = true
        }
        if (authorization == "unknown") {
            val result = root.run("id -u", 30_000)
            if (result.success && result.output.lineSequence().any { it.trim() == "0" }) {
                authorization = "granted"
                authorizationError = ""
            } else {
                authorization = if (result.error.startsWith("su unavailable")) "unavailable" else "denied"
                authorizationError = result.error.ifBlank { "Root access not granted" }
                clearSelection()
            }
        }
        if (authorization != "granted") return unavailable(authorizationError, authorization)
        val now = System.nanoTime()
        if (foregroundReadAt == 0L || now - foregroundReadAt >= 750_000_000L) {
            val detected = findForeground()
            foregroundReadAt = System.nanoTime()
            if (detected != foreground) {
                foreground = detected
                surface = ""
                surfaceScannedAt = 0
                frameStats.reset()
            }
        }
        val app = foreground ?: return unavailable("No external foreground app", "granted")
        val values = linkedMapOf("root_status" to "granted", "foreground" to app.packageName,
            "foreground_user" to app.userId.toString())
        val needsFrames = arrayOf("fps", "frame_time", "frame_p95", "fps_source", "surface").any(config::isEnabled)
        if (!needsFrames) return values
        if (config.surfaceOverride != previousOverride) {
            previousOverride = config.surfaceOverride
            surface = ""
            surfaceScannedAt = 0
            frameStats.reset()
        }
        var selectedTrace: FrameStats.Trace? = null
        var sampledAt = System.nanoTime()
        if (surface.isEmpty() || sampledAt - surfaceScannedAt >= 5_000_000_000L || latencyFailures >= 2) {
            val selection = selectSurface(app.packageName, config.surfaceOverride)
            surfaceScannedAt = System.nanoTime()
            if (selection == null) {
                surface = ""
                frameStats.reset()
                values.putAll(unavailableFrames(if (config.surfaceOverride.isBlank())
                    "No matching app surface" else "Configured surface not found"))
                return values
            }
            if (surface != selection.name) {
                frameStats.reset()
                surface = selection.name
            }
            selectedTrace = selection.trace
            sampledAt = selection.sampledAt
        }
        if (selectedTrace == null) {
            val latency = root.run("dumpsys SurfaceFlinger --latency ${RootCommand.quote(surface)}", 2000)
            sampledAt = System.nanoTime()
            selectedTrace = FrameStats.parseLatency(if (latency.success) latency.output else "", sampledAt)
            if (!latency.success) {
                values.putAll(unavailableFrames(latency.error))
                values["surface"] = surface
                frameStats.reset()
                latencyFailures++
                return values
            }
        }
        val sample = frameStats.update("${app.userId}:${app.packageName}:$surface", selectedTrace, sampledAt)
        values["surface"] = surface
        if (!sample.supported || !sample.fps.isFinite()) {
            latencyFailures = if (sample.warmingUp) 0 else latencyFailures + 1
            values.putAll(unavailableFrames(sample.error))
            values["surface"] = surface
            values["fps_source"] = if (sample.warmingUp) "SurfaceFlinger: collecting" else "N/A (${sample.error})"
            return values
        }
        latencyFailures = 0
        values["fps"] = format(sample.fps)
        values["frame_time"] = if (sample.frameTimeMs.isFinite()) "${format(sample.frameTimeMs)} ms" else "N/A (idle)"
        values["frame_p95"] = if (sample.frameP95Ms.isFinite()) "${format(sample.frameP95Ms)} ms" else "N/A (idle)"
        values["fps_source"] = if (sample.truncatedWindow)
            "SurfaceFlinger actual-present (recent ring; shorter window)"
        else "SurfaceFlinger actual-present"
        return values
    }

    private fun findForeground(): Foreground? {
        val activities = root.run("dumpsys activity activities | grep -E 'topResumedActivity|mTopResumedActivity|mResumedActivity|ResumedActivity'", 2000)
        val resumed = parseForeground(activities.output, true)
        if (resumed != null) return resumed.takeUnless { it.packageName == SELF_PACKAGE }
        val windows = root.run("dumpsys window windows | grep -E 'mCurrentFocus|mFocusedApp'", 2000)
        return parseForeground(windows.output, false)?.takeUnless { it.packageName == SELF_PACKAGE }
    }

    private fun selectSurface(packageName: String, override: String): Selection? {
        val listed = root.run("dumpsys SurfaceFlinger --list", 2000)
        if (!listed.success) return null
        val candidates = candidateSurfaces(listed.output, packageName, override)
        var selected: Selection? = null
        for (name in candidates) {
            if (closed) return null
            val result = root.run("dumpsys SurfaceFlinger --latency ${RootCommand.quote(name)}", 1500)
            val sampledAt = System.nanoTime()
            val trace = FrameStats.parseLatency(if (result.success) result.output else "", sampledAt)
            val candidate = Selection(name, trace, sampledAt)
            val prior = selected
            if (prior == null || selectionPriority(candidate) > selectionPriority(prior) ||
                (selectionPriority(candidate) == selectionPriority(prior) && presentationAge(candidate) < presentationAge(prior))) {
                selected = candidate
            }
        }
        return selected
    }

    /**
     * Compares freshness against each probe time so later serial queries have no built-in advantage.
     */
    private fun presentationAge(selection: Selection): Long =
        selection.trace.presents.lastOrNull()?.let { selection.sampledAt - it } ?: Long.MAX_VALUE

    private fun selectionPriority(selection: Selection): Int =
        surfaceSelectionPriority(selection.name, selection.trace.supported, presentationAge(selection))

    private fun clearSelection() {
        foreground = null
        foregroundReadAt = 0
        surface = ""
        surfaceScannedAt = 0
        latencyFailures = 0
        frameStats.reset()
    }

    private fun unavailable(reason: String, rootState: String): Map<String, String> =
        unavailableFrames(reason).toMutableMap().apply {
            put("root_status", rootState)
            put("foreground", "N/A ($reason)")
        }

    private fun unavailableFrames(reason: String): Map<String, String> =
        listOf("fps", "frame_time", "frame_p95", "fps_source", "surface").associateWith { "N/A ($reason)" }

    private fun format(value: Double): String = String.format(Locale.US, "%.1f", value)

    /**
     * Resets presentation baselines on screen-on without repeating root authorization.
     */
    fun resetSampling() {
        clearSelection()
    }

    /**
     * 取消当前 root 请求并禁止新的查询，可从服务销毁线程调用。
     */
    override fun close() {
        closed = true
        root.close()
    }

    internal data class Foreground(val packageName: String, val userId: Int)
    private data class Selection(val name: String, val trace: FrameStats.Trace, val sampledAt: Long)

    companion object {
        private const val SELF_PACKAGE = "dev.snowflake.vulkan"
        private val COMPONENT = Regex("\\bu(\\d+)\\s+([A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+)/[A-Za-z0-9_.$]+")

        /**
         * Parses explicit foreground records only, never arbitrary background activity entries.
         */
        internal fun parseForeground(output: String, activities: Boolean): Foreground? {
            val candidates = output.lineSequence().mapNotNull { line ->
                val priority = if (activities) when {
                    Regex("\\b(?:topResumedActivity|mTopResumedActivity)\\b").containsMatchIn(line) -> 3
                    Regex("\\bmResumedActivity\\b").containsMatchIn(line) -> 2
                    Regex("\\bResumedActivity\\b").containsMatchIn(line) -> 1
                    else -> 0
                } else when {
                    line.contains("mCurrentFocus") -> 2
                    line.contains("mFocusedApp") -> 1
                    else -> 0
                }
                if (priority == 0) return@mapNotNull null
                val match = COMPONENT.find(line) ?: return@mapNotNull null
                val userId = match.groupValues[1].toIntOrNull() ?: return@mapNotNull null
                Pair(Foreground(match.groupValues[2], userId), priority)
            }.toList()
            val applicable = if (activities) candidates else candidates.filter { it.first.packageName != SELF_PACKAGE }
            return applicable.maxByOrNull { it.second }?.first
        }

        /**
         * Matches a package token without confusing similarly named applications.
         */
        internal fun containsPackage(layer: String, packageName: String): Boolean =
            Regex("(?:^|[^A-Za-z0-9_.])${Regex.escape(packageName)}(?=$|[^A-Za-z0-9_.])").containsMatchIn(layer)

        /**
         * Binds even explicit layer overrides to the current foreground package.
         */
        internal fun candidateSurfaces(output: String, packageName: String, override: String): List<String> {
            val names = output.lineSequence().map(String::trim).filter(String::isNotEmpty).distinct()
                .filter { !containsPackage(it, SELF_PACKAGE) && containsPackage(it, packageName) }
            return if (override.isNotBlank()) names.filter { it == override }.toList()
            else names.sortedByDescending(::surfacePriority).take(4).toList()
        }

        /**
         * Prefers active game content over window animations while allowing an active fallback.
         */
        internal fun surfaceSelectionPriority(name: String, supported: Boolean, ageNanos: Long): Int {
            if (!supported) return -1000
            return (if (ageNanos <= 1_000_000_000L) 1000 else 0) + surfacePriority(name)
        }

        private fun surfacePriority(name: String): Int =
            (if (name.contains("SurfaceView", true)) 100 else 0) +
                (if (name.contains("BLAST", true)) 20 else 0) -
                (if (name.contains("Splash", true) || name.contains("Starting", true)) 100 else 0)
    }
}
