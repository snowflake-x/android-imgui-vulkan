/** @brief 仅供宿主测试解析真实编译器生成的动态函数符号；不进入 APK。 */
extern "C" int eglSwapBuffers() { return 42; }
/** @brief 同一测试 ELF 提供 Vulkan 符号以覆盖混合调用的证据解析。 */
extern "C" int vkQueuePresentKHR() { return 7; }
