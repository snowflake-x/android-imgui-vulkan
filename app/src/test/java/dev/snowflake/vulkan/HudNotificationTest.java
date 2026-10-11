package dev.snowflake.vulkan;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import androidx.test.core.app.ApplicationProvider;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.Shadows;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = {35, 36})
public class HudNotificationTest {
    private final Context context = ApplicationProvider.getApplicationContext();

    @Test public void collapseRequestsPromotionAndAlwaysProvidesRestoreAndStop() {
        Notification n = HudNotification.build(context, true, true, true, "60 FPS", "Pre: 16.67 ms");
        assertTrue((n.flags & Notification.FLAG_ONGOING_EVENT) != 0);
        assertTrue((n.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0);
        assertEquals(android.os.Build.VERSION.SDK_INT >= 36, n.extras.getBoolean("android.requestPromotedOngoing"));
        assertFalse(n.extras.getBoolean(Notification.EXTRA_COLORIZED));
        assertNull(n.contentView);
        assertEquals("展开 HUD", n.actions[0].title);
        assertEquals(HudNotification.EXPAND, Shadows.shadowOf(n.actions[0].actionIntent).getSavedIntent().getAction());
        assertEquals(HudNotification.STOP, Shadows.shadowOf(n.actions[1].actionIntent).getSavedIntent().getAction());
        assertEquals(HudNotification.STOP, Shadows.shadowOf(n.deleteIntent).getSavedIntent().getAction());
        assertEquals("Pre: 16.67 ms", n.extras.getCharSequence(Notification.EXTRA_BIG_TEXT));
    }

    @Test public void expandedAndDisabledStatesDoNotRequestPromotion() {
        Notification expanded = HudNotification.build(context, true, false, false, "-- FPS", "Waiting");
        assertFalse(expanded.extras.getBoolean("android.requestPromotedOngoing"));
        assertEquals("Collapse HUD", expanded.actions[0].title);
        Notification disabled = HudNotification.build(context, false, false, false, "-- FPS", "Waiting");
        assertEquals(1, disabled.actions.length);
        assertFalse(disabled.extras.getBoolean("android.requestPromotedOngoing"));
    }

    @Test public void blockedChannelCannotBeUsedAsRecoveryEntry() {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(HudNotification.CHANNEL, "HUD", NotificationManager.IMPORTANCE_NONE));
        assertFalse(HudNotification.available(context));
        assertEquals("blocked", HudNotification.status(context));
    }
}
