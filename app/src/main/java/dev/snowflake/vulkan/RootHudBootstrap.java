package dev.snowflake.vulkan;

import android.os.Binder;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.os.SystemClock;
import android.view.Surface;
import java.lang.reflect.Method;

/**
 * app_process root entry point: owns only a SurfaceControl and its Android lifecycle.
 * The Surface is parcelled back to the app's C++ Vulkan/ImGui renderer. No WindowManager,
 * Android views, metric collection, preference persistence or native library loading occurs here.
 */
public final class RootHudBootstrap {
    private static final String DESCRIPTOR = RootDisplayBroker.DESCRIPTOR;
    private static volatile long heartbeat = SystemClock.uptimeMillis();

    private RootHudBootstrap() {}

    private static Object invoke(Object object, String name, Class<?>[] types, Object... args) throws Exception {
        Method method = object.getClass().getMethod(name, types);
        method.setAccessible(true);
        return method.invoke(object, args);
    }

    private static Parcel request(IBinder channel, int code, Parcel data) throws Exception {
        Parcel reply = Parcel.obtain();
        try {
            if (!channel.transact(code, data, reply, 0)) throw new IllegalStateException("Root display protocol unsupported");
            reply.readException();
            heartbeat = SystemClock.uptimeMillis();
            return reply;
        } catch (Exception error) { reply.recycle(); throw error; }
        finally { data.recycle(); }
    }

    private static Parcel data() {
        Parcel data = Parcel.obtain();
        data.writeInterfaceToken(DESCRIPTOR);
        return data;
    }

    /** Uses the same external-provider entry point as Android's root `content call` command. */
    private static IBinder connect(String address, String nonce) throws Exception {
        Uri uri = Uri.parse(address);
        String authority = uri.getHost();
        int user = Integer.parseInt(uri.getUserInfo());
        Object manager = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder lease = new Binder();
        Object holder = invoke(manager, "getContentProviderExternal",
            new Class<?>[]{String.class, int.class, IBinder.class, String.class}, authority, user, lease, "VulkanScopeRootDisplay");
        if (holder == null) throw new IllegalStateException("Root display provider unavailable");
        try {
            Object provider = holder.getClass().getField("provider").get(holder);
            for (Method call : Class.forName("android.content.IContentProvider").getMethods()) {
                if (!call.getName().equals("call")) continue;
                Class<?>[] parameters = call.getParameterTypes();
                Bundle result;
                if (parameters.length == 5 && parameters[0].getName().equals("android.content.AttributionSource")) {
                    Object builder = Class.forName("android.content.AttributionSource$Builder").getConstructor(int.class).newInstance(0);
                    invoke(builder, "setPackageName", new Class<?>[]{String.class}, "root");
                    Object attribution = invoke(builder, "build", new Class<?>[]{});
                    result = (Bundle) call.invoke(provider, attribution, authority, "connect", nonce, null);
                } else if (parameters.length == 6 && parameters[0] == String.class) {
                    result = (Bundle) call.invoke(provider, "root", null, authority, "connect", nonce, null);
                } else if (parameters.length == 5 && parameters[0] == String.class) {
                    result = (Bundle) call.invoke(provider, "root", authority, "connect", nonce, null);
                } else continue;
                IBinder channel = result == null ? null : result.getBinder("display");
                if (channel == null) throw new IllegalStateException("Root display channel unavailable");
                return channel;
            }
            throw new NoSuchMethodException("Unsupported IContentProvider.call signature");
        } finally {
            invoke(manager, "removeContentProviderExternalAsUser", new Class<?>[]{String.class, IBinder.class, int.class}, authority, lease, user);
        }
    }

    /** Bounded awake-time watchdog covers stalled Binder calls without expiring during deep sleep. */
    private static void watchdog() {
        Thread thread = new Thread(() -> {
            while (true) {
                if (SystemClock.uptimeMillis() - heartbeat > 15000) {
                    System.err.println("Root display heartbeat timed out");
                    System.exit(3);
                }
                SystemClock.sleep(1000);
            }
        }, "RootDisplayWatchdog");
        thread.setDaemon(true);
        thread.start();
    }

    /** Native launcher passes the provider URI and one-use token as separately shell-quoted arguments. */
    public static void main(String[] args) {
        Object layer = null, transaction = null;
        Surface surface = null;
        IBinder channel = null;
        int result = 0;
        try {
            if (Process.myUid() != 0 || args.length != 2) throw new SecurityException("Root UID required");
            watchdog();
            Looper.prepareMainLooper();
            channel = connect(args[0], args[1]);
            channel.linkToDeath(() -> System.exit(0), 0);
            Class<?> displayManager = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object displays = displayManager.getMethod("getInstance").invoke(null);
            Class<?> control = Class.forName("android.view.SurfaceControl");
            Class<?> builderClass = Class.forName("android.view.SurfaceControl$Builder");
            Class<?> transactionClass = Class.forName("android.view.SurfaceControl$Transaction");
            transaction = transactionClass.getConstructor().newInstance();
            int previousWidth = 0, previousHeight = 0;
            float previousDensity = 0;
            while (channel.isBinderAlive()) {
                Object info = invoke(displays, "getDisplayInfo", new Class<?>[]{int.class}, 0);
                if (info == null) throw new IllegalStateException("Default display unavailable");
                int width = info.getClass().getField("logicalWidth").getInt(info);
                int height = info.getClass().getField("logicalHeight").getInt(info);
                int stack = info.getClass().getField("layerStack").getInt(info);
                float density = info.getClass().getField("logicalDensityDpi").getInt(info) / 160f;
                Parcel state = data();
                state.writeInt(width); state.writeInt(height); state.writeFloat(density);
                Parcel reply = request(channel, 1, state);
                int[] size;
                try {
                    if (reply.readInt() == 0) break;
                    size = reply.createIntArray();
                } finally { reply.recycle(); }
                if (size == null || size.length != 4 || size[0] < 1 || size[1] < 1 || size[0] > width || size[1] > height)
                    throw new IllegalStateException("Invalid HUD size");
                if (layer == null) {
                    Object builder = builderClass.getConstructor().newInstance();
                    invoke(builder, "setName", new Class<?>[]{String.class}, "dev.snowflake.vulkan.RootPerformanceHUD");
                    invoke(builder, "setBufferSize", new Class<?>[]{int.class, int.class}, size[0], size[1]);
                    invoke(builder, "setFormat", new Class<?>[]{int.class}, PixelFormat.TRANSLUCENT);
                    layer = invoke(builder, "build", new Class<?>[]{});
                    // Android 12+ checks occlusion even for layers without an input channel.
                    // Only this UID-0 helper may mark its own HUD layer as trusted.
                    if (android.os.Build.VERSION.SDK_INT >= 31)
                        invoke(transaction, "setTrustedOverlay", new Class<?>[]{control, boolean.class}, layer, true);
                    surface = Surface.class.getConstructor().newInstance();
                    Surface.class.getMethod("copyFrom", control).invoke(surface, layer);
                }
                int x = (size[2] & 7) == 5 ? width - size[0] : 0;
                int y = (size[2] & 112) == 80 ? height - size[1] : 0;
                invoke(transaction, "setLayerStack", new Class<?>[]{control, int.class}, layer, stack);
                invoke(transaction, "setLayer", new Class<?>[]{control, int.class}, layer, Integer.MAX_VALUE - 1024);
                invoke(transaction, "setPosition", new Class<?>[]{control, float.class, float.class}, layer, (float) x, (float) y);
                invoke(transaction, "setBufferSize", new Class<?>[]{control, int.class, int.class}, layer, size[0], size[1]);
                invoke(transaction, size[3] == 0 ? "hide" : "show", new Class<?>[]{control}, layer);
                invoke(transaction, "apply", new Class<?>[]{});
                if (size[0] != previousWidth || size[1] != previousHeight || density != previousDensity) {
                    Parcel attach = data();
                    attach.writeFloat(density);
                    surface.writeToParcel(attach, 0);
                    request(channel, 2, attach).recycle();
                    previousWidth = size[0]; previousHeight = size[1]; previousDensity = density;
                }
                SystemClock.sleep(500);
            }
        } catch (Throwable error) {
            result = 1;
            System.err.println("Root SurfaceControl display failed (firmware/API/SELinux compatibility):");
            error.printStackTrace(System.err);
        } finally {
            if (channel != null && channel.isBinderAlive()) {
                try { request(channel, 3, data()).recycle(); } catch (Exception ignored) {}
            }
            if (surface != null) surface.release();
            if (layer != null) {
                try {
                    invoke(transaction, "remove", new Class<?>[]{Class.forName("android.view.SurfaceControl")}, layer);
                    invoke(transaction, "apply", new Class<?>[]{});
                } catch (Exception ignored) {}
                try { invoke(layer, "release", new Class<?>[]{}); } catch (Exception ignored) {}
            }
            if (transaction != null) {
                try { invoke(transaction, "close", new Class<?>[]{}); } catch (Exception ignored) {}
            }
        }
        System.exit(result);
    }
}
