cmake_minimum_required(VERSION 3.22.1)
project(vulkanscope LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(vulkanscope_plugins STATIC plugin/runtime.cpp)
set_target_properties(vulkanscope_plugins PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vulkanscope_plugins PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(vulkanscope SHARED main.cpp features/builtin_plugins.cpp features/performance_hud.cpp
    monitor/model.cpp monitor/process.cpp monitor/collector.cpp monitor/session.cpp monitor/settings_ui.cpp
    platform/android_bridge.cpp platform/overlay_renderer.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_android.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)
target_include_directories(vulkanscope PRIVATE ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(vulkanscope PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(vulkanscope PRIVATE vulkanscope_plugins native_app_glue android log vulkan)
target_link_options(vulkanscope PRIVATE "-uANativeActivity_onCreate" "-Wl,-z,max-page-size=16384")
