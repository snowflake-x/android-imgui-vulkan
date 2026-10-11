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

/**
 * 提供 NativeActivity 宿主、设备信息和受限的异步 root 诊断。
 * <p>公开的报告与操作方法由 C++ 通过 JNI 按名称及签名调用，变更时必须同步更新调用方。
 */
public class MainActivity extends NativeActivity {
    private volatile String rootReport = "Not requested. Tap the button to request su authorization.";
    private final AtomicBoolean rootBusy = new AtomicBoolean(false);
    private volatile Process rootProcess;
    private volatile boolean destroyed;

    /**
     * 创建宿主并在前台保持屏幕点亮。
     * @param state 系统保存的 Activity 状态，首次启动时可为 null
     */
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /**
     * 获取系统报告；由原生渲染线程定期读取，不保存可变 Android 对象。
     * @return 设备、屏幕和内存信息；DPI 等数值以系统报告为准
     */
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
    /**
     * 获取后台诊断线程最近发布的文本快照，不等待命令完成。
     * @return 初始提示、等待状态、诊断输出或错误信息
     */
    public String getRootReport() { return rootReport; }
    /**
     * 将原生层错误投递到 UI 线程显示，然后关闭 Activity。
     * @param message 面向用户的错误说明
     */
    public void showError(String message) {
        runOnUiThread(() -> { Toast.makeText(this, message, Toast.LENGTH_LONG).show(); finish(); });
    }
    /**
     * 异步请求已有 su 提供者授权并执行固定只读诊断，允许从渲染线程调用。
     * <p>已销毁或正在执行时忽略请求；命令等待最多 30 秒，保留最多 32 KiB 输出。
     * 读取线程在达到输出上限后继续排空管道，避免子进程因管道写满而阻塞。
     */
    public void requestRootInfo() {
        if (destroyed || !rootBusy.compareAndSet(false, true)) return;
        rootReport = "Waiting for su authorization (30 second timeout)...";
        Thread worker = new Thread(() -> {
            Process process = null;
            try {
                /** 固定只读命令，不接受任意输入或修改系统。 */
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
    /**
     * 阻止新诊断任务并终止当前 su 进程，再释放 Activity。
     * <p>后台任务通过 destroyed 标记处理与进程创建并发的销毁操作。
     */
    @Override public void onDestroy() {
        destroyed = true;
        Process process = rootProcess;
        if (process != null) process.destroyForcibly();
        super.onDestroy();
    }
}
