package dev.snowflake.vulkan;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Build;

/** Platform opacity policy for a non-interactive application overlay. */
final class HudTouchPolicy {
    private HudTouchPolicy() {}

    /** Called on the service main thread; stays strictly below the device's occlusion limit. */
    static float opacity(Context context) {
        if (Build.VERSION.SDK_INT < 31) return 0.8f;
        InputManager input = context.getSystemService(InputManager.class);
        return belowLimit(input == null ? 0f : input.getMaximumObscuringOpacityForTouch());
    }

    /** Invalid or unavailable limits fail transparent instead of blocking the underlying app. */
    static float belowLimit(float limit) {
        if (!Float.isFinite(limit) || limit <= 0f) return 0f;
        return Math.max(0f, Math.min(0.8f, limit) - 0.01f);
    }
}
