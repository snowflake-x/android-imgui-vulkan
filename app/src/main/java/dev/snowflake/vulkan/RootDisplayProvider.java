package dev.snowflake.vulkan;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;

/** Root bootstrap rendezvous only; no configuration, files or metrics are exported. */
public final class RootDisplayProvider extends ContentProvider {
    private static RootDisplayBroker broker;

    /** Service main-thread lifecycle; replacing a session revokes every old Binder handle. */
    static synchronized void install(RootDisplayBroker next) {
        if (broker != null) broker.close();
        broker = next;
    }

    @Override public boolean onCreate() { return true; }

    /** Validates root identity and a one-use nonce before returning the live Surface channel. */
    @Override public Bundle call(String method, String arg, Bundle extras) {
        synchronized (RootDisplayProvider.class) {
            if (!"connect".equals(method) || broker == null)
                throw new SecurityException("No root display session");
            Bundle result = new Bundle();
            result.putBinder("display", broker.connect(arg));
            return result;
        }
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) { throw new UnsupportedOperationException(); }
    @Override public String getType(Uri uri) { return null; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
}
