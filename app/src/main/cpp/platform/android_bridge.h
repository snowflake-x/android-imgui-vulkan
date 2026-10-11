#pragma once
#include "monitor/session.h"
#include <jni.h>
namespace vulkanscope {
/** @brief 将 APK 内的等宽字体加载到当前 ImGui atlas；必须持有 ImGuiMutex。 */
void LoadHudFont(float pixels);
/** @brief C++ 直接调用 Android 公共 API，JNI 局部引用和线程附着限于一次快照。 */
monitor::PlatformSample ReadPlatformSample();
/** @brief 通过 Android 公共剪贴板 API 复制原生诊断文本。 */
void CopyText(const std::string &text);
std::string PasteText();
/** @brief 将用户操作异步交给主线程的生命周期桥接，不持有 JNIEnv。 */
void HudAction(JNIEnv *env, jobject activity, int action);
}
