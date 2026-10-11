package dev.snowflake.vulkan;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.view.WindowManager;

/** Android lifecycle and permission bridge; all application UI and monitoring live in C++. */
public final class MainActivity extends NativeActivity {
    static { System.loadLibrary("vulkanscope"); }

    /** Initializes native application state on the UI thread before the rendering thread starts. */
    @Override
    public void onCreate(Bundle state) {
        nativeInitialize(getApplicationContext(), getFilesDir().getAbsolutePath());
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /**
     * Posts a native user action to the UI thread; ignores callbacks after Activity destruction.
     * @param action 0 grants overlay permission, 1 starts the service, 2 stops it, 3 closes this Activity, 4
     *     requests notification permission.
     */
    public void hudAction(int action) {
        runOnUiThread(() -> {
            if (isDestroyed())
                return;
            if (action == 0) {
                startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
            } else if (action == 1) {
                if (!Settings.canDrawOverlays(this)) {
                    hudAction(0);
                    return;
                }
                try {
                    startForegroundService(new Intent(this, HudService.class));
                } catch (RuntimeException error) {
                    nativeError(error.toString());
                }
            } else if (action == 2) {
                stopService(new Intent(this, HudService.class));
            } else if (action == 3) {
                finish();
            } else if (action == 4 && android.os.Build.VERSION.SDK_INT >= 33) {
                requestPermissions(new String[] {android.Manifest.permission.POST_NOTIFICATIONS}, 1);
            }
        });
    }

    /** Initializes C++ configuration and application-context ownership; performs no root request. */
    private static native void nativeInitialize(android.content.Context context, String directory);
    /** Publishes a lifecycle failure to the native UI without blocking on a worker. */
    private static native void nativeError(String error);
}
