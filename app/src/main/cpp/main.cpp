#include "monitor/hud_view.h"
#include <android_native_app_glue.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <jni.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_vulkan.h"
#include "features/builtin_plugins.h"
#include "features/performance_hud.h"
#include "platform/android_bridge.h"
#include "platform/overlay_renderer.h"
#include "monitor/settings_ui.h"

static android_app* app;
static JNIEnv* env;
static ImGuiContext* mainContext = nullptr;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static ImGui_ImplVulkanH_Window window;
static VkPhysicalDeviceProperties properties{};
static bool ready = false, focused = false, rebuild = false, backend = false;
static std::unique_ptr<vulkanscope::plugin::Runtime> plugins;
/** @brief 功能开关属于宿主配置，Surface 重建时保留用户选择。 */
static struct FeatureSetting {
    const char* id;
    const char* label;
    bool enabled;
} featureSettings[] = {
    {"frame-metrics", "Frame metrics", true},
    {"device-info", "Screen & device", true},
    {"gpu-info", "Vulkan GPU", true},
    {"performance-hud", "Foreground performance HUD", true}
};
static std::vector<std::pair<std::string, bool>> pluginChanges;
static std::string pluginError, rootReport;
static double nextRootRefresh = 0;

static void check(VkResult result) {
    if (result < 0) throw std::runtime_error("Vulkan error: " + std::to_string(result));
}
static void cleanup() {
    if (mainContext) ImGui::SetCurrentContext(mainContext);
    if (device) vkDeviceWaitIdle(device);
    /** @brief 所有插件回调和资源须在其借用的图形宿主之前销毁。 */
    plugins.reset();
    pluginChanges.clear();
    pluginError.clear();
    rootReport.clear();
    nextRootRefresh = 0;
    if (backend) { ImGui_ImplVulkan_Shutdown(); backend = false; }
    if (mainContext) { ImGui_ImplAndroid_Shutdown(); ImGui::DestroyContext(mainContext); mainContext = nullptr; }
    if (device) {
        ImGui_ImplVulkanH_DestroyWindow(instance, device, &window, nullptr);
        vkDestroyDevice(device, nullptr);
    } else if (window.Surface) vkDestroySurfaceKHR(instance, window.Surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE; device = VK_NULL_HANDLE; gpu = VK_NULL_HANDLE;
    window = ImGui_ImplVulkanH_Window();
    ready = false; rebuild = false;
}
static void initialize() {
    const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VulkanScope";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    check(vkCreateInstance(&info, nullptr, &instance));
    VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surface.window = app->window;
    check(vkCreateAndroidSurfaceKHR(instance, &surface, nullptr, &window.Surface));
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    for (auto candidate : devices) {
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
        for (uint32_t i = 0; i < n; ++i) {
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, window.Surface, &present));
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { gpu = candidate; family = i; break; }
        }
        if (gpu) break;
    }
    if (!gpu) throw std::runtime_error("No Vulkan graphics/present queue available");
    vkGetPhysicalDeviceProperties(gpu, &properties);
    vulkanscope::monitor::NativeSession().gpu({
        {"gpu_model", properties.deviceName},
        {"gpu_driver", std::to_string(properties.driverVersion) + " (raw)"},
        {"gpu_api", std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
            std::to_string(VK_VERSION_MINOR(properties.apiVersion)) + "." + std::to_string(VK_VERSION_PATCH(properties.apiVersion))}});
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
    const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q;
    d.enabledExtensionCount = 1; d.ppEnabledExtensionNames = &swapchain;
    check(vkCreateDevice(gpu, &d, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkFormat formats[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(gpu, window.Surface, formats, 2, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    window.PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, gpu, device, &window, family, nullptr,
        ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window), 2);
    IMGUI_CHECKVERSION();
    mainContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(mainContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { vulkanscope::CopyText(text); };
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        static std::string clipboard;
        clipboard = vulkanscope::PasteText(); return clipboard.c_str();
    };
    float scale = std::max(1.0f, AConfiguration_getDensity(app->config) / 160.0f);
    vulkanscope::LoadHudFont(18.0f * scale);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0; style.FrameRounding = 3;
    style.WindowPadding = ImVec2(14, 14); style.ItemSpacing = ImVec2(10, 10);
    style.ScaleAllSizes(scale);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.035f, 0.04f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.21f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.16f, 0.18f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.59f, 0.85f, 0.67f, 1);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.38f, 0.62f, 0.83f, 1);
    ImGui_ImplAndroid_Init(app->window);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_0;
    init.Instance = instance; init.PhysicalDevice = gpu; init.Device = device;
    init.QueueFamily = family; init.Queue = queue; init.DescriptorPoolSize = 16;
    init.RenderPass = window.RenderPass; init.MinImageCount = 2; init.ImageCount = window.ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT; init.CheckVkResultFn = check;
    if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan initialization failed");
    backend = true;
    plugins = std::make_unique<vulkanscope::plugin::Runtime>();
    auto reports = std::make_shared<vulkanscope::HostReports>([] { return vulkanscope::monitor::NativeSession().system(); });
    vulkanscope::RegisterBuiltinPlugins(*plugins, std::move(reports));
    auto settings = std::make_shared<vulkanscope::monitor::SettingsState>();
    auto hud = std::make_shared<vulkanscope::HostPerformanceHud>(
        [] { return vulkanscope::monitor::NativeSession().report(); },
        [settings] {
            if (ImGui::Button("Grant overlay permission")) vulkanscope::HudAction(env, app->activity->clazz, 0);
            if (ImGui::Button("Allow HUD notification")) vulkanscope::HudAction(env, app->activity->clazz, 4);
            vulkanscope::monitor::DrawSettings(*settings);
        },
        [] { vulkanscope::HudAction(env, app->activity->clazz, 1); },
        [] { vulkanscope::HudAction(env, app->activity->clazz, 2); },
        [] {
            using namespace vulkanscope::monitor;
            const auto config = NativeSession().config();
            const auto size = MeasureHud(config, ImGui::GetFontSize() * config.textSize / 18.0f, ImGui::GetContentRegionAvail().x, 10000);
            DrawClassicHud(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, config, NativeSession().hud());
            ImGui::Dummy(ImVec2(size.width, size.height));
        });
    vulkanscope::RegisterPerformanceHudPlugin(*plugins, std::move(hud));
    plugins->load("host.reports");
    for (const auto& feature : featureSettings) {
        if (feature.enabled) plugins->load(feature.id);
    }
    ready = true;
}

/** @brief 在新帧开始前提交启停请求，避免卸载仍在执行的渲染回调。 */
static void applyPluginChanges() {
    for (const auto& change : pluginChanges) {
        try {
            if (change.second) plugins->load(change.first);
            else plugins->unload(change.first);
            pluginError.clear();
        } catch (const std::exception& error) {
            pluginError = error.what();
            for (auto& feature : featureSettings) {
                if (change.first == feature.id) feature.enabled = plugins->isLoaded(feature.id);
            }
        }
    }
    pluginChanges.clear();
}

/** @brief UI 只记录意图；插件运行状态在下一帧边界改变。 */
static void pluginControls() {
    if (!ImGui::CollapsingHeader("Plugins")) return;
    for (auto& feature : featureSettings) {
        bool enabled = plugins->isLoaded(feature.id);
        if (ImGui::Checkbox(feature.label, &enabled)) {
            /**
             * @brief 用户禁用立即停止宿主会话，避免下帧前 Surface 销毁丢失停用意图。
             * @note 这里已退出插件事件分发；普通插件清理只释放界面缓存和监听。
             */
            if (!enabled && std::string(feature.id) == "performance-hud") vulkanscope::HudAction(env, app->activity->clazz, 2);
            pluginChanges.emplace_back(feature.id, enabled);
            feature.enabled = enabled;
        }
    }
    if (!pluginError.empty()) ImGui::TextWrapped("%s", pluginError.c_str());
}

static void panel() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    auto& io = ImGui::GetIO();
    /** @brief 使用 NativeActivity 的内容区域避开系统栏与屏幕开孔。 */
    ARect rect = app->contentRect;
    bool valid = rect.right > rect.left && rect.bottom > rect.top;
    ImGui::SetNextWindowPos(valid ? ImVec2(rect.left, rect.top) : ImVec2(0, 0));
    ImGui::SetNextWindowSize(valid ? ImVec2(rect.right - rect.left, rect.bottom - rect.top) : io.DisplaySize);
    ImGui::Begin("VulkanScope", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.7f, 1), "VULKAN SCOPE");
    plugins->emit(vulkanscope::RenderFrame{
        io.DeltaTime, io.Framerate, ImGui::GetTime(), window.Width, window.Height,
        properties.deviceName, properties.apiVersion, properties.driverVersion,
        properties.vendorID, properties.deviceID, window.ImageCount
    });
    pluginControls();
    if (rootReport.empty() || ImGui::GetTime() >= nextRootRefresh) {
        rootReport = vulkanscope::monitor::NativeSession().rootReport();
        nextRootRefresh = ImGui::GetTime() + 1.0;
    }
    if (ImGui::CollapsingHeader("Optional root diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Requires an existing su provider (e.g. Magisk). Grant access on your device. Reads kernel, CPU, memory, SELinux and thermal data.");
        if (ImGui::Button("Request root / refresh", ImVec2(-1, ImGui::GetFontSize()*2.5f))) {
            vulkanscope::monitor::NativeSession().requestRoot(); rootReport = vulkanscope::monitor::NativeSession().rootReport();
        }
        ImGui::TextWrapped("%s", rootReport.c_str());
    }
    ImGui::End();
    ImGui::Render();
}
static void render() {
    int width = ANativeWindow_getWidth(app->window), height = ANativeWindow_getHeight(app->window);
    if (width <= 0 || height <= 0) return;
    applyPluginChanges();
    if (rebuild || width != window.Width || height != window.Height) {
        check(vkDeviceWaitIdle(device));
        ImGui_ImplVulkan_SetMinImageCount(2);
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance, gpu, device, &window, family, nullptr, width, height, 2);
        window.FrameIndex = 0; rebuild = false;
    }
    panel();
    auto& sem = window.FrameSemaphores[window.SemaphoreIndex];
    VkResult result = vkAcquireNextImageKHR(device, window.Swapchain, UINT64_MAX, sem.ImageAcquiredSemaphore, VK_NULL_HANDLE, &window.FrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { rebuild = true; return; }
    if (result == VK_SUBOPTIMAL_KHR) rebuild = true; else check(result);
    auto& frame = window.Frames[window.FrameIndex];
    check(vkWaitForFences(device, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
    check(vkResetCommandPool(device, frame.CommandPool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.CommandBuffer, &begin));
    VkClearValue clear{{{0.035f, 0.045f, 0.065f, 1.0f}}};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = window.RenderPass; pass.framebuffer = frame.Framebuffer;
    pass.renderArea.extent = {static_cast<uint32_t>(window.Width), static_cast<uint32_t>(window.Height)};
    pass.clearValueCount = 1; pass.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.CommandBuffer);
    vkCmdEndRenderPass(frame.CommandBuffer);
    check(vkEndCommandBuffer(frame.CommandBuffer));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &sem.ImageAcquiredSemaphore; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &frame.CommandBuffer;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &sem.RenderCompleteSemaphore;
    check(vkResetFences(device, 1, &frame.Fence));
    check(vkQueueSubmit(queue, 1, &submit, frame.Fence));
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &sem.RenderCompleteSemaphore;
    present.swapchainCount = 1; present.pSwapchains = &window.Swapchain; present.pImageIndices = &window.FrameIndex;
    result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) rebuild = true; else check(result);
    window.SemaphoreIndex = (window.SemaphoreIndex + 1) % window.SemaphoreCount;
}
static void command(android_app*, int32_t cmd) {
    std::lock_guard<std::mutex> lock(vulkanscope::ImGuiMutex());
    if (mainContext) ImGui::SetCurrentContext(mainContext);
    if (cmd == APP_CMD_INIT_WINDOW && app->window && !ready) initialize();
    if (cmd == APP_CMD_TERM_WINDOW) cleanup();
    if (cmd == APP_CMD_GAINED_FOCUS) focused = true;
    if (cmd == APP_CMD_LOST_FOCUS) focused = false;
    if (cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED) rebuild = true;
}
void android_main(android_app* state) {
    app = state;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    vulkanscope::monitor::NativeSession().startSystemInfo();
    app->onAppCmd = command;
    app->onInputEvent = [](android_app*, AInputEvent* event) -> int32_t {
        std::lock_guard<std::mutex> lock(vulkanscope::ImGuiMutex());
        if (mainContext) ImGui::SetCurrentContext(mainContext);
        return ready ? ImGui_ImplAndroid_HandleInputEvent(event) : 0;
    };
    bool failed = false;
    while (!app->destroyRequested) {
        try {
            int events; android_poll_source* source;
            int timeout = (ready && focused && !failed) ? 0 : -1;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
                source->process(app, source);
            if (!app->destroyRequested && ready && focused && !failed) {
                std::lock_guard<std::mutex> lock(vulkanscope::ImGuiMutex());
                ImGui::SetCurrentContext(mainContext);
                render();
            }
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanScope", "%s", error.what());
            { std::lock_guard<std::mutex> lock(vulkanscope::ImGuiMutex()); cleanup(); } failed = true;
            vulkanscope::HudAction(env, app->activity->clazz, 3);
        }
    }
    { std::lock_guard<std::mutex> lock(vulkanscope::ImGuiMutex()); cleanup(); }
    vulkanscope::monitor::NativeSession().shutdownDiagnostics();
    vulkanscope::monitor::NativeSession().stopSystemInfo();
    app->activity->vm->DetachCurrentThread();
}
