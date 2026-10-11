package dev.snowflake.vulkan;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.TreeSet;

/**
 * Counts SurfaceFlinger actual-present timestamps without counting repeated ring entries.
 * The refresh-period header is only a format check and never contributes to reported FPS.
 * Instances keep a mutable baseline and must be used on one sampling thread; this class
 * performs no asynchronous work or Android lifecycle operations.
 */
public final class FrameStats {
    private String surface;
    private long previousSample;
    private long previousPresent;
    private boolean initialized;

    /**
     * Holds parsed presentation timestamps and whether the SurfaceFlinger response is usable.
     * Timestamps are sorted, unique, and exposed through an unmodifiable list.
     */
    public static final class Trace {
        public final boolean supported;
        public final List<Long> presents;
        public final String error;

        private Trace(boolean supported, List<Long> presents, String error) {
            this.supported = supported;
            this.presents = Collections.unmodifiableList(presents);
            this.error = error;
        }
    }

    /**
     * Holds one observation interval's FPS and presentation timing statistics.
     * Unavailable numbers are NaN; truncated windows describe only the retained ring history.
     */
    public static final class Sample {
        public final boolean supported;
        public final boolean warmingUp;
        public final boolean truncatedWindow;
        public final double fps;
        public final double frameTimeMs;
        public final double frameP95Ms;
        public final String error;

        private Sample(boolean supported, boolean warmingUp, boolean truncatedWindow,
                       double fps, double frameTimeMs, double frameP95Ms, String error) {
            this.supported = supported;
            this.warmingUp = warmingUp;
            this.truncatedWindow = truncatedWindow;
            this.fps = fps;
            this.frameTimeMs = frameTimeMs;
            this.frameP95Ms = frameP95Ms;
            this.error = error;
        }
    }

    /**
     * Reads column two, actualPresentTime, from a SurfaceFlinger --latency response.
     *
     * @param output Complete command output, including its refresh-period header.
     * @param nowNanos CLOCK_MONOTONIC time after the command completed.
     * @return Sorted unique presentation timestamps, excluding zero, pending, malformed and future values.
     */
    public static Trace parseLatency(String output, long nowNanos) {
        if (output == null || output.trim().isEmpty()) return unsupported("No SurfaceFlinger latency data");
        String[] lines = output.trim().split("\\r?\\n");
        try {
            long period = Long.parseLong(lines[0].trim());
            if (period <= 0 || period > 1_000_000_000L) return unsupported("Surface latency unavailable");
        } catch (NumberFormatException exception) {
            return unsupported("SurfaceFlinger latency unsupported or denied");
        }
        TreeSet<Long> presents = new TreeSet<>();
        for (int i = 1; i < lines.length; i++) {
            String[] columns = lines[i].trim().split("\\s+");
            if (columns.length != 3) continue;
            try {
                Long.parseLong(columns[0]);
                long actualPresent = Long.parseLong(columns[1]);
                Long.parseLong(columns[2]);
                if (actualPresent > 0 && actualPresent != Long.MAX_VALUE && actualPresent <= nowNanos) {
                    presents.add(actualPresent);
                }
            } catch (NumberFormatException ignored) { }
        }
        if (presents.isEmpty()) return unsupported("No valid presented frames for this surface");
        return new Trace(true, new ArrayList<>(presents), "");
    }

    private static Trace unsupported(String error) {
        return new Trace(false, new ArrayList<>(), error);
    }

    /**
     * Discards the baseline when the foreground app, surface, or source changes.
     * Call on the same sampling thread used for {@link #update(String, Trace, long)}.
     */
    public void reset() {
        surface = null;
        previousSample = 0;
        previousPresent = 0;
        initialized = false;
    }

    /**
     * Measures newly presented frames over the elapsed observation interval.
     * A full ring overwrite reports the observed timestamp cadence and flags its shorter window.
     * Call on the instance's sampling thread; the first usable sample establishes a baseline.
     *
     * @param surfaceId Stable identity of the selected layer, including its foreground package.
     * @param trace Parsed, current latency data.
     * @param nowNanos CLOCK_MONOTONIC time corresponding to the end of the trace query.
     * @return FPS and presentation-interval statistics; unavailable timing values are NaN.
     */
    public Sample update(String surfaceId, Trace trace, long nowNanos) {
        if (surfaceId == null || trace == null || !trace.supported) {
            reset();
            return new Sample(false, false, false, Double.NaN, Double.NaN, Double.NaN,
                    trace == null ? "No latency sample" : trace.error);
        }
        List<Long> times = trace.presents;
        long latest = times.get(times.size() - 1);
        if (!initialized || !surfaceId.equals(surface) || nowNanos <= previousSample || nowNanos - previousSample > 10_000_000_000L || latest < previousPresent) {
            surface = surfaceId;
            previousSample = nowNanos;
            previousPresent = latest;
            initialized = true;
            return new Sample(true, true, false, Double.NaN, Double.NaN, Double.NaN, "Collecting presentation timestamps");
        }
        long elapsed = nowNanos - previousSample;
        long baselinePresent = previousPresent;
        previousSample = nowNanos;
        previousPresent = Math.max(previousPresent, latest);
        ArrayList<Long> newTimes = new ArrayList<>();
        for (long time : times) if (time > baselinePresent) newTimes.add(time);
        if (newTimes.isEmpty()) {
            return new Sample(true, false, false, 0.0, Double.NaN, Double.NaN, "");
        }
        boolean overwritten = times.get(0) > baselinePresent;
        ArrayList<Double> intervals = new ArrayList<>();
        long last = overwritten ? newTimes.get(0) : baselinePresent;
        for (long time : newTimes) {
            if (time > last) intervals.add((time - last) / 1_000_000.0);
            last = time;
        }
        double fps;
        if (overwritten) {
            if (newTimes.size() < 2) {
                return new Sample(true, false, true, Double.NaN, Double.NaN, Double.NaN,
                        "Presentation ring changed; collecting a fresh baseline");
            }
            fps = (newTimes.size() - 1) * 1_000_000_000.0 / (latest - newTimes.get(0));
        } else {
            fps = newTimes.size() * 1_000_000_000.0 / elapsed;
        }
        double mean = Double.NaN;
        double p95 = Double.NaN;
        if (!intervals.isEmpty()) {
            double total = 0;
            for (double interval : intervals) total += interval;
            mean = total / intervals.size();
            Collections.sort(intervals);
            p95 = intervals.get(Math.max(0, (int) Math.ceil(intervals.size() * 0.95) - 1));
        }
        return new Sample(true, false, overwritten, fps, mean, p95, "");
    }
}
