package dev.snowflake.vulkan

/**
 * Identifies a root-observed process; game version is not an inferred engine version.
 */
data class EngineProcess(
    val packageName: String,
    val androidUser: Int,
    val pid: Int,
    val engine: String,
    val versionCode: Long?
) {
    companion object {
        /**
         * Builds a bounded read-only probe; all external package text is shell-quoted.
         */
        internal fun probeCommand(packageName: String, user: Int): String {
            return "for p in $(pidof ${RootCommand.quote(packageName)}); do " +
                "case \"\$p\" in ''|*[!0-9]*) continue;; esac; " +
                "uid=$(sed -n 's/^Uid:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/\$p/status); " +
                "case \"\$uid\" in ''|*[!0-9]*) continue;; esac; " +
                "[ $((uid / 100000)) -eq $user ] || continue; " +
                "lib=$(head -c 1048576 /proc/\$p/maps 2>/dev/null | " +
                "grep -Eo '/lib(unity|UE4|Unreal)\\.so([[:space:]]|$)' | head -n 1); " +
                "printf '%s\\n%s\\n' \"\$p\" \"\${lib:-unknown}\"; break; done; " +
                "dumpsys package ${RootCommand.quote(packageName)} | " +
                "sed -n 's/^[[:space:]]*versionCode=\\([0-9]*\\).*/versionCode=\\1/p' | head -n 1"
        }

        internal fun parseProbe(packageName: String, user: Int, output: String): EngineProcess? {
            val lines = output.lineSequence().map { it.trim() }.toList()
            val pid = lines.firstOrNull()?.toIntOrNull()?.takeIf { it > 0 } ?: return null
            if (lines.size < 2) return null
            val engine = when (lines[1]) {
                "/libunity.so" -> "Unity"
                "/libUE4.so", "/libUnreal.so" -> "Unreal"
                "unknown" -> "Unknown"
                else -> return null
            }
            val versionCode = lines.firstOrNull { it.startsWith("versionCode=") }
                ?.substringAfter('=')?.toLongOrNull()?.takeIf { it >= 0 }
            return EngineProcess(packageName, user, pid, engine, versionCode)
        }
    }
}

/**
 * Holds verified internal viewport pixels sampled before an engine's upscale pass.
 */
data class EngineResolutionReading(
    val width: Int,
    val height: Int,
    val elapsedRealtimeMs: Long,
    val source: String
)

/**
 * Extension point for validated readers of particular installed game builds.
 * There are no generic Unity/Unreal memory offsets and no bundled third-party game readers.
 * A reader must validate package version, ABI, engine library build identity, process identity,
 *       and memory/object layout before reading; any unknown combination must return unsupported.
 */
interface EngineResolutionAdapter {
    /**
     * Checks a known package/version; engine name alone cannot establish compatibility.
     */
    fun supports(process: EngineProcess): Boolean

    /**
     * Reads actual pre-upscale dimensions or returns null when they cannot be established.
     * Implementations must revalidate PID ownership/build identity before every memory read.
     */
    fun read(process: EngineProcess, root: RootCommand): EngineResolutionReading?
}
