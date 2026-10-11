package dev.snowflake.vulkan;

import android.app.Notification;
import android.app.NotificationManager;
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
    private boolean rootMode;
    private RootDisplayBroker rootBroker;
    private long rootStartedAt;
    private long lastNotificationRevision = -1;

    private Notification notification() {
        try {
            org.json.JSONObject text = new org.json.JSONObject(nativeLiveText());
            String language = text.optString("language");
            boolean chinese = "zh-Hans".equals(language) || ("system".equals(language) &&
                getResources().getConfiguration().getLocales().get(0).getLanguage().equals("zh"));
            return HudNotification.build(this, nativeLiveUpdates(), nativeCollapsed(), chinese,
                text.optString("short", "-- FPS"), text.optString("body"));
        } catch (org.json.JSONException error) { throw new IllegalStateException(error); }
    }

    private void updateNotification() {
        if (nativeCollapsed() && (!nativeLiveUpdates() || !HudNotification.available(this))) nativeCollapse(false);
        long revision = nativeRevision();
        if (revision == lastNotificationRevision) return;
        getSystemService(NotificationManager.class).notify(HudNotification.ID, notification());
        lastNotificationRevision = revision;
    }

    private final Runnable update = new Runnable() {
        @Override
        public void run() {
            if (!started)
                return;
            if ((!rootMode && !Settings.canDrawOverlays(HudService.this)) || !nativeRunning()) {
                stopSelf();
                return;
            }
            try { updateNotification(); }
            catch (RuntimeException error) { nativeCollapse(false); }
            if (rootMode) {
                if (!rootBroker.attached() && android.os.SystemClock.uptimeMillis() - rootStartedAt > 30000) {
                    nativeError("Root display startup timed out; check root authorization and firmware SurfaceControl support");
                    stopSelf();
                    return;
                }
                handler.postDelayed(this, 100);
                return;
            }
            try {
                int[] geometry = nativeGeometry(getResources().getDisplayMetrics().widthPixels,
                                                getResources().getDisplayMetrics().heightPixels,
                                                getResources().getDisplayMetrics().density);
                float alpha = nativeCollapsed() ? 0f : HudTouchPolicy.opacity(HudService.this);
                if (layout.alpha != alpha || layout.width != geometry[0] || layout.height != geometry[1] ||
                    layout.gravity != geometry[2]) {
                    layout.alpha = alpha;
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
            handler.postDelayed(this, 100);
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
        if (intent == null || HudNotification.STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (started) {
            try {
                if (HudNotification.EXPAND.equals(intent.getAction())) nativeCollapse(false);
                if (HudNotification.COLLAPSE.equals(intent.getAction()) && nativeLiveUpdates() && HudNotification.available(this)) {
                    // Establish the recovery notification before hiding either display backend.
                    getSystemService(NotificationManager.class).notify(HudNotification.ID, notification());
                    nativeCollapse(true);
                }
                lastNotificationRevision = -1;
                updateNotification();
            } catch (RuntimeException error) { nativeCollapse(false); }
            return START_NOT_STICKY;
        }
        // Old PendingIntents must never resurrect a stopped monitoring session.
        if (intent.getAction() != null) {
            stopSelf();
            return START_NOT_STICKY;
        }
        rootMode = nativeRootMode();
        if (!rootMode && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        try {
            startForeground(HudNotification.ID, notification());
            nativeStart(getApplicationContext(), getFilesDir().getAbsolutePath(), rootMode);
            started = true;
            if (rootMode) {
                rootBroker = new RootDisplayBroker(new RootDisplayBroker.Host() {
                    @Override public boolean running() { return nativeRunning(); }
                    @Override public int[] geometry(int width, int height, float density) {
                        int[] geometry = nativeGeometry(width, height, density);
                        return new int[]{geometry[0], geometry[1], geometry[2], nativeCollapsed() ? 0 : 1};
                    }
                    @Override public void surface(Surface surface, float density) { nativeSurface(surface, density); }
                });
                RootDisplayProvider.install(rootBroker);
                rootStartedAt = android.os.SystemClock.uptimeMillis();
                String uri = "content://" + (android.os.Process.myUid() / 100000) + "@" + getPackageName() + ".rootdisplay";
                nativeRootStart(getApplicationInfo().sourceDir, uri, rootBroker.token);
                handler.post(update);
                return START_NOT_STICKY;
            }
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
                                                        WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                                                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                                                    PixelFormat.TRANSLUCENT);
            layout.gravity = geometry[2];
            layout.alpha = HudTouchPolicy.opacity(HudService.this);
            layout.setTitle("dev.snowflake.vulkan.PerformanceHUD");
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
        nativePrepareStop();
        RootDisplayProvider.install(null);
        rootBroker = null;
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

    /** Reads native display preference and root availability without a shell call. */
    public static boolean usesRootDisplay() { return nativeRootMode(); }
    private static native boolean nativeRootMode();
    /** Reads the native preference without sampling; called on the service main thread. */
    private static native boolean nativeLiveUpdates();
    /** Reads the native publication sequence; notification updates follow the selected sampling cadence. */
    private static native long nativeRevision();
    /** Sets transient native visibility without stopping collection or disposing the Surface. */
    private static native void nativeCollapse(boolean collapsed);
    /** Reads native visibility; also safe from the serialized root Binder callback. */
    private static native boolean nativeCollapsed();
    /** Returns bounded, formatted native snapshot text for the system notification. */
    private static native String nativeLiveText();
    /** Starts a cancellable native-owned root SurfaceControl process off the main thread. */
    private static native void nativeRootStart(String apk, String providerUri, String token);

    /** Starts the native session; permission and foreground-service establishment have already succeeded. */
    private static native void nativeStart(android.content.Context context, String directory, boolean rootDisplay);
    /** Marks an intentional stop before revoking Binder; does not kill the Surface owner yet. */
    private static native void nativePrepareStop();
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
