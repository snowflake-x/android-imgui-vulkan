# 原生性能悬浮窗与配置

`performance-hud` 的悬浮窗、配置页和诊断页均由 **C++ + Dear ImGui + Vulkan** 绘制。C++ 负责配置持久化、采集调度、root 子进程、指标解析和系统信息读取。应用不再包含 Kotlin 插件或 Kotlin 业务代码。

Android 仅保留 `MainActivity.java`、`HudService.java` 两个生命周期桥接：系统权限入口、前台通知、WindowManager 的 SurfaceView 容器以及 Surface 回调。没有 TextView 悬浮窗、Android 配置表单、Java 指标采集或 Java 配置逻辑。NDK 未提供的电池、显示、温控及剪贴板公共 API 由 C++ 直接通过 JNI 调用；JNIEnv 不跨线程保存。

## 使用

1. 在 **Foreground performance HUD → Configure** 展开 ImGui 配置页，点击 **Grant overlay permission** 到系统页面授权。Android 13+ 可点 **Allow HUD notification** 授权通知。
2. 选择字段、顺序、字号、背景透明度、四角位置和采样间隔。设置立即保存；可使用 Game/Hardware 字段预设。
3. 主界面启动时默认在后台尝试 `su -c id -u`，30 秒超时；只有确认 UID 0 后才启用并保存 root 采样。可关闭「启动时自动申请 Root」，也可手动关闭采样。自动申请不会启动 HUD 或开启独立调用跟踪；设备仍需已有的 `su` 提供者。GPU 内存字段默认开启，GPU 耗时需要另外在字段设置中启用；耗时采集会短暂启用独立的内核跟踪实例。
4. 点击 **Start HUD** 后切换游戏。首次有效采样只建立基线，下一次开始统计。拒绝或超时后不会循环弹窗，可手动点击 Root 重试；申请期间关闭自动申请不会被迟到的授权结果覆盖。
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
| GPU 耗时 | KGSL 硬件命令批次活跃时长，或标准 `gpu_work_period` 的 UID 活跃时间窗口；分别注明 `ms/batch`、`ms/window`，不冒充逐帧耗时 |
| GPU 内存 | 前台主进程 KGSL 记账的自分配与导入缓冲字节数；共享系统内存，非独立显存容量，详见下节 |
| 内存 | `/proc/meminfo` 的 MemTotal、MemAvailable；整机已用/总内存 |
| 电池功率、温度 | C++ 调用 Android 电池 API；电流×电压是整机电池功率估计，不是 CPU 功耗 |
| 温控 | C++ 调用 Android 10+ 温控压力 API，不是 CPU 核心温度 |
| 显示 | C++ 查询物理显示模式、刷新率、密度、旋转；系统属性读取型号和 Android 版本 |
| 引擎内部/输出尺寸 | 校验后的引擎自报数据；缺少兼容读取器时明确显示 N/A |

CPU 传感器只接受 `/sys/` 路径，不接受 `..` 或 shell 语法。单位系数：µW → `0.000001`，mW → `0.001`，W → `1`。电池节点、频率或温度不能替代 CPU 功率。不可访问或不支持的指标显示 N/A，不推测数据。

图层选择兼容旧版原始名称和 Android 15/16 的 `RequestedLayerState{name#id ...}` 调试描述；发送给 `--latency` 的参数只包含真实 `name#id`。列表先按目标包过滤再做有界捕获，避免无关图层占满输出。最多保留 128 个候选，每轮最多查询 8 个并设置 2.5 秒总查询预算；优先复用仍有新呈现的图层，其余候选轮转探测，失效缓存取消优先级。手动 Surface 仍限定当前包且精确匹配名称，也可粘贴完整调试描述。

诊断记录包含 `surface_list_query`、有界列表原文、候选数、探测数与 `surface_queries`（实际查询名称、命令状态及输出片段），用于区分名称未匹配、命令失败和没有有效 actual-present 时间戳。超出输出上限会注明截断。格式依据 [AOSP Android 16 图层列表与延迟查询](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/android-16.0.0_r1/services/surfaceflinger/SurfaceFlinger.cpp#L5962) 和 [图层调试名称](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/android-16.0.0_r1/services/surfaceflinger/FrontEnd/RequestedLayerState.cpp#L421)；不启用或清空系统全局统计。

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
- Mem 左侧为前台主进程 `/proc/<pid>/status` 的 VmRSS（十进制 MB），右侧绿色方括号为 KGSL GPU 内存（同为十进制 MB）；两者可能重叠，不能相加。GPU 行显示本次采样得到的活跃耗时均值及最小/最大值，底部注明批次或工作窗口单位。GPU 数据独立于 SurfaceFlinger，FPS 缺失不妨碍读取。没有可信数据时显示 `--`，禁用字段显示 `OFF`；不使用 GPU 使用率替代毫秒，不将批次/工作窗口与 Pre 帧曲线混画。
- **Frame history graph** 控制曲线，**Additional sensor rows** 显示附加指标；核心行保持固定位置，禁用值显示 `OFF`，已启用但尚无有效样本的值显示 `--`。其他字段的显示顺序继续由配置控制。主面板的 **Sample details** 保留完整文本采样报告，其轮询区间 FPS 与 HUD 的滚动均值 FPS 口径不同。
- HUD 底部独立显示采集目标包名、采样状态以及 GPU 耗时/显存能力。完整诊断同时列出已启用字段，避免把配置关闭误判为采集失败。Game/Hardware 预设均保留 GPU 型号、FPS、Pre 和主进程 Mem；旧配置保留原选择，更新后重新选择预设即可应用修复。
- 配置格式升级为版本 2，兼容读取版本 1，并保留旧字段选择。Android 窗口仍遵守 0.8 的触摸穿透透明度限制，背景滑块在此基础上调整。

主机 `classic_hud_test` 检查滚动统计、历史隔离、缺口与尺寸约束，并可接收一个 PPM 输出路径，从实际 ImGui 三角形和随包字体生成预览；预览中的数值为测试样本，不会进入生产采集路径。

主程序现采用底部双页导航，并提供中英文配置与自动系统字体；见[原生界面说明](native-ui.md)。最新配置版本为 3，增加语言偏好并兼容此前的版本 1、2。

## GPU 耗时与共享内存

采集器 `monitor/gpu_memory.*`、`monitor/gpu_timing.*` 复用已有 C++ 后台工作线程。先确定前台包的 Android 用户、主进程 PID、UID 与启动时间，读取前后核验身份；进程退出、PID 复用或查询失败时不沿用旧数据。该功能无需向游戏注入代码。

**内存：** 优先读取 `/sys/class/kgsl/kgsl/proc/<PID>/`，兼容部分驱动的 `/sys/class/kgsl/kgsl-3d0/proc/<PID>/`。`kernel` 是当前 KGSL 自分配字节数，`imported_mem` 是驱动按 EGL 引用比例分摊的 DMA-BUF 导入字节数；两者相加得到此采集口径下的 GPU 共享内存记账量。`gpumem_mapped` 是 `kernel` 中已映射到 CPU 的子集，不能再次相加；`kernel_max`、`user_max` 是历史峰值，不能当当前值。具体来源、组成与读取失败原因保留在诊断中。它不等于独立 VRAM、物理驻留量或整个多进程应用的总量，也不能与 RSS/PSS 相加。

没有完整的 sysfs 计数时，可回退到可读的 `/sys/kernel/debug/kgsl/proc/<PID>/mem`（或 `/d/kgsl/proc/<PID>/mem`）。解析完整、有界且格式受支持的分配表，按同样口径累计 `gpumem` 与比例分摊的 `ion`，排除纯虚拟地址预留。此口径不包含通过 USERMEM_ADDR 导入的 CPU `usermem`，不另外累计 CPU 映射。只取得 `kernel` 时说明“仅分配量”，不把缺失导入量当作零。`gpu_memory_queries` 保留至多 4096 字符的查询诊断。

**耗时：** 默认关闭，以免仅更新应用就增加跟踪开销。启用后优先尝试 `kgsl/adreno_cmdbatch_retired`：仅在 KGSL context 到目标 PID 的归属可验证时采集 200 ms，使用硬件报告的 `active` AO ticks 按 19.2 MHz 换算为毫秒，显示 `ms/batch`。`start` 到 `retire` 的跨度可能包含抢占等待，不用于替代 `active`；事件所属的内核工作线程 PID 也不用于判断游戏归属。

无可用 context 映射时尝试标准 `power/gpu_work_period`，按真实 UID 过滤并采集约 1.1 秒，读取 `total_active_duration_ns`。该事件由一加 13 的 KGSL 驱动约每 900 ms 输出，显示 `ms/window` 并记录实际窗口长度；统计包含同 UID 的进程，共享 UID 的包无法单独区分。两种口径都不是游戏的逐帧 GPU 渲染耗时，不能用它们直接计算 FPS。没有事件不等于零耗时。

跟踪只操作本次创建的独立 tracefs 实例，不清空系统全局 trace、不挂载 debugfs、不改变 SELinux。进程更替、取消、超时、格式不支持、输出截断或 trace 丢失时丢弃本轮数值；退出 trap 和额外有界清理负责移除实例。清理失败会停止当前应用会话后续 GPU 耗时跟踪并报告原因；停止再启动 HUD 也不会解除禁用。总采样周期还包含 200 ms 或约 1.1 秒的捕获时间及命令耗时。内核未导出事件或权限阻止访问时显示 N/A，root 授权本身不能保证所有驱动支持。

单位与路径依据一加官方 [SM8750 Android 16 KGSL 驱动](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/tree/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/graphics-kernel)：[`kgsl_sharedmem.c` 的进程内存属性](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/d50b305f7da9e14715a25120a4ac7b1a4b8b97c3/vendor/qcom/opensource/graphics-kernel/kgsl_sharedmem.c#L223)、`kgsl.c` 的 work-period 计时与 AO 换算、`kgsl_power_trace.h` 的纳秒字段、`adreno_trace.h` 的批次事件，以及 `adreno_debugfs.c` 的 context 进程归属。设备上的事件和文件仍须实际探测；开源驱动与双 ABI 构建验证不能替代对应固件的真机验收。

## 前台图形 API 调用跟踪

在 ImGui 配置的 Root 选项附近启用 **Graphics API (root call tracing) / 图形 API（root 调用跟踪）**，并显式启用 root 采集。这是独立开关，默认和字段预设均关闭；仅授权 Root 不会开启探针。启用后 Classic HUD 的第二行显示本次观察到的 API；关闭、等待、权限失败、符号不可用和未捕获调用分别显示状态。主页报告及诊断提供完整原因。`Device Vulkan API version / 设备 Vulkan API 版本`（配置键仍为 `gpu_api`）是监控程序查询到的 Vulkan 设备能力，不代表目标应用当前使用的 API 或版本。

实现位于 `monitor/graphics_trace.*`，由现有性能 HUD 插件的 Collector 工作线程调用，不增加 Java/Kotlin 功能层。每轮通过包名和 Android user 查找前台应用的主进程，先筛选 EGL/Vulkan 可执行映射再有界读取，避免大型游戏的无关映射占满输出上限，从 ELF32/ELF64 动态符号及可执行 PT_LOAD 段计算文件偏移。优先核对本地库文件的设备号与 inode；本地不可读或文件与映射不一致时，通过 Root 有界读取 `/proc/<pid>/root/<映射路径>`（单个库最多 16 MiB），在 C++ 内存中解析 ELF，并用对应路径设置探针。读取前后核对进程身份及文件设备号、inode，避免 Android 挂载命名空间不同导致读错库。未观察到调用或 tracefs 不可用时，另行展示已加载库类别（`Loaded:`），不将库的存在当作实际调用证据。

诊断中的 `graphics_api_status` 是显示状态；`mapped` 表示仅确认库加载，`graphics_api_trace_status` 保留原始跟踪阶段状态，`graphics_api_details` 保留有界的失败细节。`symbols_unavailable` 表示尚未找到可用探针符号，不能据此断言内核不支持；`trace_unavailable` 表示 tracefs 或实例不可用，`probe_failed` 表示注册/启用探针失败，`no_calls` 表示本轮完整采样未观察到调用。授权、目标进程、捕获和清理失败也各自保留状态。停止或采样过期时，悬浮窗不继续显示旧的 API 结论。

使用 root 写入内核 tracefs uprobes，跟踪：

- `eglSwapBuffers`、`eglSwapBuffersWithDamageKHR/EXT`。
- `vkQueuePresentKHR`、`vkQueueSubmit`、`vkQueueSubmit2/KHR`。
- 厂商驱动导出的同名函数及 `glDrawArrays`、`glDrawElements`；每个库使用独立事件，避免 loader 与 driver 的同名符号相互覆盖。

每轮建立唯一事件组和独立 trace 实例，对目标进程当时存在的线程设置 `common_pid` 过滤，开启 200 ms 后停止。采样前后核对进程启动时间、UID 和 cmdline；仅在采样成功并读取到对应调用事件时报告 GLES、Vulkan 或两者。没有事件时显示已加载库类别和 `Loaded:`（仅加载，调用未确认），不保留上一进程的结论，也不推断应用未使用 GPU。跟踪会增加开销，采集周期还包含 setup/cleanup 时间，不能将此模式视为零开销性能测量。

停止、超时或失败时，shell trap 清理本轮事件；root shell 内部另有两秒超时，防止 su 服务脱离应用进程组后继续采样；C++ 另执行一次不受采集取消标志影响、最长五秒的清理。失败后先等待内部时限结束再清理，正常采样无此等待。只操作本次创建的实例和事件，不清空全局 trace、不改变 SELinux 策略。若清理失败，显示错误并停止当前宿主会话中的后续跟踪，HUD 停止/启动和 Root 开关不能清除此状态，建议重启设备后重试。系统强制终止、内核错误或 root 服务异常仍可能导致残留，不能保证此类情况下成功清理。

### 支持边界

- 需要内核 `CONFIG_UPROBES`、`CONFIG_UPROBE_EVENTS`、tracefs 实例支持，以及 root/SELinux 允许访问；root 本身不保证可用。不会自动挂载 tracefs 或关闭 SELinux。
- 跟踪可读取、具有动态函数符号的系统 EGL/GLES/Vulkan 库及 Adreno、Mali、ANGLE 和 `vulkan.*.so` 驱动；内核或符号不支持时显示不可用。APK 内映射、自定义图形库、其他进程负责渲染的应用尚不覆盖。
- Vulkan 通过 `vkGetDeviceProcAddr` 直接取得驱动函数地址时可能绕过 loader 探针，现增加可用驱动导出符号探针；未导出或私有函数地址仍可能漏检。EGL 扩展地址同样可能绕过导出函数。200 ms 内没有调用、采样后创建的新线程、缓冲区截断都可能漏检。检测结果表示“本次观察到”，不能证明列出了全部 API。
- DX11/DX12 经兼容层转换时这里只能观察底层 EGL/Vulkan；不宣称识别上层 DirectX 版本或 Vulkan/GLES API 版本。
- 宿主测试覆盖真实 ELF 动态符号、Root 文件读取回退、损坏文件、事件判定、阶段状态、独立实例脚本、身份检查和取消/失败后的清理。Android 双 ABI 编译不能代替设备内核验证；必须在支持 uprobes 的 root 设备上验证 EGL/Vulkan 应用、混合应用、切前台和停止采集。


## Root 独立显示图层

默认开启「Root 独立显示图层」。已取得 Root 授权（或启用了 Root 采样）时，启动显示由 C++ 后台进程控制器通过 `su` 启动 `app_process`，直接创建默认显示屏的 SurfaceControl 图层。此路径不调用 WindowManager，不检查或申请 SYSTEM_ALERT_WINDOW。没有 Root 时使用普通悬浮窗；有 Root 但直接显示失败时**不会静默降级**，主页报告会保留失败阶段和系统异常。可以关闭此选项、授予普通悬浮窗权限，再重新启动显示。

特权进程仅持有图层，按显示屏旋转、原生计算的尺寸和四角配置更新布局。Surface 经 Binder 交给应用内现有 C++ Vulkan/ImGui 渲染器，配置、采集、GPU 事件跟踪和字体资源仍由 C++ 宿主管理。图层没有输入窗口，不拦截游戏触摸。不会修改 SELinux、授予额外系统权限或写入系统分区。

Root 启动桥接要求 UID 0 和一次性随机会话令牌；导出的 Provider 不提供文件、配置或指标查询。停止服务先撤销 Binder、等待原生 Surface 渲染退出，再取消 Root 子进程。辅助进程监听宿主 Binder 死亡，并具有 15 秒心跳看门狗；服务也检查 30 秒启动期限，避免孤立图层和无限等待。

这是系统隐藏 SurfaceControl API 的兼容路径（主要面向 Android 10–16），不是「Root 后没有任何系统限制」。厂商固件、SELinux 和 SurfaceFlinger 策略仍可能拒绝创建；错误会完整显示，需在具体机型验证。普通悬浮窗作为手动兼容选项保留。开启该显示方式不会改变图形 API 跟踪的证据要求：`Loaded:` 表示只确认库加载，不能证明当前使用它进行渲染。
