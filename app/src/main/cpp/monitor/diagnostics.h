#pragma once
#include "monitor/model.h"
#include <string>
#include <vector>

namespace vulkanscope::monitor {
struct DiagnosticEntry {
    std::string package;
    int user = -1;
    std::string text;
};
/** @brief 按 Android 用户与包名保留最近应用诊断，切换桌面不会覆盖游戏记录。 */
class DiagnosticHistory {
  public:
    static constexpr std::size_t Capacity = 8;
    static constexpr std::size_t MaxTextBytes = 64 * 1024;
    /** @brief 更新有效前台应用的有界记录；缺失身份和监控自身不写入历史。 */
    void record(const Values &values, const std::string &text);
    /** @brief 最近采样的应用排在前面；调用方负责与 record 互斥。 */
    const std::vector<DiagnosticEntry> &entries() const { return entries_; }

  private:
    std::vector<DiagnosticEntry> entries_;
};
}
