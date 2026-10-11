#pragma once

#include "module.h"
#include <vector>

namespace vulkanscope {
/** @brief 按注册顺序拥有并绘制模块；所有操作仅允许在渲染线程执行。 */
class Dashboard {
public:
    /** @brief 重新创建内置模块，清空上一个 Surface 的缓存。 */
    void reset();
    /** @brief 释放所有模块及其缓存；可重复调用。 */
    void clear();
    /** @brief 使用当前帧上下文依次绘制已注册的模块。 @param context 当前帧信息。 */
    void draw(const ModuleContext& context);
private:
    std::vector<std::unique_ptr<Module>> modules_; ///< 按 UI 显示顺序独占模块实例。
};
}
