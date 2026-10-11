using System;
using System.IO;
using UnityEngine;

/**
 * @brief Publishes a selected camera's measured render dimensions to VulkanScope on Android.
 * @note Call from Unity's main thread; reports are atomic and limited to twice per second.
 */
public static class VulkanScopeTelemetry
{
    [Serializable]
    private sealed class ReportData
    {
        public int schemaVersion = 1;
        public string packageName;
        public string engine = "Unity";
        public string source;
        public int pid;
        public long elapsedRealtimeMs;
        public int renderWidth;
        public int renderHeight;
        public int outputWidth;
        public int outputHeight;
    }

    private static long lastReportMs = -1000;

    /**
     * @brief Writes actual pre-upscaling dimensions supplied by a supported render callback.
     * @param width Measured internal viewport width; never substitute Screen.width.
     * @param height Measured internal viewport height; never substitute Screen.height.
     * @param outputWidth Output width, labeled separately by the HUD.
     * @param outputHeight Output height, labeled separately by the HUD.
     * @param source Callback/viewport identifier used to explain the measurement.
     */
    public static void Report(int width, int height, int outputWidth, int outputHeight, string source)
    {
#if UNITY_ANDROID && !UNITY_EDITOR
        if (width <= 0 || height <= 0 || outputWidth <= 0 || outputHeight <= 0) return;
        using (var clock = new AndroidJavaClass("android.os.SystemClock"))
        {
            long now = clock.CallStatic<long>("elapsedRealtime");
            if (now - lastReportMs < 500) return;
            lastReportMs = now;
            try
            {
                using (var player = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
                using (var activity = player.GetStatic<AndroidJavaObject>("currentActivity"))
                using (var directory = activity.Call<AndroidJavaObject>("getFilesDir"))
                using (var process = new AndroidJavaClass("android.os.Process"))
                using (var os = new AndroidJavaClass("android.system.Os"))
                {
                    string path = Path.Combine(directory.Call<string>("getAbsolutePath"), "vulkanscope-render.json");
                    var report = new ReportData
                    {
                        packageName = activity.Call<string>("getPackageName"),
                        source = source,
                        pid = process.CallStatic<int>("myPid"),
                        elapsedRealtimeMs = now,
                        renderWidth = width,
                        renderHeight = height,
                        outputWidth = outputWidth,
                        outputHeight = outputHeight
                    };
                    File.WriteAllText(path + ".tmp", JsonUtility.ToJson(report));
                    os.CallStatic("rename", path + ".tmp", path);
                }
            }
            catch (Exception exception)
            {
                Debug.LogWarning("VulkanScope telemetry unavailable: " + exception.Message);
            }
        }
#endif
    }

    /**
     * @brief Limits the examples to one full-screen, non-XR game camera.
     */
    public static bool IsSupportedCamera(Camera camera)
    {
        return camera != null && camera.cameraType == CameraType.Game &&
            camera.targetTexture == null && !camera.stereoEnabled &&
            camera.rect == new Rect(0f, 0f, 1f, 1f);
    }
}
