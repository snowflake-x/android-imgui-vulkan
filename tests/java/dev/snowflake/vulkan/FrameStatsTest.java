package dev.snowflake.vulkan;

/**
 * Dependency-free regression tests for external application presentation accounting.
 */
public final class FrameStatsTest {
    /**
     * Runs this regression suite on the host without Android root access.
     */
    public static void main(String[] args) {
        readsActualPresentColumn();
        rejectsUnavailableData();
        countsOverlappingRingsOnce();
        resetsAcrossSurfacesAndFailures();
        handlesRingOverwrites();
        handlesRecreatedLayer();
        handlesPausedSampler();
        shellArgumentsStayLiteral();
        System.out.println("FrameStatsTest: all checks passed");
    }

    private static void readsActualPresentColumn() {
        String output = "16666666\n100 200 300\n100 200 300\n300 400 500\n"
                + "0 0 0\n0 9223372036854775807 0\n1 -5 1\n0 NaN 0\n1 2000 1\n";
        FrameStats.Trace trace = FrameStats.parseLatency(output, 1000);
        require(trace.supported, "Valid actual-present timestamps should be supported");
        require(trace.presents.size() == 2, "Invalid and duplicate timestamps must be excluded");
        require(trace.presents.get(0) == 200 && trace.presents.get(1) == 400,
                "Only actual-present column contributes timestamps");
    }

    private static void rejectsUnavailableData() {
        for (String output : new String[]{"", "Permission Denial", "0\n0 0 0", "16666666\n0 0 0", "NaN\n1 2 3"}) {
            require(!FrameStats.parseLatency(output, 1000).supported, "Unsupported data must remain unavailable");
        }
    }

    private static void countsOverlappingRingsOnce() {
        FrameStats stats = new FrameStats();
        require(stats.update("game", trace(1_000_000_000L, 900_000_000L, 950_000_000L), 1_000_000_000L).warmingUp,
                "Initial ring establishes a baseline");
        FrameStats.Sample active = stats.update("game", trace(1_100_000_000L, 900_000_000L, 950_000_000L,
                1_000_000_000L, 1_050_000_000L), 1_100_000_000L);
        near(active.fps, 20, "Two genuinely new frames in 100 ms should report 20 FPS");
        near(active.frameTimeMs, 50, "Mean frame time uses presentation intervals");
        near(active.frameP95Ms, 50, "P95 uses presentation intervals");
        FrameStats.Sample idle = stats.update("game", trace(1_200_000_000L, 900_000_000L, 950_000_000L,
                1_000_000_000L, 1_050_000_000L), 1_200_000_000L);
        near(idle.fps, 0, "An unchanged latency ring must report idle, not historical FPS");
        require(Double.isNaN(idle.frameTimeMs), "Idle has no newly observed frame time");
    }

    private static void resetsAcrossSurfacesAndFailures() {
        FrameStats stats = new FrameStats();
        stats.update("first", trace(1000, 800, 900), 1000);
        require(stats.update("second", trace(1100, 1000), 1100).warmingUp, "Changing surfaces resets the baseline");
        FrameStats.Sample failure = stats.update("second", FrameStats.parseLatency("error", 1200), 1200);
        require(!failure.supported && Double.isNaN(failure.fps), "Query failures must not look like zero FPS");
        require(stats.update("second", trace(1300, 1200), 1300).warmingUp, "Recovery establishes a fresh baseline");
        require(stats.update("second", trace(1300, 1200), 1300).warmingUp, "Nonadvancing clocks cannot divide by zero");
    }

    private static void handlesRingOverwrites() {
        FrameStats stats = new FrameStats();
        stats.update("game", trace(1_000_000_000L, 900_000_000L), 1_000_000_000L);
        FrameStats.Sample sample = stats.update("game", trace(2_000_000_000L,
                1_800_000_000L, 1_850_000_000L, 1_900_000_000L), 2_000_000_000L);
        require(sample.truncatedWindow, "Lost ring history must be flagged");
        near(sample.fps, 20, "Wrapped ring reports observed presentation cadence, not a false 3 FPS");
        near(sample.frameTimeMs, 50, "Missing history must not create a fabricated 900 ms frame");
    }

    private static void handlesRecreatedLayer() {
        FrameStats stats = new FrameStats();
        stats.update("game", trace(1000, 900), 1000);
        require(stats.update("game", trace(1100, 800), 1100).warmingUp,
                "A recreated layer with regressed timestamps needs a fresh baseline");
        require(stats.update("game", trace(1200, 800, 950), 1200).fps > 0,
                "A recreated layer must recover rather than remain at zero");
    }

    private static void handlesPausedSampler() {
        FrameStats stats = new FrameStats();
        stats.update("game", trace(1_000_000_000L, 900_000_000L), 1_000_000_000L);
        FrameStats.Sample resumed = stats.update("game", trace(20_000_000_000L, 19_900_000_000L), 20_000_000_000L);
        require(resumed.warmingUp && Double.isNaN(resumed.fps),
                "Long sampling pauses must establish a fresh baseline");
    }

    private static void shellArgumentsStayLiteral() {
        require("'simple'".equals(RootCommand.quote("simple")), "Simple shell quoting");
        require("'a'\\''b'".equals(RootCommand.quote("a'b")), "Quotes must remain literal shell arguments");
        require("'$(id); `id`\n'".equals(RootCommand.quote("$(id); `id`\n")), "Shell substitutions must remain quoted");
        try {
            RootCommand.quote("bad\0argument");
            throw new AssertionError("NUL must be rejected");
        } catch (IllegalArgumentException expected) { }
    }

    private static FrameStats.Trace trace(long now, long... timestamps) {
        StringBuilder output = new StringBuilder("16666666\n");
        for (long timestamp : timestamps) output.append("0 ").append(timestamp).append(" 0\n");
        return FrameStats.parseLatency(output.toString(), now);
    }

    private static void near(double actual, double expected, String message) {
        require(Double.isFinite(actual) && Math.abs(actual - expected) < 0.000001, message + ": " + actual);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
