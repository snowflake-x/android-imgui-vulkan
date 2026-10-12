package dev.snowflake.vulkan;

/** Isolated root process bridge; all process inspection and parsing are implemented in C++. */
public final class MemoryProbeBootstrap {
    private MemoryProbeBootstrap() {}

    /** Invoked by app_process with shell-quoted arguments; never runs in the UI process. */
    public static void main(String[] args) {
        if (android.os.Process.myUid() != 0 || (args.length != 6 && args.length != 8)) System.exit(2);
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(25000); } catch (InterruptedException ignored) {}
            Runtime.getRuntime().halt(124);
        }, "MemoryProbeDeadline");
        watchdog.setDaemon(true);
        watchdog.start();
        try {
            byte[] report;
            if (args[1].equals("runtime") && args.length == 8) {
                System.load(args[6]);
                byte[] request = new byte[args[5].length() / 2];
                if (args[5].length() % 2 != 0 || request.length > 8192) throw new IllegalArgumentException("Invalid runtime request");
                for (int i = 0; i < request.length; i++) request[i] = (byte) Integer.parseInt(args[5].substring(i * 2, i * 2 + 2), 16);
                report = nativeInspector(Integer.parseInt(args[2]), Long.parseLong(args[3]), request, readAgent(args[7]));
            } else {
                System.load(args[0]);
                report = nativeProbe(args[1], Integer.parseInt(args[2]), Long.parseLong(args[3]), args[4], args[5]);
            }
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

    /** Reads the packaged agent in this disposable root process; Frida receives bytes without a persistent extracted file. */
    private static byte[] readAgent(String path) throws java.io.IOException {
        int separator = path.indexOf("!/");
        if (separator >= 0) {
            try (java.util.zip.ZipFile apk = new java.util.zip.ZipFile(path.substring(0, separator))) {
                java.util.zip.ZipEntry entry = apk.getEntry(path.substring(separator + 2));
                if (entry == null || entry.getSize() <= 0 || entry.getSize() > 64 * 1024 * 1024) throw new java.io.IOException("Invalid agent entry");
                try (java.io.InputStream stream = apk.getInputStream(entry)) { return readBounded(stream); }
            }
        }
        try (java.io.InputStream stream = new java.io.FileInputStream(path)) { return readBounded(stream); }
    }

    /** Bounds native-library reads on Android versions predating InputStream.readNBytes. */
    private static byte[] readBounded(java.io.InputStream stream) throws java.io.IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[65536];
        int count;
        while ((count = stream.read(buffer)) != -1) {
            if (out.size() + count > 64 * 1024 * 1024) throw new java.io.IOException("Agent exceeds 64 MiB");
            out.write(buffer, 0, count);
        }
        return out.toByteArray();
    }

    /** Performs one root attach or runtime request; cancellation is bounded by the helper watchdog. */
    private static native byte[] nativeInspector(int pid, long start, byte[] request, byte[] agent);
}
