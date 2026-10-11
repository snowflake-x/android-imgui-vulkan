package dev.snowflake.vulkan;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;

/** System notification bridge; metric text and display state are supplied by the native session. */
final class HudNotification {
    static final int ID = 3001;
    static final String CHANNEL = "performance_hud";
    static final String COLLAPSE = "dev.snowflake.vulkan.hud.COLLAPSE";
    static final String EXPAND = "dev.snowflake.vulkan.hud.EXPAND";
    static final String STOP = "dev.snowflake.vulkan.hud.STOP";

    static boolean available(Context context) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        NotificationChannel channel = manager.getNotificationChannel(CHANNEL);
        return manager.areNotificationsEnabled() && (channel == null || channel.getImportance() != NotificationManager.IMPORTANCE_NONE);
    }

    /** Reads only this app's notification state; promotion permission is not proof of a visible chip. */
    static String status(Context context) {
        try {
            if (!available(context)) return "blocked";
            if (Build.VERSION.SDK_INT < 36) return "unsupported";
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            for (android.service.notification.StatusBarNotification entry : manager.getActiveNotifications()) {
                if (entry.getId() == ID && (entry.getNotification().flags & Notification.FLAG_PROMOTED_ONGOING) != 0)
                    return "promoted";
            }
            return manager.canPostPromotedNotifications() ? "eligible" : "not_allowed";
        } catch (RuntimeException | LinkageError error) { return "unavailable"; }
    }

    static Intent settings(Context context) {
        Intent intent = new Intent("android.settings.MANAGE_APP_PROMOTED_NOTIFICATIONS")
            .putExtra(Settings.EXTRA_APP_PACKAGE, context.getPackageName());
        if (Build.VERSION.SDK_INT >= 36 && intent.resolveActivity(context.getPackageManager()) != null) return intent;
        return new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, context.getPackageName());
    }

    private static PendingIntent command(Context context, String action) {
        return PendingIntent.getService(context, 0, new Intent(context, HudService.class).setAction(action),
            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    /** Builds a standard live notification; Android owns promotion, layout and capsule interactions. */
    static Notification build(Context context, boolean enabled, boolean collapsed, boolean chinese, String compact, String body) {
        context.getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel(
            CHANNEL, "Performance HUD", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(context, 0, new Intent(context, MainActivity.class),
            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder builder = new Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_hud_notification)
            .setContentTitle(chinese ? "性能监测" : "Performance monitoring")
            .setContentText(enabled ? compact : (chinese ? "点击配置性能插件" : "Tap to configure performance monitoring"))
            .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true).setShowWhen(false)
            .setVisibility(Notification.VISIBILITY_PRIVATE).setCategory(Notification.CATEGORY_STATUS);
        if (enabled) {
            builder.setStyle(new Notification.BigTextStyle().bigText(body));
            builder.addAction(new Notification.Action.Builder(null,
                collapsed ? (chinese ? "展开 HUD" : "Expand HUD") : (chinese ? "收起 HUD" : "Collapse HUD"),
                command(context, collapsed ? EXPAND : COLLAPSE)).build());
        }
        if (enabled && collapsed && Build.VERSION.SDK_INT >= 36) {
            Bundle extras = new Bundle();
            // Public Android 36.1 key, sent as an extra to support compileSdk 36 and OEM backports.
            extras.putBoolean("android.requestPromotedOngoing", true);
            builder.addExtras(extras).setShortCriticalText(compact);
        }
        builder.addAction(new Notification.Action.Builder(null, chinese ? "停止" : "Stop", command(context, STOP)).build());
        // Dismissing the live activity ends this user-started session; do not continually repost it.
        builder.setDeleteIntent(command(context, STOP));
        return builder.build();
    }
}
