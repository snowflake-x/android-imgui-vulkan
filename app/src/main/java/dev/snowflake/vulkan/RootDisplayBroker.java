package dev.snowflake.vulkan;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.view.Surface;
import java.util.UUID;

/** Revocable Surface lifecycle channel. The native host owns layout, collection and rendering. */
final class RootDisplayBroker extends Binder {
    static final String DESCRIPTOR = "dev.snowflake.vulkan.RootSurface.v2";
    static final int STATE = IBinder.FIRST_CALL_TRANSACTION;
    static final int ATTACH = STATE + 1;
    static final int DETACH = STATE + 2;

    interface Host {
        boolean running();
        int[] geometry(int width, int height, float density);
        void surface(Surface surface, float density);
    }

    private final Host host;
    final String token = UUID.randomUUID().toString();
    private boolean active = true;
    private boolean connected;
    private boolean attached;

    RootDisplayBroker(Host host) { this.host = host; }

    /** A root UID and a one-use per-service token are required even though the provider is exported. */
    synchronized IBinder connect(String suppliedToken) {
        if (Binder.getCallingUid() != 0 || !active || connected || !token.equals(suppliedToken))
            throw new SecurityException("Root display session rejected");
        connected = true;
        return this;
    }

    synchronized boolean attached() { return attached; }

    /** Revoke before stopping the native process; joins native rendering before releasing its Surface. */
    synchronized void close() {
        if (!active) return;
        active = false;
        attached = false;
        host.surface(null, 1);
    }

    /** Binder threads serialize Surface changes against service destruction. No metric logic runs here. */
    @Override
    protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        if (Binder.getCallingUid() != 0) throw new SecurityException("Root UID required");
        data.enforceInterface(DESCRIPTOR);
        if (reply == null) return false;
        if (code == STATE) {
            int width = data.readInt(), height = data.readInt();
            float density = data.readFloat();
            if (width < 1 || height < 1 || width > 32768 || height > 32768 ||
                !Float.isFinite(density) || density < 0.5f || density > 8)
                throw new IllegalArgumentException("Invalid display geometry");
            reply.writeNoException();
            boolean running = active && connected && host.running();
            reply.writeInt(running ? 1 : 0);
            if (running) reply.writeIntArray(host.geometry(width, height, density));
            return true;
        }
        if (code == ATTACH) {
            if (!active || !connected || !host.running()) throw new IllegalStateException("Display stopped");
            float density = data.readFloat();
            if (!Float.isFinite(density) || density < 0.5f || density > 8)
                throw new IllegalArgumentException("Invalid surface density");
            Surface surface = Surface.CREATOR.createFromParcel(data);
            try {
                if (!surface.isValid()) throw new IllegalArgumentException("Invalid root Surface");
                host.surface(surface, density);
                attached = true;
            } finally { surface.release(); }
            reply.writeNoException();
            return true;
        }
        if (code == DETACH) {
            if (active) host.surface(null, 1);
            attached = false;
            reply.writeNoException();
            return true;
        }
        return false;
    }
}
