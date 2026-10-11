#include "ui/i18n.h"
#include <algorithm>
#include <cctype>
#include <sstream>
namespace vulkanscope::ui {
namespace {
struct Entry {
    const char *source;
    const char *chinese;
};
constexpr Entry entries[] = {
    {"Foreground", "前台应用"},
    {"Foreground FPS", "前台帧率"},
    {"P95 frame time", "P95 帧时间"},
    {"System RAM", "系统内存"},
    {"Battery power (device)", "电池功耗（整机）"},
    {"Thermal state", "温控状态"},
    {"Engine render size", "引擎渲染尺寸"},
    {"Engine output size", "引擎输出尺寸"},
    {"Device", "设备"},
    {"CPU cores", "CPU 核心数"},
    {"RAM available / total", "内存可用 / 总量"},
    {"Display", "显示尺寸"},
    {"Density", "显示密度"},
    {"Physical DPI (reported)", "物理 DPI（报告值）"},
    {"Refresh", "刷新率"},
    {"Rotation", "旋转角度"},

    {"Home", "主页"},
    {"Plugins", "插件列表"},
    {"Language", "语言"},
    {"Follow system", "跟随系统"},
    {"English", "English"},
    {"Simplified Chinese", "简体中文"},
    {"Overview", "概览"},
    {"Monitor your device and foreground applications.", "查看设备信息与前台应用性能。"},
    {"Manage the features shown on your home page.", "管理主页显示的功能。"},
    {"Enabled", "已启用"},
    {"Disabled", "已停用"},
    {"Frame metrics", "帧率与帧时间"},
    {"Screen & device", "屏幕与设备"},
    {"Vulkan GPU", "Vulkan 图形设备"},
    {"Foreground performance HUD", "前台应用性能悬浮窗"},
    {"Frame metrics description", "显示本应用的帧率与帧时间曲线。"},
    {"Screen & device description", "查看屏幕、设备及系统信息。"},
    {"Vulkan GPU description", "查看 Vulkan 图形设备、驱动和接口信息。"},
    {"Foreground performance HUD description", "以经典 HUD 显示前台应用性能，支持悬浮窗与原生配置。"},
    {"Optional root diagnostics", "Root 诊断（可选）"},
    {"Request root / refresh", "请求 Root / 刷新"},
    {"Requires an existing su provider (e.g. Magisk). Grant access on your device. Reads kernel, CPU, "
     "memory, SELinux and thermal data.",
     "需要设备已安装 su 提供者（例如 Magisk），并在设备上授权。读取内核、CPU、内存、SELinux 和温控信息。"},
    {"Grant overlay permission", "授予悬浮窗权限"},
    {"Allow HUD notification", "允许悬浮窗通知"},
    {"Configure", "配置"},
    {"Start HUD", "启动悬浮窗"},
    {"Stop HUD", "停止悬浮窗"},
    {"Sample details", "采样详情"},
    {"Start a HUD session, then switch to your game. Stop it here or from its notification.",
     "启动悬浮窗后切换到游戏。可在此处或通知中停止。"},
    {"App render rate / FIFO VSync. This is not a global game FPS monitor.",
     "本应用渲染帧率 / FIFO 垂直同步，不代表前台游戏帧率。"},
    {"Frame time (ms)", "帧时间（毫秒）"},
    {"Render surface: %d x %d px", "渲染表面：%d × %d 像素"},
    {"Driver (raw): 0x%08x", "驱动（原始值）：0x%08x"},
    {"Vendor / device: %04x / %04x", "厂商 / 设备：%04x / %04x"},
    {"Swapchain: %u images / FIFO", "交换链：%u 张图像 / FIFO"},
    {"Classic Metal HUD", "经典 Metal HUD"},
    {"Frame history graph", "帧历史曲线"},
    {"Additional sensor rows", "附加传感器行"},
    {"Enable read-only root sampling", "启用只读 Root 采样"},
    {"Root is optional. FPS and engine inspection need an existing su provider. Enabling this option "
     "authorizes read-only queries while the HUD runs. To retry a denied authorization, disable it for one "
     "sample, then re-enable it.",
     "Root 为可选项。FPS 与引擎检测需要设备提供 "
     "su。开启后允许悬浮窗运行时执行只读查询。授权被拒绝后，请关闭至少一个采样周期，再重新开启以重试。"},
    {"Sample interval", "采样间隔"},
    {"Text size", "字号"},
    {"Background opacity", "背景不透明度"},
    {"Corner", "位置"},
    {"Top left", "左上角"},
    {"Top right", "右上角"},
    {"Bottom left", "左下角"},
    {"Bottom right", "右下角"},
    {"Changes are saved in native storage. The overlay is touch-through; configure it here.",
     "修改会自动保存。悬浮窗支持触摸穿透，请在此处配置。"},
    {"Fields and display order", "字段与显示顺序"},
    {"Core rows keep their classic positions. Disabled values show --. Enable Additional sensor rows to show "
     "other fields in the order below.",
     "核心行保持固定位置，停用值显示 --。开启“附加传感器行”后，其他字段按下方顺序显示。"},
    {"Up", "上移"},
    {"Down", "下移"},
    {"Advanced sensors and Surface selection", "高级传感器与 Surface 选择"},
    {"Exact foreground Surface", "前台 Surface 精确名称"},
    {"Edit Surface name", "编辑 Surface 名称"},
    {"Paste Surface", "粘贴 Surface"},
    {"CPU power sensor (/sys/...)", "CPU 功耗传感器（/sys/...）"},
    {"Edit sensor path", "编辑传感器路径"},
    {"Paste path", "粘贴路径"},
    {"Sensor to watts multiplier", "传感器读数转瓦特系数"},
    {"Watts", "瓦特"},
    {"Milliwatts", "毫瓦"},
    {"Microwatts", "微瓦"},
    {"Battery power is a whole-device estimate, never a CPU power substitute. Leave the Surface override "
     "empty for automatic selection.",
     "电池功耗是整机估算值，不能替代 CPU 功耗。Surface 名称留空时自动选择。"},
    {"Game fields", "游戏字段"},
    {"Hardware fields", "硬件字段"},
    {"Restore defaults", "恢复默认"},
    {"Latest diagnostics", "最新诊断"},
    {"Copy latest diagnostics", "复制最新诊断"},
    {"Last game diagnostics", "上次游戏诊断"},
    {"Copy game diagnostics", "复制游戏诊断"},
    {"No sample yet", "暂无样本"},
    {"No game sample yet", "暂无游戏样本"},
    {"Touch keyboard (appends at end)", "触摸键盘（追加到末尾）"},
    {"Space", "空格"},
    {"Shift", "切换大小写"},
    {"Backspace", "退格"},
    {"Clear", "清空"},
    {"Done", "完成"},
    {"Foreground app", "前台应用"},
    {"FPS", "FPS"},
    {"Frame time", "帧时间"},
    {"Frame P95", "帧时间 P95"},
    {"FPS source", "FPS 来源"},
    {"Surface", "Surface"},
    {"GPU model", "GPU 型号"},
    {"Process memory (RSS)", "进程内存（RSS）"},
    {"GPU driver", "GPU 驱动"},
    {"GPU API", "GPU API"},
    {"CPU clock", "CPU 时钟"},
    {"CPU load", "CPU 使用率"},
    {"CPU power", "CPU 功耗"},
    {"GPU clock", "GPU 时钟"},
    {"GPU load", "GPU 使用率"},
    {"Memory", "内存"},
    {"Battery power", "电池功耗"},
    {"Battery temperature", "电池温度"},
    {"Thermal status", "温控状态"},
    {"Display resolution", "显示分辨率"},
    {"Display refresh", "屏幕刷新率"},
    {"Engine render resolution", "引擎渲染分辨率"},
    {"Engine output resolution", "引擎输出分辨率"},
    {"Engine source", "引擎数据来源"},
    {"System font", "系统字体"},
    {"System font fallback", "系统字体不可用，使用回退字体"},
    {"Raw diagnostics", "原始诊断数据"},
    {"Capture stopped", "采集已停止"},
    {"Waiting for a fresh sample", "等待新样本"},
    {"Start HUD to capture a foreground application", "启动悬浮窗以采集前台应用"},
};
}
Language ResolveLanguage(std::string_view preference, std::string_view systemLocale) {
    std::string tag(preference == "system" ? systemLocale : preference);
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tag == "zh" || tag.rfind("zh-", 0) == 0 || tag.rfind("zh_", 0) == 0 ? Language::Chinese
                                                                               : Language::English;
}
const char *Text(Language language, const char *source) {
    if (language == Language::Chinese)
        for (const auto &entry : entries)
            if (std::string_view(entry.source) == source)
                return entry.chinese;
    return source;
}
std::string Label(Language language, const char *source) {
    return std::string(Text(language, source)) + "###" + source;
}
std::string TranslateReport(Language language, const std::string &report) {
    std::istringstream input(report);
    std::string line, result;
    while (std::getline(input, line)) {
        if (!result.empty())
            result += '\n';
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            result += Text(language, line.c_str());
        else
            result += std::string(Text(language, line.substr(0, colon).c_str())) + line.substr(colon);
    }
    return result;
}
std::string TranslationCorpus() {
    std::string text;
    for (const auto &entry : entries) {
        text += entry.source;
        text += entry.chinese;
    }
    return text;
}
}
