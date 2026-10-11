package dev.snowflake.vulkan

/**
 * Exercises exact foreground identity and SurfaceFlinger package matching without Android.
 */
object ForegroundParsingTest {
    /**
     * Runs the parser regression checks and fails on invalid results.
     */
    @JvmStatic fun main(args: Array<String>) {
        val top = ForegroundFpsCollector.parseForeground("""
            mLastResumedActivity: ActivityRecord{123 u0 com.stale/.Activity t1}
            mResumedActivity: ActivityRecord{456 u0 com.secondary/.Activity t2}
            topResumedActivity=ActivityRecord{789 u10 com.game/.GameActivity t3}
        """.trimIndent(), true)
        check(top?.packageName == "com.game" && top.userId == 10) { "Top resumed package and Android user must be exact" }
        val overlay = ForegroundFpsCollector.parseForeground("""
            mCurrentFocus=Window{123 u0 dev.snowflake.vulkan/HudOverlay}
            mFocusedApp=ActivityRecord{456 u10 com.game/.Game t3}
        """.trimIndent(), false)
        check(overlay?.packageName == "com.game") { "Own overlay must not hide the focused game" }
        check(ForegroundFpsCollector.parseForeground(
            "mLastResumedActivity: ActivityRecord{123 u0 com.background/.Activity t1}", true) == null)
        check(ForegroundFpsCollector.parseForeground(
            "topResumedActivity=ActivityRecord{123 u0 dev.snowflake.vulkan/.MainActivity t1}", true)?.packageName == "dev.snowflake.vulkan") {
            "The actual foreground HUD settings activity must not select a background game"
        }
        check(ForegroundFpsCollector.containsPackage("SurfaceView[com.game/com.unity3d.player.UnityPlayerActivity](BLAST)#5", "com.game"))
        check(ForegroundFpsCollector.containsPackage("com.game/.Main#3", "com.game"))
        check(!ForegroundFpsCollector.containsPackage("SurfaceView[com.game.extra/.Main]", "com.game"))
        check(!ForegroundFpsCollector.containsPackage("SurfaceView[com.game2/.Main]", "com.game"))
        check(!ForegroundFpsCollector.containsPackage("SurfaceView[org.com.game/.Main]", "com.game"))
        val layers = "SurfaceView[com.game/.Main](BLAST)#5\ncom.game/.Main#6\nSurfaceView[com.other/.Main]#7\ndev.snowflake.vulkan.PerformanceHUD"
        check(ForegroundFpsCollector.candidateSurfaces(layers, "com.game", "").first().startsWith("SurfaceView"))
        check(ForegroundFpsCollector.candidateSurfaces(layers, "com.other", "SurfaceView[com.game/.Main](BLAST)#5").isEmpty()) {
            "An override from the previous game must stop matching after foreground switches"
        }
        check(ForegroundFpsCollector.candidateSurfaces(layers, "com.game", "dev.snowflake.vulkan.PerformanceHUD").isEmpty())
        check(ForegroundFpsCollector.surfaceSelectionPriority("SurfaceView[com.game/.Main](BLAST)", true, 20_000_000) >
            ForegroundFpsCollector.surfaceSelectionPriority("com.game/.Main#6", true, 1_000_000)) {
            "A later serial probe of an animating window must not replace active game content"
        }
        check(ForegroundFpsCollector.surfaceSelectionPriority("SurfaceView[com.game/.Main](BLAST)", true, 5_000_000_000) <
            ForegroundFpsCollector.surfaceSelectionPriority("com.game/.Main#6", true, 1_000_000)) {
            "Stale content must allow a currently presenting app surface to win"
        }
        println("ForegroundParsingTest: all checks passed")
    }
}
