package dev.snowflake.vulkan;

import org.junit.Test;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import static org.junit.Assert.*;

public class MemoryProbeBootstrapTest {
    private byte[] read(String path) throws Exception {
        Method method = MemoryProbeBootstrap.class.getDeclaredMethod("readAgent", String.class);
        method.setAccessible(true);
        return (byte[]) method.invoke(null, path);
    }
    @Test public void readsExtractedAndApkLibraries() throws Exception {
        Path file = Files.createTempFile("agent library ", ".so");
        Path apk = Files.createTempFile("agent package ", ".apk");
        try {
            byte[] bytes = new byte[] {0x7f, 'E', 'L', 'F', 0, 42};
            Files.write(file, bytes);
            assertArrayEquals(bytes, read(file.toString()));
            try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(apk))) {
                out.putNextEntry(new ZipEntry("lib/arm64-v8a/libvulkanscope_unity.so"));
                out.write(bytes); out.closeEntry();
            }
            assertArrayEquals(bytes, read(apk + "!/lib/arm64-v8a/libvulkanscope_unity.so"));
        } finally { Files.deleteIfExists(file); Files.deleteIfExists(apk); }
    }
    @Test public void missingApkEntryFailsClearly() throws Exception {
        Path apk = Files.createTempFile("agent-empty", ".apk");
        try {
            try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(apk))) {
                out.putNextEntry(new ZipEntry("unrelated")); out.write(1); out.closeEntry();
            }
            try { read(apk + "!/lib/arm64-v8a/libvulkanscope_unity.so"); fail("missing entry accepted"); }
            catch (InvocationTargetException error) { assertTrue(error.getCause() instanceof IOException); }
        } finally { Files.deleteIfExists(apk); }
    }
}
