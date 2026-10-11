# 前台游戏的引擎内部渲染分辨率

性能 HUD 使用现有插件框架启停，`engine_resolution`、`engine_output`、`engine_source` 可以分别显示、隐藏和排序。**屏幕像素、Surface/窗口大小、交换链大小都不能证明游戏的内部渲染分辨率**。例如游戏先以 1280 × 720 渲染、再升采样到 1920 × 1080，内部尺寸必须显示前者。

## 已安装的第三方游戏

开启 root 采集后，自动根据前台包名、Android 用户和进程查询引擎共享库：`libunity.so` 识别为 Unity，`libUE4.so` / `libUnreal.so` 识别为 Unreal；同时读取安装包的 `versionCode`。支持按《王者荣耀》`com.tencent.tmgp.sgame` 和《和平精英》`com.tencent.tmgp.pubgmhd` 的包名定位进程，**名称本身不作为引擎类型证据**。没有读取到引擎库时显示 Unknown。

当前实现自动识别进程、引擎和游戏版本，并允许在 C++ 采集器中扩展按游戏版本验证的读取器；**尚未提供这两个游戏的内部渲染分辨率读取器**。因此只有识别到引擎时，内部尺寸仍会明确显示 `N/A (no compatible ... resolution reader)`。这不是已完成对任意 Unity/UE 游戏的内部尺寸读取。

root 只能提供读取权限，不能把跨版本变化的引擎对象布局变成稳定 API。实现第三方游戏读取器需要相应版本的可验证渲染数据入口、ABI、引擎库构建身份和真实设备上的升采样前尺寸验证。新增 C++ 读取器必须对未知版本返回不支持；不得套用来源不明的偏移、用屏幕大小补值或把其他进程的样本移过来。

一加 Android 15 上，实际覆盖还取决于游戏构建及系统 SELinux 的进程可读性。直接从 APK 映射的原生库在 `/proc/<pid>/maps` 中可能只显示 APK 路径；这种情况下目前无法确定引擎，显示 Unknown，而不从游戏名称猜测。游戏在不同包名的独立子进程里运行时也需要专项支持。工作资料用户使用自己的 Android user ID 查询，不读取主用户下同包名游戏的数据。

ImGui 设置页的 **Last game diagnostics → Copy game diagnostics**可以提供包名、引擎来源/版本、PID、Android 用户和采集状态，作为对应游戏版本适配的依据。仅有这些信息还不能确定内存地址或内部尺寸。

## 可控游戏的可选诊断导出

这条路径用于你能修改的游戏、测试构建或已有兼容诊断出口。它不要求也不声称普通安装版第三方游戏会生成文件。Unity 示例在 [integrations/unity](../integrations/unity/)；UE 项目可以在自己已验证的渲染回调中按同一协议导出。导出的值在来源中标记为 `engine-reported`，属于游戏自报数据，不是外部独立测量。

游戏每 500 ms 左右，将 UTF-8 JSON 原子替换到以下任一位置：

- 首选：Android `Context.getFilesDir()/vulkanscope-render.json`。
- 可选：`Context.getExternalFilesDir(null)/vulkanscope-render.json`，当前实现对应 `/storage/emulated/<user>/Android/data/<package>/files/`。

采集器先读取内部目录，仅在内部文件不存在时读取外部目录。切换导出路径时移除旧文件，避免旧的内部报告遮住外部新报告。游戏暂停、后台或停止渲染时应停止更新；超过 5 秒的报告自动失效。采集器不会请求文件访问权限或更改文件权限，仍通过用户已开启的 root 权限只读查询。

```json
{
  "schemaVersion": 1,
  "packageName": "com.example.game",
  "engine": "Unity",
  "source": "URP14/cameraColorTarget viewport pre-upscale",
  "pid": 12345,
  "elapsedRealtimeMs": 12345678,
  "renderWidth": 1280,
  "renderHeight": 720,
  "outputWidth": 1920,
  "outputHeight": 1080
}
```

`engine` 只能是 `Unity` 或 `Unreal`。所有数值使用 JSON 整数；宽高为 1–32768 像素。输出宽高可一起省略，内部宽高不可省略。时间戳必须来自 Android `SystemClock.elapsedRealtime()`，或原生 `clock_gettime(CLOCK_BOOTTIME)` 换算的毫秒值；不能使用 Unity 的启动时长、UE 的应用计时或墙上时钟。`source` 是不超过 128 字符的 ASCII 测量位置说明。

采集器拒绝超过 4 KiB、格式错误、未知协议版本、包名不匹配、PID/Android 用户不匹配、无效尺寸、未来或过期的报告。通过 `.tmp` 文件和同目录原子 rename 发布，避免读到半写入内容。只有活着且属于当前前台包名及用户的进程报告会被使用；root 授权失败时不会反复请求授权。

## Unity 示例的范围

- 将 `VulkanScopeTelemetry.cs` 加入项目。它使用 Android 私有 files 目录，单次写入后通过 `android.system.Os.rename` 原子发布。
- Built-in：把 `VulkanScopeBuiltinCamera.cs` 放到唯一的最终游戏相机，并排在任何升采样图像效果之前。它报告 `OnRenderImage` 的源渲染纹理尺寸，随后正常 blit。
- Unity 2022.3 / URP 14：加入 `VulkanScopeUrp14Feature.cs`，把 Renderer Feature 放入该相机使用的 Universal Renderer，保留唯一带 `MainCamera` 标签的 base 相机。它在后处理前读取 camera-color RTHandle 的当前 viewport；固定尺寸纹理则读取其纹理尺寸，避免误用 RTHandle 的历史最大分配尺寸。URP 的软件 Render Scale 可以使此尺寸不同于屏幕大小。

示例只覆盖全屏、非 XR、未输出到自定义目标纹理的选定游戏相机。硬件动态分辨率 `useDynamicScale` 被明确排除；需要按具体渲染路径读取实际缩放后的 viewport。也不适用于 URP 17 RenderGraph、HDRP、分屏、多相机合成、在该回调前已升采样的自定义管线。没有支持的数据时停止发布，HUD 等待旧样本过期后显示 N/A。示例的 Unity API 已对照 2022.3 Graphics 源码，尚未在 Unity 编辑器或真机项目中编译运行；应在自己的具体版本中验证。

API 参考：[RTHandle](https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.core/Runtime/Textures/RTHandle.cs)、[ScriptableRendererFeature](https://github.com/Unity-Technologies/Graphics/blob/2022.3/staging/Packages/com.unity.render-pipelines.universal/Runtime/ScriptableRendererFeature.cs)。

## Unreal 导出要求

UE 项目应在自己确认动态分辨率和 Screen Percentage 已应用、升采样尚未执行的渲染回调读取场景视图实际渲染 viewport。对应版本中通常关联 `FSceneView::ViewRect`；`UnscaledViewRect`、最终 viewport 和场景目标纹理的最大分配 extent 不能直接作为内部尺寸替代。参考 [FSceneView API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FSceneView)。

本仓库没有注册可直接适用于所有 UE 版本的 SceneViewExtension，也没有把未经编译验证的回调宣称为完成的 SDK。集成方需要为自己的 UE 版本验证取值时点、单眼/多视图、动态分辨率和上采样器行为，再按上面的协议导出 `engine: "Unreal"`。

## 验证

1. 在可控游戏中设置输出 1920 × 1080、内部 1280 × 720，确认 HUD 两项分别显示正确值。
2. 改变内部 Render Scale，保持屏幕大小不变，确认内部尺寸随渲染目标变化。
3. 暂停发布 6 秒、填写其他包名、终止报告 PID、换 Android 用户，均应显示 N/A。
4. 普通安装版游戏没有兼容读取器时，应显示检测到的引擎/游戏版本以及缺失读取器的原因，不能显示屏幕尺寸充当内部尺寸。

`app/src/main/cpp/tests/native_monitor_test.cpp` 覆盖协议新鲜度、字段边界、前台包隔离、内部与输出尺寸分离和 JSON 严格类型检查；设备上的真实渲染行为仍需对应游戏版本验证。
