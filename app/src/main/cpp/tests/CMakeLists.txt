cmake_minimum_required(VERSION 3.22.1)
project(vulkanscope_plugin_tests LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)

option(VULKANSCOPE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(plugin_runtime_test runtime_test.cpp ../plugin/runtime.cpp)
target_include_directories(plugin_runtime_test PRIVATE ..)
target_link_libraries(plugin_runtime_test PRIVATE Threads::Threads)

add_executable(builtin_plugins_test builtin_plugins_test.cpp
    ../features/builtin_plugins.cpp
    ../plugin/runtime.cpp
    ../vendor/imgui/imgui.cpp
    ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp
    ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(builtin_plugins_test PRIVATE .. ../vendor/imgui)
target_link_libraries(builtin_plugins_test PRIVATE Threads::Threads)

add_executable(performance_hud_test performance_hud_test.cpp
    ../features/performance_hud.cpp
    ../plugin/runtime.cpp
    ../vendor/imgui/imgui.cpp
    ../vendor/imgui/imgui_draw.cpp
    ../vendor/imgui/imgui_tables.cpp
    ../vendor/imgui/imgui_widgets.cpp)
target_include_directories(performance_hud_test PRIVATE .. ../vendor/imgui)
target_link_libraries(performance_hud_test PRIVATE Threads::Threads)

foreach(test_target plugin_runtime_test builtin_plugins_test performance_hud_test)
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
