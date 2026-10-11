package dev.snowflake.vulkan;

import android.os.Parcel;
import android.view.Surface;
import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowBinder;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35)
public class RootDisplayBrokerTest {
    private static final class Host implements RootDisplayBroker.Host {
        int releases;
        boolean running = true;
        public boolean running() { return running; }
        public int[] geometry(int width, int height, float density) { return new int[]{320, 180, 51, 1}; }
        public void surface(Surface surface, float density) { if (surface == null) releases++; }
    }
    @After public void resetIdentity() { ShadowBinder.reset(); }

    @Config(sdk = {35, 36})
    @Test public void androidSurfaceBridgeMethodsExist() throws Exception {
        Class<?> control = Class.forName("android.view.SurfaceControl");
        Class<?> builder = Class.forName("android.view.SurfaceControl$Builder");
        Class<?> transaction = Class.forName("android.view.SurfaceControl$Transaction");
        assertNotNull(builder.getConstructor());
        assertNotNull(builder.getMethod("setBufferSize", int.class, int.class));
        assertNotNull(transaction.getMethod("setLayerStack", control, int.class));
        assertNotNull(transaction.getMethod("remove", control));
        assertNotNull(transaction.getMethod("hide", control));
        assertNotNull(transaction.getMethod("setTrustedOverlay", control, boolean.class));
        assertNotNull(transaction.getMethod("show", control));
        assertNotNull(Surface.class.getConstructor());
        assertNotNull(Surface.class.getMethod("copyFrom", control));
        assertNotNull(Class.forName("android.app.IActivityManager").getMethod("getContentProviderExternal", String.class, int.class, android.os.IBinder.class, String.class));
        assertNotNull(Class.forName("android.app.IActivityManager").getMethod("removeContentProviderExternalAsUser", String.class, android.os.IBinder.class, int.class));
    }

    @Test public void rejectsOrdinaryUidWrongTokenAndReplay() {
        RootDisplayBroker broker = new RootDisplayBroker(new Host());
        ShadowBinder.setCallingUid(10000);
        assertThrows(SecurityException.class, () -> broker.connect(broker.token));
        ShadowBinder.setCallingUid(0);
        assertThrows(SecurityException.class, () -> broker.connect("wrong-token"));
        assertSame(broker, broker.connect(broker.token));
        assertThrows(SecurityException.class, () -> broker.connect(broker.token));
        broker.close();
        assertThrows(SecurityException.class, () -> broker.connect(broker.token));
    }

    private Parcel state(RootDisplayBroker broker, int width, float density) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        data.writeInterfaceToken(RootDisplayBroker.DESCRIPTOR);
        data.writeInt(width); data.writeInt(1080); data.writeFloat(density);
        data.setDataPosition(0);
        try { assertTrue(broker.onTransact(RootDisplayBroker.STATE, data, reply, 0)); }
        catch (Exception error) { reply.recycle(); throw error; }
        finally { data.recycle(); }
        reply.setDataPosition(0);
        reply.readException();
        return reply;
    }

    @Test public void revocationStopsRootPollingAndDetachesRenderer() throws Exception {
        Host host = new Host();
        RootDisplayBroker broker = new RootDisplayBroker(host);
        ShadowBinder.setCallingUid(0);
        broker.connect(broker.token);
        Parcel reply = state(broker, 1920, 3.5f);
        assertEquals(1, reply.readInt());
        assertArrayEquals(new int[]{320, 180, 51, 1}, reply.createIntArray());
        reply.recycle();
        broker.close();
        assertEquals(1, host.releases);
        reply = state(broker, 1920, 3.5f);
        assertEquals(0, reply.readInt());
        reply.recycle();
    }

    @Test public void rejectsMalformedGeometryAndForeignBinderCalls() {
        RootDisplayBroker broker = new RootDisplayBroker(new Host());
        ShadowBinder.setCallingUid(0);
        broker.connect(broker.token);
        assertThrows(IllegalArgumentException.class, () -> state(broker, 0, 3.5f));
        assertThrows(IllegalArgumentException.class, () -> state(broker, 1920, Float.NaN));
        assertThrows(IllegalArgumentException.class, () -> state(broker, Integer.MAX_VALUE, 3.5f));
        ShadowBinder.setCallingUid(10000);
        assertThrows(SecurityException.class, () -> state(broker, 1920, 3.5f));
    }
}
