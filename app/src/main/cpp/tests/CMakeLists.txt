cmake_minimum_required(VERSION 3.22.1)
project(vulkanscope_plugin_tests LANGUAGES CXX)
add_compile_definitions(IMGUI_USE_WCHAR32)

enable_testing()
find_package(Threads REQUIRED)

option(VULKANSCOPE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(plugin_runtime_test runtime_test.cpp ../plugin/runtime.cpp)
target_include_directories(plugin_runtime_test PRIVATE ..)
target_link_libraries(plugin_runtime_test PRIVATE Threads::Threads)

add_executable(builtin_plugins_test builtin_plugins_test.cpp
    ../features/builtin_plugins.cpp ../ui/i18n.cpp ../ui/theme.cpp
    ../plugin/runtime.cpp
    ../vendor/imgui/imgui.cpp
    ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp
    ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(builtin_plugins_test PRIVATE .. ../vendor/imgui)
target_link_libraries(builtin_plugins_test PRIVATE Threads::Threads)

add_executable(performance_hud_test performance_hud_test.cpp
    ../features/performance_hud.cpp ../ui/i18n.cpp ../ui/theme.cpp
    ../plugin/runtime.cpp
    ../vendor/imgui/imgui.cpp
    ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp
    ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(performance_hud_test PRIVATE .. ../vendor/imgui)
target_link_libraries(performance_hud_test PRIVATE Threads::Threads)

add_executable(native_monitor_test native_monitor_test.cpp ../monitor/model.cpp ../monitor/process.cpp
    ../monitor/collector.cpp ../monitor/graphics_trace.cpp ../monitor/session.cpp ../monitor/diagnostics.cpp ../monitor/hud.cpp)
target_include_directories(native_monitor_test PRIVATE ..)
target_link_libraries(native_monitor_test PRIVATE Threads::Threads)

add_executable(native_settings_test native_settings_test.cpp ../ui/i18n.cpp ../ui/theme.cpp ../monitor/settings_ui.cpp ../monitor/hud_view.cpp
    ../monitor/model.cpp ../monitor/process.cpp ../monitor/collector.cpp ../monitor/graphics_trace.cpp ../monitor/session.cpp ../monitor/diagnostics.cpp ../monitor/hud.cpp
    ../vendor/imgui/imgui.cpp ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(native_settings_test PRIVATE .. ../vendor/imgui)
target_link_libraries(native_settings_test PRIVATE Threads::Threads)

add_executable(classic_hud_test classic_hud_test.cpp ../monitor/hud.cpp ../monitor/hud_view.cpp ../monitor/model.cpp
    ../vendor/imgui/imgui.cpp ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(classic_hud_test PRIVATE .. ../vendor/imgui)
target_compile_definitions(classic_hud_test PRIVATE HUD_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/fonts/DejaVuSansMono.ttf")

find_file(TEST_LATIN_FONT DejaVuSans.ttf PATHS /usr/share/fonts/truetype/dejavu REQUIRED)
find_file(TEST_CJK_FONT NotoSansCJK-Regular.ttc PATHS /usr/share/fonts/opentype/noto REQUIRED)
add_executable(navigation_i18n_test navigation_i18n_test.cpp
    ../plugin/runtime.cpp ../features/builtin_plugins.cpp ../features/performance_hud.cpp ../monitor/hud_view.cpp ../monitor/hud.cpp ../ui/navigation.cpp ../ui/i18n.cpp ../ui/theme.cpp
    ../ui/font_catalog.cpp ../platform/system_fonts.cpp ../vendor/tinyxml2/tinyxml2.cpp
    ../monitor/collector.cpp ../monitor/graphics_trace.cpp ../monitor/model.cpp ../monitor/process.cpp
    ../vendor/imgui/imgui.cpp ../vendor/imgui/imgui_draw.cpp ../vendor/imgui/imgui_tables.cpp ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(navigation_i18n_test PRIVATE .. ../vendor/imgui)
target_link_libraries(navigation_i18n_test PRIVATE Threads::Threads)
target_compile_definitions(navigation_i18n_test PRIVATE TEST_LATIN_FONT="${TEST_LATIN_FONT}" TEST_CJK_FONT="${TEST_CJK_FONT}")

add_library(graphics_trace_fixture SHARED graphics_trace_fixture.cpp)
set_target_properties(graphics_trace_fixture PROPERTIES OUTPUT_NAME EGL)
add_executable(graphics_trace_test graphics_trace_test.cpp ../monitor/graphics_trace.cpp ../monitor/model.cpp ../monitor/process.cpp)
target_include_directories(graphics_trace_test PRIVATE ..)
target_compile_definitions(graphics_trace_test PRIVATE TRACE_FIXTURE="$<TARGET_FILE:graphics_trace_fixture>")
add_dependencies(graphics_trace_test graphics_trace_fixture)

add_executable(navigation_touch_test navigation_touch_test.cpp ../ui/navigation.cpp ../ui/i18n.cpp ../ui/theme.cpp
    ../vendor/imgui/imgui.cpp ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(navigation_touch_test PRIVATE .. ../vendor/imgui)

add_executable(navigation_inertia_test navigation_inertia_test.cpp ../ui/navigation.cpp ../ui/i18n.cpp ../ui/theme.cpp
    ../vendor/imgui/imgui.cpp ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(navigation_inertia_test PRIVATE .. ../vendor/imgui)

add_executable(surface_selection_test surface_selection_test.cpp ../monitor/model.cpp)
target_include_directories(surface_selection_test PRIVATE ..)

add_executable(surface_collection_test surface_collection_test.cpp ../monitor/collector.cpp
    ../monitor/graphics_trace.cpp ../monitor/model.cpp ../monitor/process.cpp)
target_include_directories(surface_collection_test PRIVATE ..)

add_executable(diagnostic_history_test diagnostic_history_test.cpp ../monitor/diagnostics.cpp)
target_include_directories(diagnostic_history_test PRIVATE ..)

foreach(test_target navigation_touch_test navigation_inertia_test graphics_trace_test navigation_i18n_test classic_hud_test plugin_runtime_test builtin_plugins_test performance_hud_test native_monitor_test native_settings_test surface_selection_test surface_collection_test diagnostic_history_test)
    target_compile_features(${test_target} PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(VULKANSCOPE_ENABLE_SANITIZERS)
            target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
