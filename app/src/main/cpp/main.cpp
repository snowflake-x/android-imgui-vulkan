#include <android_native_app_glue.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_vulkan.h"

static android_app* app;
static JNIEnv* env;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static ImGui_ImplVulkanH_Window window;
static VkPhysicalDeviceProperties properties{};
static bool ready = false, focused = false, rebuild = false, backend = false;
static std::array<float, 180> history{};
static int historyIndex = 0;
static std::string systemReport, rootReport;
static double nextRefresh = 0;

static void check(VkResult result) {
    if (result < 0) throw std::runtime_error("Vulkan error: " + std::to_string(result));
}
static std::string javaText(const char* name) {
    jclass cls = env->GetObjectClass(app->activity->clazz);
    jmethodID method = env->GetMethodID(cls, name, "()Ljava/lang/String;");
    auto value = static_cast<jstring>(env->CallObjectMethod(app->activity->clazz, method));
    std::string result;
    if (env->ExceptionCheck()) { env->ExceptionClear(); result = "Information unavailable"; }
    else if (value) {
        const char* text = env->GetStringUTFChars(value, nullptr);
        if (text) { result = text; env->ReleaseStringUTFChars(value, text); }
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(cls);
    return result;
}
static void javaCall(const char* name, const char* message = nullptr) {
    jclass cls = env->GetObjectClass(app->activity->clazz);
    jmethodID method = env->GetMethodID(cls, name, message ? "(Ljava/lang/String;)V" : "()V");
    if (message) {
        jstring text = env->NewStringUTF(message);
        env->CallVoidMethod(app->activity->clazz, method, text);
        env->DeleteLocalRef(text);
    } else env->CallVoidMethod(app->activity->clazz, method);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
}
static void cleanup() {
    if (device) vkDeviceWaitIdle(device);
    if (backend) { ImGui_ImplVulkan_Shutdown(); backend = false; }
    if (ImGui::GetCurrentContext()) { ImGui_ImplAndroid_Shutdown(); ImGui::DestroyContext(); }
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
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = std::max(1.0f, AConfiguration_getDensity(app->config) / 160.0f);
    ImFontConfig font; font.SizePixels = 18.0f * scale;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0; style.FrameRounding = 6;
    style.WindowPadding = ImVec2(14, 14); style.ItemSpacing = ImVec2(10, 10);
    style.ScaleAllSizes(scale);
    style.Colors[ImGuiCol_Button] = ImVec4(0.08f, 0.36f, 0.42f, 1);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.2f, 0.9f, 0.7f, 1);
    ImGui_ImplAndroid_Init(app->window);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_0;
    init.Instance = instance; init.PhysicalDevice = gpu; init.Device = device;
    init.QueueFamily = family; init.Queue = queue; init.DescriptorPoolSize = 16;
    init.RenderPass = window.RenderPass; init.MinImageCount = 2; init.ImageCount = window.ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT; init.CheckVkResultFn = check;
    if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan initialization failed");
    backend = true; ready = true;
    history.fill(0); historyIndex = 0; nextRefresh = 0;
}
static void panel() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    auto& io = ImGui::GetIO();
    history[historyIndex] = 1000.0f * io.DeltaTime;
    historyIndex = (historyIndex + 1) % history.size();
    // Respect status/navigation bars and cutouts supplied by NativeActivity.
    ARect rect = app->contentRect;
    bool valid = rect.right > rect.left && rect.bottom > rect.top;
    ImGui::SetNextWindowPos(valid ? ImVec2(rect.left, rect.top) : ImVec2(0, 0));
    ImGui::SetNextWindowSize(valid ? ImVec2(rect.right - rect.left, rect.bottom - rect.top) : io.DisplaySize);
    ImGui::Begin("VulkanScope", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.7f, 1), "VULKAN SCOPE");
    ImGui::Text("%.1f FPS  |  %.2f ms", io.Framerate, io.Framerate > 0 ? 1000.0f/io.Framerate : 0);
    ImGui::TextWrapped("App render rate / FIFO VSync. This is not a global game FPS monitor.");
    ImGui::PlotLines("##frame", history.data(), history.size(), historyIndex, "Frame time (ms)", 0, 50, ImVec2(-1, ImGui::GetFontSize()*4));
    if (systemReport.empty() || ImGui::GetTime() >= nextRefresh) {
        systemReport = javaText("getSystemReport"); rootReport = javaText("getRootReport");
        nextRefresh = ImGui::GetTime() + 1.0;
    }
    if (ImGui::CollapsingHeader("Screen & device", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", systemReport.c_str());
        ImGui::Text("Render surface: %d x %d px", window.Width, window.Height);
    }
    if (ImGui::CollapsingHeader("Vulkan GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", properties.deviceName);
        ImGui::Text("API: %u.%u.%u", VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        ImGui::Text("Driver (raw): 0x%08x", properties.driverVersion);
        ImGui::Text("Vendor / device: %04x / %04x", properties.vendorID, properties.deviceID);
        ImGui::Text("Swapchain: %u images / FIFO", window.ImageCount);
    }
    if (ImGui::CollapsingHeader("Optional root diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Requires an existing su provider (e.g. Magisk). Grant access on your device. Reads kernel, CPU, memory, SELinux and thermal data.");
        if (ImGui::Button("Request root / refresh", ImVec2(-1, ImGui::GetFontSize()*2.5f))) {
            javaCall("requestRootInfo"); rootReport = javaText("getRootReport");
        }
        ImGui::TextWrapped("%s", rootReport.c_str());
    }
    ImGui::End();
    ImGui::Render();
}
static void render() {
    int width = ANativeWindow_getWidth(app->window), height = ANativeWindow_getHeight(app->window);
    if (width <= 0 || height <= 0) return;
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
    if (cmd == APP_CMD_INIT_WINDOW && app->window && !ready) initialize();
    if (cmd == APP_CMD_TERM_WINDOW) cleanup();
    if (cmd == APP_CMD_GAINED_FOCUS) focused = true;
    if (cmd == APP_CMD_LOST_FOCUS) focused = false;
    if (cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED) rebuild = true;
}
void android_main(android_app* state) {
    app = state;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    app->onAppCmd = command;
    app->onInputEvent = [](android_app*, AInputEvent* event) -> int32_t {
        return ready ? ImGui_ImplAndroid_HandleInputEvent(event) : 0;
    };
    bool failed = false;
    while (!app->destroyRequested) {
        try {
            int events; android_poll_source* source;
            int timeout = (ready && focused && !failed) ? 0 : -1;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
                source->process(app, source);
            if (!app->destroyRequested && ready && focused && !failed) render();
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_ERROR, "VulkanScope", "%s", error.what());
            cleanup(); failed = true;
            javaCall("showError", error.what());
        }
    }
    cleanup();
    app->activity->vm->DetachCurrentThread();
}
