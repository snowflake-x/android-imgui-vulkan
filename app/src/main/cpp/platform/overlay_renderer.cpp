#include "platform/overlay_renderer.h"
#include "imgui_impl_vulkan.h"
#include "monitor/model.h"
#include "platform/android_bridge.h"
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace vulkanscope {
std::mutex &ImGuiMutex() {
    static std::mutex mutex;
    return mutex;
}
static void check(VkResult result) {
    if (result < 0)
        throw std::runtime_error("Vulkan error: " + std::to_string(result));
}
void OverlayRenderer::initialize(ANativeWindow *surfaceWindow, float scale) {
    nativeWindow = surfaceWindow;
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VulkanScope";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    check(vkCreateInstance(&info, nullptr, &instance));
    VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surface.window = nativeWindow;
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
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                gpu = candidate;
                family = i;
                break;
            }
        }
        if (gpu)
            break;
    }
    if (!gpu)
        throw std::runtime_error("No Vulkan graphics/present queue available");
    vkGetPhysicalDeviceProperties(gpu, &properties);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    const char *swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    d.queueCreateInfoCount = 1;
    d.pQueueCreateInfos = &q;
    d.enabledExtensionCount = 1;
    d.ppEnabledExtensionNames = &swapchain;
    check(vkCreateDevice(gpu, &d, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(gpu, window.Surface, formats, 2,
                                                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    window.PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, window.Surface, &capabilities));
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
        window.CompositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        window.CompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    else
        throw std::runtime_error("Transparent Vulkan composition is unsupported");
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance, gpu, device, &window, family, nullptr,
                                           ANativeWindow_getWidth(nativeWindow),
                                           ANativeWindow_getHeight(nativeWindow), 2);
    IMGUI_CHECKVERSION();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    LoadHudFont(24.0f * scale);
    ImGui::StyleColorsDark();
    auto &style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 6;
    style.WindowPadding = ImVec2(14, 14);
    style.ItemSpacing = ImVec2(10, 10);
    style.ScaleAllSizes(scale);
    style.Colors[ImGuiCol_Button] = ImVec4(0.08f, 0.36f, 0.42f, 1);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.2f, 0.9f, 0.7f, 1);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_0;
    init.Instance = instance;
    init.PhysicalDevice = gpu;
    init.Device = device;
    init.QueueFamily = family;
    init.Queue = queue;
    init.DescriptorPoolSize = 16;
    init.RenderPass = window.RenderPass;
    init.MinImageCount = 2;
    init.ImageCount = window.ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = check;
    if (!ImGui_ImplVulkan_Init(&init))
        throw std::runtime_error("ImGui Vulkan initialization failed");
    backend = true;
    ready = true;
}
void OverlayRenderer::render(const std::function<void()> &draw) {
    ImGui::SetCurrentContext(context);
    int width = ANativeWindow_getWidth(nativeWindow), height = ANativeWindow_getHeight(nativeWindow);
    if (width <= 0 || height <= 0)
        return;
    if (rebuild || width != window.Width || height != window.Height) {
        check(vkDeviceWaitIdle(device));
        ImGui_ImplVulkan_SetMinImageCount(2);
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance, gpu, device, &window, family, nullptr, width, height,
                                               2);
        window.FrameIndex = 0;
        rebuild = false;
    }
    ImGui::SetCurrentContext(context);
    ImGui_ImplVulkan_NewFrame();
    auto &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    const double now = monitor::MonotonicNs() / 1e9;
    io.DeltaTime = lastFrame > 0 ? static_cast<float>(std::max(0.001, now - lastFrame)) : 0.1f;
    lastFrame = now;
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    auto &sem = window.FrameSemaphores[window.SemaphoreIndex];
    VkResult result = vkAcquireNextImageKHR(device, window.Swapchain, 100000000ULL,
                                            sem.ImageAcquiredSemaphore, VK_NULL_HANDLE, &window.FrameIndex);
    if (result == VK_TIMEOUT || result == VK_NOT_READY)
        return;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuild = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        rebuild = true;
    else
        check(result);
    auto &frame = window.Frames[window.FrameIndex];
    check(vkWaitForFences(device, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
    check(vkResetCommandPool(device, frame.CommandPool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.CommandBuffer, &begin));
    VkClearValue clear{{{0, 0, 0, 0}}};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = window.RenderPass;
    pass.framebuffer = frame.Framebuffer;
    pass.renderArea.extent = {static_cast<uint32_t>(window.Width), static_cast<uint32_t>(window.Height)};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(frame.CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.CommandBuffer);
    vkCmdEndRenderPass(frame.CommandBuffer);
    check(vkEndCommandBuffer(frame.CommandBuffer));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &sem.ImageAcquiredSemaphore;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &sem.RenderCompleteSemaphore;
    check(vkResetFences(device, 1, &frame.Fence));
    check(vkQueueSubmit(queue, 1, &submit, frame.Fence));
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &sem.RenderCompleteSemaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &window.Swapchain;
    present.pImageIndices = &window.FrameIndex;
    result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        rebuild = true;
    else
        check(result);
    window.SemaphoreIndex = (window.SemaphoreIndex + 1) % window.SemaphoreCount;
}
void OverlayRenderer::cleanup() {
    if (context)
        ImGui::SetCurrentContext(context);
    if (device)
        vkDeviceWaitIdle(device);
    if (backend) {
        ImGui_ImplVulkan_Shutdown();
        backend = false;
    }
    if (context) {
        ImGui::DestroyContext(context);
        context = nullptr;
    }
    if (device) {
        ImGui_ImplVulkanH_DestroyWindow(instance, device, &window, nullptr);
        vkDestroyDevice(device, nullptr);
    } else if (window.Surface)
        vkDestroySurfaceKHR(instance, window.Surface, nullptr);
    if (instance)
        vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    gpu = VK_NULL_HANDLE;
    window = ImGui_ImplVulkanH_Window();
    ready = false;
    rebuild = false;
}
}
