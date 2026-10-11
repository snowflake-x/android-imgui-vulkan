package dev.snowflake.vulkan;

import java.util.concurrent.atomic.AtomicReference;

/**
 * Exercises bounded command execution on the host without invoking su.
 */
public final class RootCommandTest {
    /**
     * Runs this regression suite on the host without Android root access.
     */
    public static void main(String[] args) throws Exception {
        try (RootCommand command = new RootCommand("/bin/sh")) {
            RootCommand.Result success = command.run("printf 'valid output'", 1000);
            require(success.success && "valid output".equals(success.output), "Successful command output");
            RootCommand.Result failure = command.run("exit 7", 1000);
            require(!failure.success && failure.error.contains("7"), "Failed commands expose their status");
            RootCommand.Result overflow = command.run("i=0; while [ $i -lt 20000 ]; do printf '0123456789'; i=$((i+1)); done", 2000);
            require(!overflow.success && overflow.output.length() <= 128 * 1024,
                    "Oversized output must be bounded and marked unsuccessful");
            long started = System.nanoTime();
            RootCommand.Result timeout = command.run("sleep 10", 50);
            require(!timeout.success && timeout.error.contains("timed out"), "Timeout failure is explicit");
            require(System.nanoTime() - started < 2_000_000_000L, "Timeout must return promptly");
        }
        RootCommand cancellable = new RootCommand("/bin/sh");
        AtomicReference<RootCommand.Result> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(cancellable.run("exec sleep 10", 10000)));
        worker.start();
        Thread.sleep(100);
        long started = System.nanoTime();
        cancellable.close();
        worker.join(2000);
        require(!worker.isAlive() && result.get() != null && !result.get().success, "Close must cancel the active command");
        require(System.nanoTime() - started < 2_000_000_000L, "Cancellation must return promptly");
        require(!cancellable.run("printf ignored", 1000).success, "Closed collectors cannot launch commands");
        Thread.sleep(50);
        for (Thread thread : Thread.getAllStackTraces().keySet()) {
            require(!thread.isAlive() || !thread.getName().equals("hud-root-output"), "No command reader may survive completion");
        }
        System.out.println("RootCommandTest: all checks passed");
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
