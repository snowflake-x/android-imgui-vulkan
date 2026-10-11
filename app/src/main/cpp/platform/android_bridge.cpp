#include <android/asset_manager_jni.h>
#include "platform/android_bridge.h"
#include "monitor/hud_view.h"
#include "platform/overlay_renderer.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <sys/system_properties.h>
#include <thread>
#include <unistd.h>

namespace vulkanscope {
namespace {
JavaVM *vm = nullptr;
jobject context = nullptr;
std::mutex contextMutex;
std::atomic<bool> overlayCancel{true};
std::thread overlayThread;
std::mutex overlayWaitMutex;
std::condition_variable overlayWake;
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
    const char *chars = e->GetStringUTFChars(value, nullptr);
    if (!chars)
        return {};
    std::string text(chars);
    e->ReleaseStringUTFChars(value, chars);
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
    overlayWake.notify_all();
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
    auto settings = e->FindClass("android/provider/Settings");
    auto canDraw = e->GetStaticMethodID(settings, "canDrawOverlays", "(Landroid/content/Context;)Z");
    sample.overlayAllowed = e->CallStaticBooleanMethod(settings, canDraw, context);
    exception(e);
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
                                                                                   jobject c, jstring path) {
    initialize(e, c, path);
    monitor::NativeSession().start();
}
extern "C" JNIEXPORT void JNICALL Java_dev_snowflake_vulkan_HudService_nativeStop(JNIEnv *, jclass) {
    stopOverlay();
    monitor::NativeSession().stop();
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_snowflake_vulkan_HudService_nativeRunning(JNIEnv *, jclass) {
    return monitor::NativeSession().running();
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
            while (!overlayCancel) {
                auto config = monitor::NativeSession().config();
                auto snapshot = monitor::NativeSession().hud();
                bool visible = monitor::NativeSession().interactive();
                {
                    std::lock_guard<std::mutex> lock(ImGuiMutex());
                    renderer.render([&] {
                        if (!visible)
                            return;
                        const auto display = ImGui::GetIO().DisplaySize;
                        const auto size = monitor::MeasureHud(config, config.textSize * density, display.x, display.y);
                        monitor::DrawClassicHud(*ImGui::GetForegroundDrawList(), ImVec2(0, 0), size, config, snapshot);

                    });
                }
                std::unique_lock<std::mutex> lock(overlayWaitMutex);
                overlayWake.wait_for(lock, std::chrono::milliseconds(100),
                                     [] { return overlayCancel.load(); });
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
