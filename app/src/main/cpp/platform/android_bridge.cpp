#include <android/asset_manager_jni.h>
#include "platform/android_bridge.h"
#include "platform/system_fonts.h"
#include "monitor/hud_view.h"
#include "platform/overlay_renderer.h"
#include "manager/manager.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/system_properties.h>
#include <thread>
#include <vector>
#include <unistd.h>

namespace vulkanscope {
namespace {
JavaVM *vm = nullptr;
jobject context = nullptr;
std::mutex contextMutex;
std::atomic<bool> overlayCancel{true};
std::thread overlayThread;
std::thread rootDisplayThread;
std::atomic<bool> rootDisplayCancel{true};
std::atomic<bool> rootDisplayStopping{true};
/** @brief Service 生命周期串行调用；取消 root 子进程并回收所属工作线程。 */
void stopRootDisplay() {
    rootDisplayStopping = true;
    rootDisplayCancel = true;
    if (rootDisplayThread.joinable()) rootDisplayThread.join();
}
/** @brief 每次采样建立局部引用帧；后台线程离开作用域时自动从 VM 分离。 */
struct Jni {
    JNIEnv *env = nullptr;
    bool attached = false;
    Jni() {
        if (!vm)
            return;
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
            if (!attached)
                env = nullptr;
        }
        if (env)
            env->PushLocalFrame(128);
    }
    ~Jni() {
        if (env) {
            if (env->ExceptionCheck())
                env->ExceptionClear();
            env->PopLocalFrame(nullptr);
        }
        if (attached)
            vm->DetachCurrentThread();
    }
};
jmethodID method(JNIEnv *e, jobject o, const char *name, const char *sig) {
    auto cls = e->GetObjectClass(o);
    auto id = e->GetMethodID(cls, name, sig);
    e->DeleteLocalRef(cls);
    return id;
}
jobject service(JNIEnv *e, const char *name) {
    return e->CallObjectMethod(
        context, method(e, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"),
        e->NewStringUTF(name));
}
std::string utf(JNIEnv *e, jstring value) {
    if (!value)
        return {};
    auto encoding = e->NewStringUTF("UTF-8");
    auto data = static_cast<jbyteArray>(e->CallObjectMethod(
        value, method(e, value, "getBytes", "(Ljava/lang/String;)[B"), encoding));
    e->DeleteLocalRef(encoding);
    if (!data || e->ExceptionCheck()) return {};
    std::string text(static_cast<std::size_t>(e->GetArrayLength(data)), '\0');
    e->GetByteArrayRegion(data, 0, static_cast<jsize>(text.size()), reinterpret_cast<jbyte *>(text.data()));
    e->DeleteLocalRef(data);
    return text;
}
std::string property(const char *key) {
    char value[PROP_VALUE_MAX]{};
    __system_property_get(key, value);
    return value;
}
bool exception(JNIEnv *e) {
    if (!e->ExceptionCheck())
        return false;
    e->ExceptionClear();
    return true;
}
/** @brief 首次升级用 C++ 导入旧偏好，后续只读写原生私有配置文件。 */
void migratePreferences(JNIEnv *e, const std::string &directory) {
    if (std::ifstream(directory + "/native-hud.conf").good() ||
        !std::ifstream(directory + "/../shared_prefs/performance_hud.xml").good())
        return;
    e->PushLocalFrame(128);
    auto prefs = e->CallObjectMethod(context,
                                     method(e, context, "getSharedPreferences",
                                            "(Ljava/lang/String;I)Landroid/content/SharedPreferences;"),
                                     e->NewStringUTF("performance_hud"), 0);
    if (!prefs || exception(e)) {
        e->PopLocalFrame(nullptr);
        return;
    }
    monitor::Config config;
    bool valid = true;
    auto boolean = [&](const char *name, bool fallback) {
        auto result = e->CallBooleanMethod(prefs, method(e, prefs, "getBoolean", "(Ljava/lang/String;Z)Z"),
                                           e->NewStringUTF(name), fallback);
        if (exception(e)) {
            valid = false;
            return static_cast<jboolean>(fallback);
        }
        return result;
    };
    auto integer = [&](const char *name, int fallback) {
        auto result = e->CallIntMethod(prefs, method(e, prefs, "getInt", "(Ljava/lang/String;I)I"),
                                       e->NewStringUTF(name), fallback);
        if (exception(e)) {
            valid = false;
            return fallback;
        }
        return result;
    };
    auto string = [&](const char *name, const char *fallback) {
        auto value = static_cast<jstring>(e->CallObjectMethod(
            prefs, method(e, prefs, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
            e->NewStringUTF(name), e->NewStringUTF(fallback)));
        if (exception(e)) {
            valid = false;
            return std::string(fallback);
        }
        return utf(e, value);
    };
    config.root = boolean("root_enabled", false);
    config.interval =
        static_cast<int>(e->CallLongMethod(prefs, method(e, prefs, "getLong", "(Ljava/lang/String;J)J"),
                                           e->NewStringUTF("interval_ms"), static_cast<jlong>(1000)));
    if (exception(e))
        valid = false;
    config.textSize = integer("text_sp", 14);
    config.opacity = integer("opacity", 80);
    config.surface = string("surface_override", "");
    config.powerPath = string("cpu_power_path", "");
    config.powerScale = monitor::Number(string("cpu_power_scale", "0.000001")).value_or(0.000001);
    auto corner = string("corner", "top_left");
    config.corner = corner == "top_right"      ? 1
                    : corner == "bottom_left"  ? 2
                    : corner == "bottom_right" ? 3
                                               : 0;
    std::istringstream order(string("order", ""));
    std::string id;
    config.order.clear();
    while (std::getline(order, id, ','))
        config.order.push_back(id);
    auto selected = e->CallObjectMethod(
        prefs, method(e, prefs, "getStringSet", "(Ljava/lang/String;Ljava/util/Set;)Ljava/util/Set;"),
        e->NewStringUTF("enabled"), nullptr);
    if (selected && !exception(e)) {
        for (const auto &metric : monitor::Metrics())
            config.enabled[metric.id] =
                e->CallBooleanMethod(selected, method(e, selected, "contains", "(Ljava/lang/Object;)Z"),
                                     e->NewStringUTF(metric.id));
    }
    if (!exception(e) && valid) {
        std::string error;
        monitor::SaveConfig(directory + "/native-hud.conf", config, error);
    }
    e->PopLocalFrame(nullptr);
}
void initialize(JNIEnv *e, jobject appContext, jstring directory) {
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (!context) {
            e->GetJavaVM(&vm);
            context = e->NewGlobalRef(appContext);
        }
    }
    const auto path = utf(e, directory);
    migratePreferences(e, path);
    monitor::NativeSession().initialize(path, ReadPlatformSample);
}
void stopOverlay() {
    overlayCancel = true;
    if (overlayThread.joinable())
        overlayThread.join();
}
}
monitor::PlatformSample ReadPlatformSample() {
    monitor::PlatformSample sample;
    Jni j;
    auto *e = j.env;
    if (!e || !context)
        return sample;
    sample.system = "Device: " + property("ro.product.manufacturer") + " " + property("ro.product.model") +
                    "\nAndroid: " + property("ro.build.version.release") + " (API " +
                    property("ro.build.version.sdk") + ")" + "\nABI: " + property("ro.product.cpu.abilist") +
                    "\nCPU cores: " + std::to_string(sysconf(_SC_NPROCESSORS_ONLN));
    std::istringstream memory(monitor::ReadFile("/proc/meminfo", 8192));
    std::string line;
    long long total = 0, available = -1;
    while (std::getline(memory, line)) {
        std::istringstream row(line);
        std::string key;
        long long n;
        if (row >> key >> n) {
            if (key == "MemTotal:")
                total = n;
            if (key == "MemAvailable:")
                available = n;
        }
    }
    if (total > 0 && available >= 0)
        sample.system += "\nRAM available / total: " + std::to_string(available / 1024) + " / " +
                         std::to_string(total / 1024) + " MiB";
    auto power = service(e, "power");
    if (power && !exception(e)) {
        sample.interactive = e->CallBooleanMethod(power, method(e, power, "isInteractive", "()Z"));
        auto thermal = method(e, power, "getCurrentThermalStatus", "()I");
        if (thermal && !exception(e)) {
            int level = e->CallIntMethod(power, thermal);
            const char *names[] = {"None",     "Light",     "Moderate", "Severe",
                                   "Critical", "Emergency", "Shutdown"};
            if (!exception(e) && level >= 0 && level < 7)
                sample.values["thermal"] = std::string(names[level]) + " (Android thermal pressure)";
        }
        exception(e);
    }
    if (!monitor::NativeSession().rootDisplayActive()) {
        auto settings = e->FindClass("android/provider/Settings");
        auto canDraw = e->GetStaticMethodID(settings, "canDrawOverlays", "(Landroid/content/Context;)Z");
        sample.overlayAllowed = e->CallStaticBooleanMethod(settings, canDraw, context);
        exception(e);
    }
    auto displays = service(e, "display");
    if (displays && !exception(e)) {
        auto display =
            e->CallObjectMethod(displays, method(e, displays, "getDisplay", "(I)Landroid/view/Display;"), 0);
        if (display && !exception(e)) {
            float refresh = e->CallFloatMethod(display, method(e, display, "getRefreshRate", "()F"));
            int rotation = e->CallIntMethod(display, method(e, display, "getRotation", "()I"));
            auto mode =
                e->CallObjectMethod(display, method(e, display, "getMode", "()Landroid/view/Display$Mode;"));
            if (mode && !exception(e)) {
                int width = e->CallIntMethod(mode, method(e, mode, "getPhysicalWidth", "()I"));
                int height = e->CallIntMethod(mode, method(e, mode, "getPhysicalHeight", "()I"));
                sample.values["display_resolution"] =
                    std::to_string(width) + " x " + std::to_string(height) + " px (display mode)";
            }
            if (std::isfinite(refresh) && refresh > 0)
                sample.values["display_refresh"] = monitor::Format(refresh, 2) + " Hz";
            auto metricsClass = e->FindClass("android/util/DisplayMetrics");
            auto metrics = e->NewObject(metricsClass, e->GetMethodID(metricsClass, "<init>", "()V"));
            e->CallVoidMethod(
                display, method(e, display, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V"), metrics);
            auto integer = [&](const char *name) {
                return e->GetIntField(metrics, e->GetFieldID(metricsClass, name, "I"));
            };
            auto number = [&](const char *name) {
                return e->GetFloatField(metrics, e->GetFieldID(metricsClass, name, "F"));
            };
            sample.system += "\nDisplay: " + std::to_string(integer("widthPixels")) + " x " +
                             std::to_string(integer("heightPixels")) +
                             "\nDensity: " + std::to_string(integer("densityDpi")) + " dpi / scale " +
                             monitor::Format(number("density"), 2) +
                             "\nPhysical DPI (reported): " + monitor::Format(number("xdpi")) + " x " +
                             monitor::Format(number("ydpi")) + "\nRefresh: " + monitor::Format(refresh, 2) +
                             " Hz\nRotation: " + std::to_string(rotation * 90) + " degrees";
        }
        exception(e);
    }
    auto filterClass = e->FindClass("android/content/IntentFilter");
    auto filter = e->NewObject(filterClass, e->GetMethodID(filterClass, "<init>", "(Ljava/lang/String;)V"),
                               e->NewStringUTF("android.intent.action.BATTERY_CHANGED"));
    auto battery = e->CallObjectMethod(
        context,
        method(e, context, "registerReceiver",
               "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;"),
        nullptr, filter);
    if (battery && !exception(e)) {
        auto extra = [&](const char *name, int fallback) {
            return e->CallIntMethod(battery, method(e, battery, "getIntExtra", "(Ljava/lang/String;I)I"),
                                    e->NewStringUTF(name), fallback);
        };
        int temperature = extra("temperature", INT32_MIN), voltage = extra("voltage", -1);
        if (temperature != INT32_MIN)
            sample.values["battery_temp"] = monitor::Format(temperature / 10.0) + " C (battery)";
        auto manager = service(e, "batterymanager");
        if (manager && !exception(e)) {
            int current = e->CallIntMethod(manager, method(e, manager, "getIntProperty", "(I)I"), 2);
            if (current != INT32_MIN && voltage > 0)
                sample.values["battery_power"] =
                    monitor::Format(std::abs(static_cast<double>(current)) * voltage / 1e9, 2) +
                    " W (whole-device battery estimate)";
        }
    }
    exception(e);
    return sample;
}
void CopyText(const std::string &text) {
    Jni j;
    auto *e = j.env;
    if (!e || !context)
        return;
    auto clipboard = service(e, "clipboard");
    if (!clipboard || exception(e))
        return;
    auto clipClass = e->FindClass("android/content/ClipData");
    auto clip = e->CallStaticObjectMethod(
        clipClass,
        e->GetStaticMethodID(clipClass, "newPlainText",
                             "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;"),
        e->NewStringUTF("VulkanScope"), e->NewStringUTF(text.c_str()));
    if (clip && !exception(e))
        e->CallVoidMethod(clipboard, method(e, clipboard, "setPrimaryClip", "(Landroid/content/ClipData;)V"),
                          clip);
}
std::string PasteText() {
    Jni j;
    auto *e = j.env;
    if (!e || !context)
        return {};
    auto clipboard = service(e, "clipboard");
    if (!clipboard || exception(e))
        return {};
    auto clip = e->CallObjectMethod(clipboard,
                                    method(e, clipboard, "getPrimaryClip", "()Landroid/content/ClipData;"));
    if (!clip || exception(e))
        return {};
    auto count = e->CallIntMethod(clip, method(e, clip, "getItemCount", "()I"));
    if (count <= 0 || exception(e))
        return {};
    auto item =
        e->CallObjectMethod(clip, method(e, clip, "getItemAt", "(I)Landroid/content/ClipData$Item;"), 0);
    auto text = e->CallObjectMethod(
        item, method(e, item, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;"),
        context);
    if (!text || exception(e))
        return {};
    auto string =
        static_cast<jstring>(e->CallObjectMethod(text, method(e, text, "toString", "()Ljava/lang/String;")));
    return exception(e) ? std::string{} : utf(e, string);
}
float ReadSystemFontScale() {
    Jni j;
    auto *e = j.env;
    if (!e || !context) return 1;
    auto resources = e->CallObjectMethod(context, method(e, context, "getResources", "()Landroid/content/res/Resources;"));
    if (exception(e) || !resources) return 1;
    auto config = e->CallObjectMethod(resources, method(e, resources, "getConfiguration", "()Landroid/content/res/Configuration;"));
    if (exception(e) || !config) return 1;
    auto field = e->GetFieldID(e->GetObjectClass(config), "fontScale", "F");
    if (exception(e) || !field) return 1;
    const float scale = e->GetFloatField(config, field);
    return !exception(e) && std::isfinite(scale) && scale > 0 ? std::clamp(scale, .5f, 3.f) : 1;
}
std::string ReadSystemLocale() {
    Jni j;
    auto *e = j.env;
    if (!e || !context) return "en";
    auto resources = e->CallObjectMethod(context, method(e, context, "getResources", "()Landroid/content/res/Resources;"));
    if (exception(e) || !resources) return "en";
    auto configuration = e->CallObjectMethod(resources, method(e, resources, "getConfiguration", "()Landroid/content/res/Configuration;"));
    if (exception(e) || !configuration) return "en";
    auto locales = e->CallObjectMethod(configuration, method(e, configuration, "getLocales", "()Landroid/os/LocaleList;"));
    if (exception(e) || !locales) return "en";
    auto tags = static_cast<jstring>(e->CallObjectMethod(locales, method(e, locales, "toLanguageTags", "()Ljava/lang/String;")));
    return exception(e) || !tags ? "en" : utf(e, tags);
}
void LoadHudFont(float pixels) {
    Jni j;
    auto *e = j.env;
    AAsset *asset = nullptr;
    if (e && context) {
        auto manager = e->CallObjectMethod(context, method(e, context, "getAssets", "()Landroid/content/res/AssetManager;"));
        if (!exception(e) && manager) {
            auto *native = AAssetManager_fromJava(e, manager);
            if (native) asset = AAssetManager_open(native, "fonts/DejaVuSansMono.ttf", AASSET_MODE_BUFFER);
        }
    }
    if (asset) {
        const auto length = AAsset_getLength(asset);
        void *data = length > 0 ? ImGui::MemAlloc(length) : nullptr;
        const bool read = data && AAsset_read(asset, data, length) == length;
        AAsset_close(asset);
        if (read) {
            ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(length), pixels);
            return;
        }
        ImGui::MemFree(data);
    }
    ImFontConfig font;
    font.SizePixels = pixels;
    ImGui::GetIO().Fonts->AddFontDefault(&font);
}
void HudAction(JNIEnv *env, jobject activity, int action) {
    auto id = method(env, activity, "hudAction", "(I)V");
    if (id)
        env->CallVoidMethod(activity, id, action);
    if (exception(env))
        monitor::NativeSession().setError("Android lifecycle action failed");
}
namespace {
/** @brief Capture immutable package paths for bounded root launches; no JNI references escape. */
MemoryExecutor memoryExecutor() {
    Jni j;
    auto *e = j.env;
    if (!e || !context) return {};
    auto info = e->CallObjectMethod(context, method(e, context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;"));
    if (!info || exception(e)) return {};
    auto type = e->GetObjectClass(info);
    auto apkObject = static_cast<jstring>(e->GetObjectField(info, e->GetFieldID(type, "sourceDir", "Ljava/lang/String;")));
    const auto apk = utf(e, apkObject);
    if (exception(e) || apk.empty()) return {};
    /** @brief Unextracted libraries live at APK!/lib/ABI paths, including ABI split APKs.
     * Resolve through Android's loader instead of assuming nativeLibraryDir contains a file. */
    auto loader = e->CallObjectMethod(context, method(e, context, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    if (exception(e) || !loader) return {};
    auto findLibrary = method(e, loader, "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;");
    if (exception(e) || !findLibrary) return {};
    auto libObject = static_cast<jstring>(e->CallObjectMethod(loader, findLibrary, e->NewStringUTF("vulkanscope")));
    if (exception(e) || !libObject) return {};
    const auto library = utf(e, libObject);
    if (exception(e) || library.empty()) return {};
    auto injectorObject = static_cast<jstring>(e->CallObjectMethod(loader, findLibrary, e->NewStringUTF("vulkanscope_injector")));
    if (exception(e)) return {};
    auto agentObject = static_cast<jstring>(e->CallObjectMethod(loader, findLibrary, e->NewStringUTF("vulkanscope_unity")));
    if (exception(e)) return {};
    const auto injector = utf(e, injectorObject), agent = utf(e, agentObject);
    return [apk, library, injector, agent](const MemoryJob &job, const std::atomic<bool> &cancel) {
        std::string encoded;
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : job.input) { encoded += hex[c >> 4]; encoded += hex[c & 15]; }
        if (encoded.empty()) encoded = "00";
        auto command = "CLASSPATH=" + monitor::Quote(apk) +
            " exec /system/bin/app_process /system/bin dev.snowflake.vulkan.MemoryProbeBootstrap " +
            monitor::Quote(library) + " " + monitor::Quote(job.operation) + " " + std::to_string(job.pid) +
            " " + std::to_string(job.start) + " " + monitor::Quote(job.kind) + " " + monitor::Quote(encoded);
        if (job.operation == "runtime") command += " " + monitor::Quote(injector) + " " + monitor::Quote(agent);
        return monitor::RunCommand({"su", "-c", command}, cancel, 30000, 512u << 10);
    };
}
/** @brief Kotlin 的进程级单线程调度器持有此宿主，JNI 不执行 ImGui 渲染。 */
Manager &manager() {
    static Manager value(monitor::NativeSession(), memoryExecutor());
    return value;
}
/** @brief 使用标准 UTF-8 字节数组避免 JNI Modified UTF-8 损坏系统字体支持的扩展字符。 */
jbyteArray bytes(JNIEnv *env, const std::string &text) {
    auto result = env->NewByteArray(static_cast<jsize>(text.size()));
    if (result) env->SetByteArrayRegion(result, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte *>(text.data()));
    return result;
}
void managerError(JNIEnv *env, const std::exception &error) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.what());
}
/** @brief 主界面显示硬件能力不再依赖创建 Vulkan 渲染 Surface。 */
void readGpuProperties() {
    VkInstance instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) return;
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count > 0 && count <= 16) {
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS && count > 0) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(devices.front(), &p);
            monitor::NativeSession().gpu({{"gpu_model", p.deviceName}, {"gpu_driver", std::to_string(p.driverVersion)},
                {"gpu_api", std::to_string(VK_VERSION_MAJOR(p.apiVersion)) + "." + std::to_string(VK_VERSION_MINOR(p.apiVersion)) + "." + std::to_string(VK_VERSION_PATCH(p.apiVersion))}});
        }
    }
    vkDestroyInstance(instance, nullptr);
}
}
/** @brief Root helper entry without initializing app services, Vulkan, or a manager runtime. */
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_snowflake_vulkan_MemoryProbeBootstrap_nativeProbe(
    JNIEnv *e, jclass, jstring operation, jint pid, jlong start, jstring kind, jstring input) {
    try {
        auto decoded = memory::Pattern("hex", utf(e, input));
        return bytes(e, memory::Probe(utf(e, operation), pid, start, utf(e, kind),
            std::string(decoded.begin(), decoded.end())));
    }
    catch (const std::exception &error) { managerError(e, error); return nullptr; }
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_NativeManager_initialize(JNIEnv *e, jobject, jobject c, jstring path) {
    try {
        initialize(e, c, path);
        (void)manager();
        monitor::NativeSession().startSystemInfo();
        monitor::NativeSession().requestStartupRoot();
        readGpuProperties();
    } catch (const std::exception &error) { managerError(e, error); }
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_snowflake_vulkan_NativeManager_snapshot(JNIEnv *e, jobject) {
    try {
        const auto language = ui::ResolveLanguage(monitor::NativeSession().config().language, ReadSystemLocale());
        return bytes(e, manager().snapshot(language));
    } catch (const std::exception &error) { managerError(e, error); return nullptr; }
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_NativeManager_command(JNIEnv *e, jobject, jstring key, jstring value) {
    try { return manager().command(utf(e, key), utf(e, value)); }
    catch (const std::exception &error) { managerError(e, error); return false; }
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_dev_snowflake_vulkan_NativeManager_diagnostic(JNIEnv *e, jobject, jstring package, jint user) {
    try { return bytes(e, manager().diagnostic(utf(e, package), user)); }
    catch (const std::exception &error) { managerError(e, error); return nullptr; }
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_NativeManager_pause(JNIEnv *e, jobject) {
    try {
        manager().pauseMemory();
        monitor::NativeSession().shutdownDiagnostics();
        monitor::NativeSession().stopSystemInfo();
    } catch (const std::exception &error) { managerError(e, error); }
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_MainActivity_nativeInitialize(JNIEnv *e, jclass,
                                                                                          jobject c,
                                                                                          jstring path) {
    initialize(e, c, path);
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_MainActivity_nativeError(JNIEnv *e, jclass,
                                                                                     jstring error) {
    monitor::NativeSession().fail(utf(e, error));
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeError(JNIEnv *e, jclass,
                                                                                   jstring error) {
    monitor::NativeSession().fail(utf(e, error));
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeStart(JNIEnv *e, jclass,
                                                                                   jobject c, jstring path, jboolean rootDisplay) {
    initialize(e, c, path);
    monitor::NativeSession().start(rootDisplay);
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_HudService_nativeRootMode(JNIEnv *, jclass) {
    const auto c = monitor::NativeSession().config();
    return c.rootDisplay && (c.root || monitor::NativeSession().rootAccess() == monitor::RootAccess::Granted);
}
/** @brief Root 仅创建系统 Surface；绑定回应用的 ImGui 渲染器，不在特权进程重复采集或写配置。 */
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeRootStart(
    JNIEnv *e, jclass, jstring apk, jstring uri, jstring token) {
    stopRootDisplay();
    try {
        const auto command = "CLASSPATH=" + monitor::Quote(utf(e, apk)) +
            " exec /system/bin/app_process /system/bin dev.snowflake.vulkan.RootHudBootstrap " +
            monitor::Quote(utf(e, uri)) + " " + monitor::Quote(utf(e, token));
        rootDisplayCancel = false;
        rootDisplayStopping = false;
        rootDisplayThread = std::thread([command] {
            try {
                const auto result = monitor::RunCommand({"su", "-c", command}, rootDisplayCancel,
                                                       std::numeric_limits<int>::max(), 8192);
                if (!rootDisplayStopping && !rootDisplayCancel && monitor::NativeSession().running())
                    monitor::NativeSession().fail("Root display stopped: " + result.error + "\n" + result.output +
                        "\nDisable Root display in settings to use the standard overlay compatibility mode.");
            } catch (const std::exception &error) {
                if (!rootDisplayStopping) monitor::NativeSession().fail(std::string("Root display worker failed: ") + error.what());
            }
        });
    } catch (const std::exception &error) {
        monitor::NativeSession().fail(std::string("Root display launch failed: ") + error.what());
    }
}
/** @brief 撤销 Binder 前标记正常停止，避免辅助进程先退出时被错误报告为显示故障。 */
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativePrepareStop(JNIEnv *, jclass) {
    rootDisplayStopping = true;
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeStop(JNIEnv *, jclass) {
    stopRootDisplay();
    stopOverlay();
    monitor::NativeSession().stop();
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_HudService_nativeRunning(JNIEnv *, jclass) {
    return monitor::NativeSession().running();
}
/** @brief Android 通知与 ImGui 消费同一原生发布序号，不独立重采样。 */
extern "C" JNIEXPORT jlong JNICALL Java_dev_snowflake_vulkan_HudService_nativeRevision(JNIEnv *, jclass) {
    return static_cast<jlong>(monitor::NativeSession().displayRevision());
}
/** @brief 非阻塞读取原生设置，Android 桥接据此提供通知操作。 */
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_HudService_nativeLiveUpdates(JNIEnv *, jclass) {
    return monitor::NativeSession().config().liveUpdates;
}
/** @brief 仅控制显示，保留采集线程与当前会话。 */
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeCollapse(JNIEnv *, jclass, jboolean value) {
    monitor::NativeSession().collapse(value);
}
/** @brief Root Binder 与服务主线程均可读取原子显示状态。 */
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_HudService_nativeCollapsed(JNIEnv *, jclass) {
    return monitor::NativeSession().collapsed();
}
/** @brief 通知数据来自与 ImGui 相同的有时效限制的原生快照，不在 Java 重复采集。 */
extern "C" JNIEXPORT jstring JNICALL Java_dev_snowflake_vulkan_HudService_nativeLiveText(JNIEnv *e, jclass) {
    const auto c = monitor::NativeSession().config();
    const auto h = monitor::NativeSession().hud();
    auto number = [](const std::optional<double> &value, int digits) {
        return value ? monitor::Format(*value, digits) : std::string("--");
    };
    const auto shortText = h.capturedNs > 0 && h.status.rfind("Last sample:", 0) != 0 &&
        h.values.find("sample_age_ms_frames") != h.values.end() &&
        monitor::Number(h.values.at("sample_age_ms_frames")).value_or(1e9) <= c.interval * 2
        ? number(h.fps.current, 0) + " FPS" : std::string("-- FPS");
    const auto target = h.values.find("foreground");
    const auto app = target == h.values.end() ? std::string{} : target->second.substr(0, 256);
    const auto body = app + "\nFPS: " + number(h.fps.current, 1) + " | Pre: " + number(h.present.current, 2) +
        " ms\nGPU: " + number(h.gpu.current, 2) + " " + h.gpuUnit + " | Mem (RSS): " + number(h.processMb, 1) +
        " MB\nGPU Mem: " + number(h.gpuMb, 1) + " MB\n" + h.status.substr(0, 512);
    const auto json = "{\"language\":" + JsonString(c.language) + ",\"short\":" + JsonString(shortText) +
        ",\"body\":" + JsonString(body) + "}";
    return e->NewStringUTF(json.c_str());
}
extern "C" JNIEXPORT jintArray JNICALL Java_dev_snowflake_vulkan_HudService_nativeGeometry(JNIEnv *e, jclass,
                                                                                           jint width,
                                                                                           jint height,
                                                                                           jfloat density) {
    const auto c = monitor::NativeSession().config();
    const auto size = monitor::MeasureHud(c, c.textSize * density, width, height);
    int gravity[] = {48 | 3, 48 | 5, 80 | 3, 80 | 5};
    jint values[] = {std::max(1, static_cast<int>(std::ceil(size.width))),
                     std::max(1, static_cast<int>(std::ceil(size.height))), gravity[c.corner]};
    auto result = e->NewIntArray(3);
    e->SetIntArrayRegion(result, 0, 3, values);
    return result;
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeSurface(JNIEnv *e, jclass,
                                                                                     jobject surface,
                                                                                     jfloat density) {
    stopOverlay();
    if (!surface)
        return;
    auto *window = ANativeWindow_fromSurface(e, surface);
    if (!window)
        return;
    overlayCancel = false;
    overlayThread = std::thread([window, density] {
        OverlayRenderer renderer;
        try {
            {
                std::lock_guard<std::mutex> lock(ImGuiMutex());
                renderer.initialize(window, density);
            }
            std::uint64_t revision = 0;
            while (!overlayCancel) {
                if (!monitor::NativeSession().waitForDisplay(revision, overlayCancel)) continue;
                auto config = monitor::NativeSession().config();
                auto snapshot = monitor::NativeSession().hud();
                bool visible = monitor::NativeSession().interactive() && !monitor::NativeSession().collapsed();
                if (!monitor::NativeSession().collapsed()) {
                    std::lock_guard<std::mutex> lock(ImGuiMutex());
                    renderer.render([&] {
                        if (!visible)
                            return;
                        const auto display = ImGui::GetIO().DisplaySize;
                        const auto size = monitor::MeasureHud(config, config.textSize * density, display.x, display.y);
                        monitor::DrawHud(*ImGui::GetForegroundDrawList(), ImVec2(0, 0), size, config, snapshot);

                    });
                }
            }
        } catch (const std::exception &error) {
            monitor::NativeSession().fail(std::string("Overlay renderer failed: ") + error.what());
        }
        {
            std::lock_guard<std::mutex> lock(ImGuiMutex());
            renderer.cleanup();
        }
        ANativeWindow_release(window);
    });
}
}
