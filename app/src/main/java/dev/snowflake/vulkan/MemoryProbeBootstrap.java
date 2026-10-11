package dev.snowflake.vulkan;

/** Isolated root process bridge; all process inspection and parsing are implemented in C++. */
public final class MemoryProbeBootstrap {
    private MemoryProbeBootstrap() {}

    /** Invoked by app_process with shell-quoted arguments; never runs in the UI process. */
    public static void main(String[] args) {
        if (android.os.Process.myUid() != 0 || args.length != 6) System.exit(2);
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(25000); } catch (InterruptedException ignored) {}
            Runtime.getRuntime().halt(124);
        }, "MemoryProbeDeadline");
        watchdog.setDaemon(true);
        watchdog.start();
        try {
            System.load(args[0]);
            byte[] report = nativeProbe(args[1], Integer.parseInt(args[2]), Long.parseLong(args[3]), args[4], args[5]);
            System.out.write(report);
            System.out.flush();
            System.exit(0);
        } catch (Throwable error) {
            System.err.println("Memory probe failed: " + error);
            System.exit(1);
        }
    }

    /** Executes one bounded read-only probe on this disposable process's main thread. */
    private static native byte[] nativeProbe(String operation, int pid, long start, String kind, String input);
}
