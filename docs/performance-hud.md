# 原生性能悬浮窗与配置

`performance-hud` 的悬浮窗、配置页和诊断页均由 **C++ + Dear ImGui + Vulkan** 绘制。C++ 负责配置持久化、采集调度、root 子进程、指标解析和系统信息读取。应用不再包含 Kotlin 插件或 Kotlin 业务代码。

Android 仅保留 `MainActivity.java`、`HudService.java` 两个生命周期桥接：系统权限入口、前台通知、WindowManager 的 SurfaceView 容器以及 Surface 回调。没有 TextView 悬浮窗、Android 配置表单、Java 指标采集或 Java 配置逻辑。NDK 未提供的电池、显示、温控及剪贴板公共 API 由 C++ 直接通过 JNI 调用；JNIEnv 不跨线程保存。

## 使用

1. 在 **Foreground performance HUD → Configure** 展开 ImGui 配置页，点击 **Grant overlay permission** 到系统页面授权。Android 13+ 可点 **Allow HUD notification** 授权通知。
2. 选择字段、顺序、字号、背景透明度、四角位置和采样间隔。设置立即保存；可使用 Game/Hardware 字段预设。
3. 如需前台 FPS 或引擎信息，显式勾选 **Enable read-only root sampling**。默认关闭；需要设备已安装的 `su` 提供者。
4. 点击 **Start HUD** 后切换游戏。首次有效采样只建立基线，下一次开始统计。拒绝授权后不会循环弹窗，停止再启动可重试。
5. 点击通知的 **Stop**、主面板的 **Stop HUD**，或关闭性能插件以停止。关闭主界面不会结束用户启动的悬浮窗会话。

悬浮窗不接收触摸，Android 窗口 alpha 限制为 0.8。ImGui 背景透明度独立可调；渲染使用支持预乘透明度或继承透明度的 Vulkan Surface。不支持透明合成时显示错误并结束会话，不生成假透明的黑色窗口。默认字体使用英文标签；长字段可在 ImGui 诊断页查看完整内容。

高级设置提供 Surface 精确名称、CPU 功耗节点与瓦特换算系数。手机可使用 ImGui 触摸键盘或剪贴板粘贴，不依赖 EditText。**Latest diagnostics / Last game diagnostics** 支持复制完整诊断；返回本应用后保留最后的游戏样本及其采集时间。

配置保存在应用私有 `files/native-hud.conf`，通过临时文件和同目录 rename 发布。首次升级由 C++ 导入旧 `performance_hud` SharedPreferences 的字段、顺序、root 选择和显示设置；之后只使用原生配置。损坏或非法配置采用受限默认值。

## 指标来源

| 指标 | C++ 采集来源和含义 |
| --- | --- |
| 前台应用 | root 下的 Activity/Window 焦点记录；按包名和 Android 用户隔离 |
| FPS、帧时间、P95 | `dumpsys SurfaceFlinger --latency` 的第二列 actual-present 时间戳 |
| FPS 来源、Surface | 实际选中的游戏层；不使用监控器自身的渲染 FPS 或屏幕刷新率补值 |
| GPU 型号、驱动、API | Vulkan 物理设备查询；原始驱动版本和 Vulkan API，不代表游戏使用的独立驱动/API |
| CPU 时钟、使用率 | cpufreq policy/核心节点、`/proc/stat` 差分；排除重复包含的 guest 计数 |
| CPU 功耗 | 用户确认单位的 `/sys/` CPU 功率传感器；原始值乘以 W 系数 |
| GPU 时钟、使用率 | Adreno/devfreq 已知 Hz 节点和 Adreno busy/total |
| 内存 | `/proc/meminfo` 的 MemTotal、MemAvailable；整机已用/总内存 |
| 电池功率、温度 | C++ 调用 Android 电池 API；电流×电压是整机电池功率估计，不是 CPU 功耗 |
| 温控 | C++ 调用 Android 10+ 温控压力 API，不是 CPU 核心温度 |
| 显示 | C++ 查询物理显示模式、刷新率、密度、旋转；系统属性读取型号和 Android 版本 |
| 引擎内部/输出尺寸 | 校验后的引擎自报数据；缺少兼容读取器时明确显示 N/A |

CPU 传感器只接受 `/sys/` 路径，不接受 `..` 或 shell 语法。单位系数：µW → `0.000001`，mW → `0.001`，W → `1`。电池节点、频率或温度不能替代 CPU 功率。不可访问或不支持的指标显示 N/A，不推测数据。

FPS 统计去除重复、零值、pending 和未来时间戳。换应用、换 Surface、休眠后重新建立基线；没有新帧为 0 FPS，没有有效数据为 N/A。历史环被覆盖时标注 `retained ring only`，仅报告保留下来的呈现节奏。不会清空 SurfaceFlinger 全局统计。

采样在 C++ 工作线程串行执行，500/1000/2000 ms 是每轮完成后的等待间隔；实际周期还包含系统命令耗时。root 授权等待最多 30 秒，普通查询有独立短超时，输出受限且持续排空。停止会取消等待、终止子进程组并 join 工作线程。熄屏暂停采样；撤销悬浮权限会结束会话。

## 引擎尺寸的边界

Unity/Unreal 游戏不会因为取得 root 就提供通用内部渲染尺寸。当前读取兼容的开发者遥测文件，同时可以识别可读进程的引擎库、PID、Android 用户和包版本；没有已验证的王者荣耀/和平精英私有尺寸读取器。显示模式、Surface、交换链大小均不能替代升采样前尺寸。协议与 Unity 示例见[引擎说明](engine-resolution.md)。

## 生命周期与验证

插件使用显式依赖 `host.performance-hud`。加载/卸载仅处理 UI 监听和缓存；用户动作才触发权限或会话。配置界面的临时键盘状态归 UI 宿主对象所有。C++ Session 独立于 Activity Surface 存活；Android Service 管理其启停。

主窗口和悬浮窗持有独立 ImGui 上下文、Vulkan 实例/设备/交换链。ImGui 访问通过宿主互斥锁串行化，悬浮窗不复用带全局窗口状态的 Android backend。每次 Surface 销毁先取消并 join 渲染线程，等待 GPU 后释放资源。通知停止、插件停止和错误终止均清理窗口；服务不自动重启。

主机 CTest 覆盖插件生命周期、ImGui 配置/软键盘绘制、FPS 环、前台隔离、硬件计数、JSON 校验、配置往返、命令超时/取消/输出上限及线程退出。运行方法见[插件文档](cpp-plugins.md#宿主测试)。Android CI 同时构建两种 ABI 的 APK 并执行 Lint。

真机验收仍需：权限允许/拒绝/撤销、root 拒绝/超时、切换游戏、横竖屏、Activity/Surface 重建、熄屏恢复、透明度和触摸穿透、通知停止、快速启动/停止、旧配置导入、设备实际 SurfaceFlinger 格式与传感器单位。主机测试和 APK 构建不代表上述设备行为已通过。
