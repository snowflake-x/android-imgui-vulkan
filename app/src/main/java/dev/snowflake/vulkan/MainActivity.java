package dev.snowflake.vulkan;

import android.app.NativeActivity;
import android.app.ActivityManager;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.WindowManager;
import android.widget.Toast;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends NativeActivity {
    private volatile String rootReport = "Not requested. Tap the button to request su authorization.";
    private final AtomicBoolean rootBusy = new AtomicBoolean(false);
    private volatile Process rootProcess;
    private volatile boolean destroyed;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    public String getSystemReport() {
        Display display = getWindowManager().getDefaultDisplay();
        DisplayMetrics metrics = new DisplayMetrics();
        display.getRealMetrics(metrics);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ((ActivityManager)getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(memory);
        return "Device: " + Build.MANUFACTURER + " " + Build.MODEL
            + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
            + "\nABI: " + String.join(", ", Build.SUPPORTED_ABIS)
            + "\nDisplay: " + metrics.widthPixels + " x " + metrics.heightPixels + " px"
            + "\nDensity: " + metrics.densityDpi + " dpi / scale " + metrics.density
            + "\nPhysical DPI (reported): " + metrics.xdpi + " x " + metrics.ydpi
            + "\nRefresh: " + display.getRefreshRate() + " Hz"
            + "\nRotation: " + (display.getRotation() * 90) + " degrees"
            + "\nCPU cores: " + Runtime.getRuntime().availableProcessors()
            + "\nRAM available / total: " + memory.availMem / 1048576 + " / " + memory.totalMem / 1048576 + " MiB";
    }
    public String getRootReport() { return rootReport; }
    public void showError(String message) {
        runOnUiThread(() -> { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); finish(); });
    }
    public void requestRootInfo() {
        if (destroyed || !rootBusy.compareAndSet(false, true)) return;
        rootReport = "Waiting for su authorization (30 second timeout)...";
        Thread worker = new Thread(() -> {
            Process process = null;
            try {
                // Fixed read-only commands. No exploitation, arbitrary input, or system changes.
                String script = "id; echo '\n--- Kernel ---'; uname -a; "
                    + "echo '\n--- CPU ---'; cat /proc/cpuinfo; "
                    + "echo '\n--- Memory ---'; head -30 /proc/meminfo; "
                    + "echo '\n--- SELinux ---'; getenforce; "
                    + "echo '\n--- Thermal zones (raw units) ---'; "
                    + "for z in /sys/class/thermal/thermal_zone*; do "
                    + "[ -r \"$z/type\" ] && cat \"$z/type\"; [ -r \"$z/temp\" ] && cat \"$z/temp\"; done";
                process = new ProcessBuilder("su", "-c", script).redirectErrorStream(true).start();
                rootProcess = process;
                if (destroyed) { process.destroyForcibly(); return; }
                final Process running = process;
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                Thread reader = new Thread(() -> {
                    try (InputStream in = running.getInputStream()) {
                        byte[] bytes = new byte[2048];
                        int count;
                        while ((count = in.read(bytes)) != -1) {
                            synchronized (output) {
                                int remaining = 32768 - output.size();
                                if (remaining > 0) output.write(bytes, 0, Math.min(count, remaining));
                            }
                        }
                    } catch (Exception ignored) { }
                }, "su-output");
                reader.setDaemon(true);
                reader.start();
                boolean finished = process.waitFor(30, TimeUnit.SECONDS);
                if (!finished) process.destroyForcibly();
                reader.join(1000);
                String result;
                synchronized (output) { result = new String(output.toByteArray(), StandardCharsets.UTF_8); }
                rootReport = (finished ? "su exit code: " + process.exitValue() : "Authorization / command timed out")
                    + "\n" + result + "\n(Output limited to 32 KiB)";
            } catch (Exception e) {
                rootReport = "su unavailable or denied: " + e.getMessage();
            } finally {
                if (process != null) process.destroy();
                rootProcess = null;
                rootBusy.set(false);
            }
        }, "root-info");
        worker.setDaemon(true);
        worker.start();
    }
    @Override public void onDestroy() {
        destroyed = true;
        Process process = rootProcess;
        if (process != null) process.destroyForcibly();
        super.onDestroy();
    }
}
