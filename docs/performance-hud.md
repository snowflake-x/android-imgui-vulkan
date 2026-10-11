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

高级设置提供 Surface 精确名称、CPU 功耗节点与瓦特换算系数。手机可使用 ImGui 触摸键盘或剪贴板粘贴，不依赖 EditText。**Latest diagnostics / Last game diagnostics** 支持复制诊断。上次游戏诊断按 Android 用户和包名保留本次运行中最近 8 个应用各自的最新样本（每项最多 64 KiB，超长会标记截断）；切到桌面再返回本应用后，可在 **Captured application / 已采样应用** 中选择游戏包名查看、复制。桌面样本单独存放，已选中的游戏不会随新采样自动切换；进程结束后历史不保留。

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

## 经典 Metal HUD 界面

布局参照用户提供的经典 Metal HUD 截图和 [Apple 官方 HUD 讲解](https://developer.apple.com/videos/play/tech-talks/110339/)，指标含义同时参照 [Metal HUD metrics](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics)。悬浮窗、主面板实时预览和配置页共用 `DrawClassicHud`：黑色背景、DejaVu Sans Mono 等宽字体、右对齐固定列、蓝色 Pre、绿色 GPU、异常边界红色、白色竖刻度与底边。字体随 APK 分发，许可证位于 `assets/fonts/LICENSE-DejaVu.txt`。

- 头部显示真实 Vulkan GPU 型号、显示模式分辨率和刷新率；第二行标注 SurfaceFlinger 来源，不伪造 Apple 型号、Direct 合成模式或缩放倍率。显示模式尺寸不等于游戏内部渲染尺寸。
- Pre 为实际呈现间隔（ms），当前值取最近最多 120 个新间隔均值，FPS 为该均值的倒数。方括号为保留的最多 1200 个历史间隔的极值，曲线展示最近 200 个间隔。重复轮询不添加点；环形缓冲缺口断开曲线，切换应用/用户/Surface、休眠或失效时重置历史。没有新呈现时 FPS 显示 0，Pre 当前值显示 `--`。
- FPS 下界低于当前值 80%，或 Pre 上界高于当前值 120% 时标红。这是本实现的显示阈值，并非声称复现 Apple 内部算法。
- Mem 为前台主进程 `/proc/<pid>/status` 的 VmRSS（十进制 MB）；读取由 C++ 发起，需 root，并验证 Android 用户。它不是 PSS、整机内存或 Metal 资源分配大小。GPU 耗时与 GPU 内存尚无可信跨应用来源，显示 `--`，不会使用 GPU 使用率替代毫秒，也不会绘制假零值曲线。
- **Frame history graph** 控制曲线，**Additional sensor rows** 显示附加指标；核心行保持固定位置，禁用值显示 `OFF`，已启用但尚无有效样本的值显示 `--`。其他字段的显示顺序继续由配置控制。主面板的 **Sample details** 保留完整文本采样报告，其轮询区间 FPS 与 HUD 的滚动均值 FPS 口径不同。
- HUD 底部独立显示采集目标包名、采样状态以及 GPU 耗时/显存能力。完整诊断同时列出已启用字段，避免把配置关闭误判为采集失败。Game/Hardware 预设均保留 GPU 型号、FPS、Pre 和主进程 Mem；旧配置保留原选择，更新后重新选择预设即可应用修复。
- 配置格式升级为版本 2，兼容读取版本 1，并保留旧字段选择。Android 窗口仍遵守 0.8 的触摸穿透透明度限制，背景滑块在此基础上调整。

主机 `classic_hud_test` 检查滚动统计、历史隔离、缺口与尺寸约束，并可接收一个 PPM 输出路径，从实际 ImGui 三角形和随包字体生成预览；预览中的数值为测试样本，不会进入生产采集路径。

主程序现采用底部双页导航，并提供中英文配置与自动系统字体；见[原生界面说明](native-ui.md)。最新配置版本为 3，增加语言偏好并兼容此前的版本 1、2。

## 前台图形 API 调用跟踪

在 ImGui 配置中启用 **Graphics API (root call tracing) / 图形 API（root 调用跟踪）**，并显式启用 root 采集。该字段默认关闭，保存在现有原生配置中。启用后 Classic HUD 的第二行显示本次观察到的 API；主页报告及诊断提供完整状态。原有 `GPU API` 字段仍是悬浮窗自身 Vulkan 设备能力，不代表目标应用。

实现位于 `monitor/graphics_trace.*`，由现有性能 HUD 插件的 Collector 工作线程调用，不增加 Java/Kotlin 功能层。每轮通过包名和 Android user 查找前台应用的主进程，读取其映射，核对本地库文件的设备号与 inode，从 ELF32/ELF64 动态符号及可执行 PT_LOAD 段计算文件偏移。库的存在本身不会被作为 API 检测结果。

使用 root 写入内核 tracefs uprobes，跟踪：

- `eglSwapBuffers`、`eglSwapBuffersWithDamageKHR/EXT`。
- `vkQueuePresentKHR`、`vkQueueSubmit`、`vkQueueSubmit2/KHR`。

每轮建立唯一事件组和独立 trace 实例，对目标进程当时存在的线程设置 `common_pid` 过滤，开启 200 ms 后停止。采样前后核对进程启动时间、UID 和 cmdline；仅在采样成功并读取到对应调用事件时报告 GLES、Vulkan 或两者。没有事件时显示未知，不保留上一进程的结论，也不推断应用未使用 GPU。跟踪会增加开销，采集周期还包含 setup/cleanup 时间，不能将此模式视为零开销性能测量。

停止、超时或失败时，shell trap 清理本轮事件；root shell 内部另有两秒超时，防止 su 服务脱离应用进程组后继续采样；C++ 另执行一次不受采集取消标志影响、最长五秒的清理。失败后先等待内部时限结束再清理，正常采样无此等待。只操作本次创建的实例和事件，不清空全局 trace、不改变 SELinux 策略。若清理失败，显示错误并停止本次 Collector 生命周期中的后续跟踪，建议重启设备后重试。系统强制终止、内核错误或 root 服务异常仍可能导致残留，不能保证此类情况下成功清理。

### 支持边界

- 需要内核 `CONFIG_UPROBES`、`CONFIG_UPROBE_EVENTS`、tracefs 实例支持，以及 root/SELinux 允许访问；root 本身不保证可用。不会自动挂载 tracefs 或关闭 SELinux。
- 只跟踪可读取、具有动态函数符号的目标进程 `libEGL.so`、`libvulkan.so`；内核或符号不支持时显示不可用。APK 内映射、自定义图形库、其他进程负责渲染的应用尚不覆盖。
- Vulkan 通过 `vkGetDeviceProcAddr` 直接取得驱动函数地址时可能绕过 loader 探针；EGL 扩展地址同样可能绕过导出函数。200 ms 内没有调用、采样后创建的新线程、缓冲区截断都可能漏检。检测结果表示“本次观察到”，不能证明列出了全部 API。
- DX11/DX12 经兼容层转换时这里只能观察底层 EGL/Vulkan；不宣称识别上层 DirectX 版本或 Vulkan/GLES API 版本。
- 宿主测试覆盖真实 ELF 动态符号、损坏文件、事件判定、独立实例脚本、身份检查和取消/失败后的清理。Android 双 ABI 编译不能代替设备内核验证；必须在支持 uprobes 的 root 设备上验证 EGL/Vulkan 应用、混合应用、切前台和停止采集。
