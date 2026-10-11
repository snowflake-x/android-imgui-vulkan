package dev.snowflake.vulkan;

import org.junit.Test;
import static org.junit.Assert.*;

public class HudTouchPolicyTest {
    @Test public void honorsVendorLimitsWithRoundingMargin() {
        for (float limit : new float[]{0.8f, 0.5f, 0.01f, 1f}) {
            float opacity = HudTouchPolicy.belowLimit(limit);
            assertTrue(opacity >= 0f && opacity < limit && opacity <= 0.8f);
        }
    }

    @Test public void unavailableLimitCannotOccludeInput() {
        for (float limit : new float[]{0f, -1f, Float.NaN, Float.POSITIVE_INFINITY})
            assertEquals(0f, HudTouchPolicy.belowLimit(limit), 0f);
    }
}
