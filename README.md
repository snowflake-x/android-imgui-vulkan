# VulkanScope · Android ImGui Vulkan

原生 Android **Vulkan 1.0 + Dear ImGui** 系统监视器。主界面使用 **Kotlin Compose + Miuix**，配置保存、系统采集和 HUD 使用 **C++ + ImGui**。Android 8.0/API 26 起，支持 `arm64-v8a` 真机与支持 Vulkan 的 `x86_64` 模拟器。主界面提供主页、插件列表两栏，支持中文/英文、系统字体、深浅主题和惯性滚动。

## 功能

- Miuix 管理界面：大标题、圆角卡片、蓝色强调；配置操作通过串行 JNI 工作线程提交到 C++。
- `performance-hud`：接入已有可逆 C++ 插件系统的前台应用监控，支持跨应用 HUD；有 Root 时默认直接创建 SurfaceFlinger 图层，普通悬浮窗为兼容选项。用户启用 root 后，通过前台应用 SurfaceFlinger 的实际呈现时间戳统计 FPS、平均/P95 呈现间隔；系统不提供数据时显示 N/A，不用本应用帧率或刷新率代替。
- HUD 支持 23 项指标的开关、排序和预设，采样间隔、字号、背景透明度、四角位置均可持久保存；可指定 Surface 和 CPU 功耗传感器。
- 自动识别可读取的 Unity/Unreal 引擎进程及游戏版本。**第三方游戏的内部渲染分辨率需要经过验证的游戏/版本读取器；当前尚未提供王者荣耀、和平精英的读取器**，不会用屏幕尺寸冒充。详见[性能 HUD 使用说明](docs/performance-hud.md)和[引擎数据接入说明](docs/engine-resolution.md)。
- 屏幕像素、渲染表面尺寸、密度、设备报告的 DPI、刷新率和旋转角度。
- Android/设备型号、ABI、CPU 核数、可用/总内存。
- Vulkan GPU 名称、API 版本、原始驱动版本、厂商/设备 ID、交换链信息。
- 启动自动申请 root：通过已有 `su`（例如 Magisk）在后台申请权限，验证 UID 0 后开启采样；支持关闭自动申请。拒绝后不循环重试，可点击 **Request root / refresh** 手动重试并查询诊断。
- 不含提权漏洞、不安装 root、不修改系统；未 root 或拒绝授权仍可使用。后台执行，30 秒超时，最多显示 32 KiB 输出，无网络权限。
- 前后台暂停、Surface 销毁/重建、旋转和交换链过期处理。界面可触摸、折叠和滚动。

## 下载自动构建 APK

每次 push、PR 或手动运行都会触发 **Actions → Build Android APK**。

1. 打开成功的运行记录，在 **Artifacts** 按需单独下载 `VulkanScope-debug.apk` 或 `VulkanScope-release-unsigned.apk`。两个 APK 均以原文件上传，不额外打包为 ZIP，无须解压。
2. 安装 **VulkanScope-debug.apk**（已用调试密钥签名，可直接安装）。
3. `VulkanScope-release-unsigned.apk` 是供自行正式签名的发布包，不能直接安装。`SHA256SUMS.txt` 单独提供，供下载后校验。

无须配置 secrets 即可自动产出 APK。推送 `v*` 标签后还会自动创建 GitHub Release，附 APK 和 SHA256。

GitHub 临时 runner 的调试签名可能变化，后续构建若提示签名冲突，请卸载旧调试版后安装。长期分发应为 release 配置自己持有的稳定签名密钥；不要提交密钥。工作流不将调试签名伪装成正式签名。

## 本地构建

使用最新 LTS **JDK 25**（CI 使用 Temurin 25）和 Android SDK 构建，首次构建需下载 Gradle/Android 插件。依赖版本：Gradle 9.1.0、AGP 8.13.2、SDK 36、NDK 28.0.13004108、CMake 3.22.1。

JDK 25 用于运行 Gradle 和编译工具（本地也可用 JDK 21；Miuix 的 Robolectric 页面测试需要 Java 21 或更新版本）。为兼容 Android 的 D8/R8 工具链，Java 生命周期桥接的 JVM 字节码目标保留为 17；可用平台 API 仍受 Android SDK 和 `minSdk = 26` 限制，不代表支持全部 Java 25 API。Android Studio 的 Gradle JDK 也应选择 JDK 25。

```sh
export JAVA_HOME=/path/to/jdk-25
export PATH="$JAVA_HOME/bin:$PATH"
sdkmanager 'platforms;android-36' 'build-tools;35.0.0' 'ndk;28.0.13004108' 'cmake;3.22.1'
export ANDROID_HOME=/path/to/android-sdk
./gradlew assembleDebug assembleRelease lintDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

NDK 28 和链接器按 16 KiB 页大小构建。无 Vulkan 驱动的设备不受支持；系统报告 DPI/温度节点的精度和可读性取决于设备厂商。FPS 来自目标应用实际呈现；GPU 时间另由可用的内核事件采集，不以呈现间隔替代。

## 代码结构

- `app/src/main/java/dev/snowflake/vulkan/ManagerApp.kt`：Miuix 管理页面、配置表单和诊断。
- `app/src/main/cpp/manager/`：串行宿主插件、配置命令与 JSON 快照。
- `app/src/main/cpp/plugin/`：可逆插件内核，管理依赖、类型事件、服务与作用域资源。
- `app/src/main/cpp/features/`：帧率、设备信息、GPU 信息插件；界面 **Plugins** 可独立启停。
- `features/performance_hud.*`：性能插件面板与原生配置界面入口。
- `app/src/main/cpp/monitor/`：配置保存、ImGui 设置、后台采集、进程取消、硬件/FPS/引擎解析。
- `app/src/main/cpp/platform/`：C++ Android 公共 API 调用和独立 Vulkan/ImGui 悬浮窗渲染器。
- [C++ 插件设计与接入](docs/cpp-plugins.md)：参考 DeepSeek harness / Cordis，说明回滚保证、扩展方式和宿主测试。
- `MainActivity.java`、`HudService.java`：仅保留 Activity/Service/Surface 生命周期、权限和通知桥接。
- `.github/workflows/android.yml`：自动编译、Lint、APK 上传、标签发布。
- `app/src/main/cpp/vendor/imgui`：固定 Dear ImGui **v1.91.9b**，上游提交 `f5befd2d29e66809cd1110a152e375a7f1981f06`，附 MIT 许可证，构建时无需拉子模块。Vulkan 窗口辅助结构增加了可选 CompositeAlpha，以支持透明悬浮窗。

## 真机验收

安装后观察 FPS 与曲线、切换横竖屏、返回桌面再进入、锁屏再恢复；核对系统显示设置中的刷新率。分别验证无 `su`、拒绝授权、允许授权及超时场景。GPU 驱动和 root 管理器行为需要真机验证；CI 构建和 Lint 不替代运行测试。
