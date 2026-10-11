package dev.snowflake.vulkan;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

/**
 * Android foreground-service and Surface lifecycle bridge. C++ owns configuration, collection,
 * layout decisions and ImGui rendering; this class has no metric or preference implementation.
 */
public final class HudService extends Service implements SurfaceHolder.Callback {
    static { System.loadLibrary("vulkanscope"); }
    private final Handler handler = new Handler(Looper.getMainLooper());
    private WindowManager windows;
    private SurfaceView surface;
    private WindowManager.LayoutParams layout;
    private boolean started;
    private static final String STOP = "dev.snowflake.vulkan.hud.STOP";

    private final Runnable update = new Runnable() {
        @Override
        public void run() {
            if (!started)
                return;
            if (!Settings.canDrawOverlays(HudService.this) || !nativeRunning()) {
                stopSelf();
                return;
            }
            try {
                int[] geometry = nativeGeometry(getResources().getDisplayMetrics().widthPixels,
                                                getResources().getDisplayMetrics().heightPixels,
                                                getResources().getDisplayMetrics().density);
                if (layout.width != geometry[0] || layout.height != geometry[1] ||
                    layout.gravity != geometry[2]) {
                    layout.width = geometry[0];
                    layout.height = geometry[1];
                    layout.gravity = geometry[2];
                    windows.updateViewLayout(surface, layout);
                }
            } catch (RuntimeException error) {
                nativeError(error.toString());
                stopSelf();
                return;
            }
            handler.postDelayed(this, 500);
        }
    };

    /** Unbound service: monitoring is explicitly started and stopped by user actions. */
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /** Starts the Android foreground lifetime and supplies a translucent Surface to C++. */
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (started)
            return START_NOT_STICKY;
        if (!Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        try {
            NotificationManager notifications = getSystemService(NotificationManager.class);
            notifications.createNotificationChannel(new NotificationChannel(
                "performance_hud", "Performance HUD", NotificationManager.IMPORTANCE_LOW));
            PendingIntent open =
                PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                                          PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            PendingIntent stop =
                PendingIntent.getService(this, 1, new Intent(this, HudService.class).setAction(STOP),
                                         PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            Notification notification =
                new Notification.Builder(this, "performance_hud")
                    .setSmallIcon(getApplicationInfo().icon)
                    .setContentTitle("VulkanScope performance HUD")
                    .setContentText("Tap to configure; Stop ends native monitoring")
                    .setContentIntent(open)
                    .setOngoing(true)
                    .addAction(new Notification.Action.Builder(null, "Stop", stop).build())
                    .build();
            startForeground(3001, notification);
            windows = getSystemService(WindowManager.class);
            surface = new SurfaceView(this);
            surface.setZOrderOnTop(true);
            surface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
            surface.getHolder().addCallback(this);
            int[] geometry = nativeGeometry(getResources().getDisplayMetrics().widthPixels,
                                            getResources().getDisplayMetrics().heightPixels,
                                            getResources().getDisplayMetrics().density);
            layout = new WindowManager.LayoutParams(geometry[0], geometry[1],
                                                    WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                                                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                                                        WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
                                                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                                                    PixelFormat.TRANSLUCENT);
            layout.gravity = geometry[2];
            layout.alpha = 0.8f;
            layout.setTitle("dev.snowflake.vulkan.PerformanceHUD");
            nativeStart(getApplicationContext(), getFilesDir().getAbsolutePath());
            started = true;
            windows.addView(surface, layout);
            handler.post(update);
        } catch (RuntimeException error) {
            nativeError(error.toString());
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    /** Surface changes are handled after dimensions become known. */
    @Override
    public void surfaceCreated(SurfaceHolder holder) {}
    /** Replaces the native renderer after cancelling and joining any previous Surface worker. */
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurface(holder.getSurface(), getResources().getDisplayMetrics().density);
    }
    /** Joins native rendering before Android invalidates the Surface. */
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurface(null, 1);
    }

    /** Cancels native subprocesses and joins workers before removing the window and notification. */
    @Override
    public void onDestroy() {
        started = false;
        handler.removeCallbacksAndMessages(null);
        nativeStop();
        if (surface != null) {
            surface.getHolder().removeCallback(this);
            try {
                windows.removeViewImmediate(surface);
            } catch (IllegalArgumentException ignored) {
            }
            surface = null;
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    /** Starts the native session; permission and foreground-service establishment have already succeeded. */
    private static native void nativeStart(android.content.Context context, String directory);
    /** Cancels collection and rendering and waits for resource ownership to return. */
    private static native void nativeStop();
    /** Transfers a Surface reference to C++, or cancels rendering for a null Surface. */
    private static native void nativeSurface(Surface surface, float density);
    /** Returns C++ layout width, height and Android gravity for the current configuration. */
    private static native int[] nativeGeometry(int width, int height, float density);
    /** Reads native session state without waiting for collection. */
    private static native boolean nativeRunning();
    /** Publishes bridge failures to the native UI. */
    private static native void nativeError(String error);
}
