package dev.snowflake.vulkan;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Runs bounded, read-only root commands after the caller enables root collection.
 * Each collector owns one instance and calls {@link #run(String, long)} on its worker thread.
 * Only one command may run per instance; {@link #close()} may run concurrently on the main
 * thread to cancel the active process without waiting for its timeout. No output-reader
 * thread is created. The owning service closes collectors when its monitoring session ends;
 * Activity destruction alone does not end that service-owned session.
 */
public final class RootCommand implements AutoCloseable {
    private static final int OUTPUT_LIMIT = 128 * 1024;
    private final Object processLock = new Object();
    private volatile boolean closed;
    private Process active;
    private final String executable;

    /** Creates a collector that invokes the device's {@code su} executable. */
    public RootCommand() { this("su"); }

    /**
     * Allows the host regression suite to exercise cancellation without requesting root.
     *
     * @param executable Command interpreter used instead of {@code su}.
     */
    RootCommand(String executable) { this.executable = executable; }

    /** Holds bounded command output, a success flag, and a failure reason when unsuccessful. */
    public static final class Result {
        public final String output;
        public final boolean success;
        public final String error;

        /**
         * Creates an immutable command result.
         *
         * @param output Captured standard output and standard error.
         * @param success Whether the command completed successfully without exceeding limits.
         * @param error Failure reason, or an empty string on success.
         */
        public Result(String output, boolean success, String error) {
            this.output = output;
            this.success = success;
            this.error = error;
        }
    }

    /**
     * Runs a caller-supplied read-only shell command synchronously on a worker thread.
     * Captured output is limited to 128 KiB. The command wait budget begins after process
     * creation and is clamped to 1–30,000 ms; timeout, interruption, or {@link #close()}
     * causes process cleanup. Concurrent runs and runs after close return failure results.
     *
     * @param command Shell source whose variable arguments must use {@link #quote(String)}.
     * @param timeoutMs Command wait budget in milliseconds, including initial root authorization.
     * @return Output and an explicit failure reason; truncated output is never marked successful.
     */
    public Result run(String command, long timeoutMs) {
        if (command == null || command.isEmpty()) return new Result("", false, "Empty command");
        Process process = null;
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        String error = "";
        boolean successful = false;
        try {
            synchronized (processLock) {
                if (closed) return new Result("", false, "Collector closed");
                if (active != null) return new Result("", false, "Collector already running");
                process = new ProcessBuilder(executable, "-c", command).redirectErrorStream(true).start();
                active = process;
            }
            long budgetNanos = TimeUnit.MILLISECONDS.toNanos(Math.max(1, Math.min(timeoutMs, 30000)));
            long started = System.nanoTime();
            InputStream input = process.getInputStream();
            byte[] bytes = new byte[4096];
            boolean finished = false;
            while (true) {
                /**
                 * Reading only available bytes prevents inherited child pipes from trapping a reader after cancellation.
                 */
                int available;
                while ((available = input.available()) > 0) {
                    int count = input.read(bytes, 0, Math.min(bytes.length, available));
                    if (count < 0) break;
                    int remaining = OUTPUT_LIMIT - output.size();
                    if (remaining > 0) output.write(bytes, 0, Math.min(count, remaining));
                    if (count > remaining) {
                        error = "Command output exceeded 128 KiB";
                        break;
                    }
                }
                if (!error.isEmpty()) break;
                if (closed) {
                    error = "Collector closed";
                    break;
                }
                if (finished) {
                    if (process.exitValue() == 0) successful = true;
                    else error = "su failed (exit " + process.exitValue() + ")";
                    break;
                }
                long remainingNanos = budgetNanos - (System.nanoTime() - started);
                if (remainingNanos <= 0) {
                    error = "Root authorization or command timed out";
                    break;
                }
                finished = process.waitFor(Math.min(remainingNanos, 10_000_000L), TimeUnit.NANOSECONDS);
            }
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            error = "Command interrupted";
        } catch (Exception exception) {
            error = closed ? "Collector closed" : "su unavailable: " + exception.getClass().getSimpleName();
        } finally {
            if (process != null) {
                process.destroyForcibly();
                try { process.getInputStream().close(); } catch (Exception ignored) { }
                try { process.getOutputStream().close(); } catch (Exception ignored) { }
                try { process.getErrorStream().close(); } catch (Exception ignored) { }
                synchronized (processLock) {
                    if (active == process) active = null;
                }
            }
        }
        return new Result(new String(output.toByteArray(), StandardCharsets.UTF_8), successful, error);
    }

    /**
     * Quotes one literal shell argument, including quotes and embedded newlines.
     *
     * @param value Literal argument to pass to the shell.
     * @return A single-quoted shell argument with embedded quotes escaped.
     * @throws IllegalArgumentException If the value is null or contains a NUL character.
     */
    public static String quote(String value) {
        if (value == null) throw new IllegalArgumentException("Shell argument cannot be null");
        if (value.indexOf('\0') >= 0) throw new IllegalArgumentException("Shell argument contains NUL");
        return "'" + value.replace("'", "'\\''") + "'";
    }

    /**
     * Permanently rejects new commands and requests termination of the active process.
     * Safe to call repeatedly or concurrently with the sampling worker, including from the
     * main thread during service destruction; it does not wait for the worker to finish.
     */
    @Override public void close() {
        synchronized (processLock) {
            closed = true;
            if (active != null) active.destroyForcibly();
        }
    }
}
