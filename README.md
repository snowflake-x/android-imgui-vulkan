# VulkanScope · Android ImGui Vulkan

原生 Android **Vulkan 1.0 + Dear ImGui** 系统监视器。Android 8.0/API 26 起，支持 `arm64-v8a` 真机与支持 Vulkan 的 `x86_64` 模拟器。界面使用英文，避免系统中文字体依赖。

## 功能

- 实时本应用 FPS、平均帧耗时、最近 180 帧耗时曲线。FIFO 垂直同步；不是其他游戏的全局 FPS 或悬浮窗。
- 屏幕像素、渲染表面尺寸、密度、设备报告的 DPI、刷新率和旋转角度。
- Android/设备型号、ABI、CPU 核数、可用/总内存。
- Vulkan GPU 名称、API 版本、原始驱动版本、厂商/设备 ID、交换链信息。
- 可选 root 查询：点击 **Request root / refresh**，通过现有 `su`（例如 Magisk）请求用户授权，只读查询内核、CPU、内存、SELinux、温度节点。
- 不含提权漏洞、不安装 root、不修改系统；未 root 或拒绝授权仍可使用。后台执行，30 秒超时，最多显示 32 KiB 输出，无网络权限。
- 前后台暂停、Surface 销毁/重建、旋转和交换链过期处理。界面可触摸、折叠和滚动。

## 下载自动构建 APK

每次 push、PR 或手动运行都会触发 **Actions → Build Android APK**。

1. 打开成功的运行记录，在 **Artifacts** 下载 `VulkanScope-APKs-<编号>`。
2. 解压，安装 **VulkanScope-debug.apk**（已用调试密钥签名，可直接安装）。
3. `VulkanScope-release-unsigned.apk` 是供自行正式签名的发布包，不能直接安装。

无须配置 secrets 即可自动产出 APK。推送 `v*` 标签后还会自动创建 GitHub Release，附 APK 和 SHA256。

GitHub 临时 runner 的调试签名可能变化，后续构建若提示签名冲突，请卸载旧调试版后安装。长期分发应为 release 配置自己持有的稳定签名密钥；不要提交密钥。工作流不将调试签名伪装成正式签名。

## 本地构建

需要 JDK 17+、Android SDK，首次构建需下载 Gradle/Android 插件。依赖版本：Gradle 8.11.1、AGP 8.9.1、SDK 35、NDK 28.0.13004108、CMake 3.22.1。

```sh
sdkmanager 'platforms;android-35' 'build-tools;35.0.0' 'ndk;28.0.13004108' 'cmake;3.22.1'
export ANDROID_HOME=/path/to/android-sdk
./gradlew assembleDebug assembleRelease lintDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

NDK 28 和链接器按 16 KiB 页大小构建。无 Vulkan 驱动的设备不受支持；系统报告 DPI/温度节点的精度和可读性取决于设备厂商。FPS 为 ImGui 滚动平均，曲线是帧间隔，不是 GPU 时间戳测量。

## 代码结构

- `app/src/main/cpp/main.cpp`：NativeActivity、Vulkan、触摸输入、宿主面板与生命周期。
- `app/src/main/cpp/plugin/`：可逆插件内核，管理依赖、类型事件、服务与作用域资源。
- `app/src/main/cpp/features/`：帧率、设备信息、GPU 信息插件；界面 **Plugins** 可独立启停。
- [C++ 插件设计与接入](docs/cpp-plugins.md)：参考 DeepSeek harness / Cordis，说明回滚保证、扩展方式和宿主测试。
- `app/src/main/java/dev/snowflake/vulkan/MainActivity.java`：屏幕/设备信息与受限 root 查询。
- `.github/workflows/android.yml`：自动编译、Lint、APK 上传、标签发布。
- `app/src/main/cpp/vendor/imgui`：固定 Dear ImGui **v1.91.9b**，上游提交 `f5befd2d29e66809cd1110a152e375a7f1981f06`，附 MIT 许可证，构建时无需拉子模块。

## 真机验收

安装后观察 FPS 与曲线、切换横竖屏、返回桌面再进入、锁屏再恢复；核对系统显示设置中的刷新率。分别验证无 `su`、拒绝授权、允许授权及超时场景。GPU 驱动和 root 管理器行为需要真机验证；CI 构建和 Lint 不替代运行测试。
