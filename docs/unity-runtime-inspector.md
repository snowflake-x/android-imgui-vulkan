# Unity Runtime Inspector

入口：插件列表 → 内存扫描 → 打开 Unity 运行时 Inspector。启用插件并授权 Root 后，刷新进程，选择目标 PID，点击「附加 / 重新连接」。当前 APK 内置原生 Frida Core / Gum，不需要电脑、Python 或另装 frida-server。

本功能参考 PicoShot/Il2CPP-Base 的运行时对象、组件、类和方法浏览路线；实现为 Android C++ 代理，并未移植 Windows DX11 / Kiero。普通内存搜索和标准元数据导出保留在原页面。

## 功能

- 程序集 → 类型：按程序集浏览、筛选类型，分页读取运行时类列表。
- 类 → 成员：显示继承链中的字段名称、类型、真实偏移、属性标志；方法包含返回类型、参数、重载、原生入口。
- 场景对象：对 UnityEngine.Object 子类调用 FindObjectsOfType，选择对象实例；对 GameObject 浏览组件。对象以会话内 GC 句柄保留，可手动释放。
- 字段：读取和写入实例 / 静态字段。支持布尔、整数、浮点、字符串、对象引用；值类型使用精确大小的十六进制字节。只读 / 常量字段不会通过字段接口改写。
- 方法：通过 il2cpp_runtime_invoke 调用指定重载，显示返回值、ref/out 值和托管异常。开放泛型方法需要先有具体实例化版本，不能直接执行。
- Hook：对真实方法入口计数，按签名覆盖参数 / 返回值，或设置 `skip:true` 跳过原方法。类型覆盖包含布尔、整数、浮点、字符串、对象引用，以及逐字段验证了大小、对齐和偏移的值类型。libffi 负责本机调用约定；显式重叠 / 非标准结构体布局不匹配时拒绝安装。ref/out 槽位的 Hook 覆盖暂不支持，可用方法调用的 ref/out 参数接口或内存查看器。纯计数不要求构造替换签名。本页提供声明式 Hook 策略，没有任意用户 C++ 脚本编译器。
- 原始内存：按地址读写当前目标的可读 / 可写映射，单次最多 1,024 字节。界面默认读取 64 字节，也可从对象地址开始查看。写入不自动回滚。

宿主测试还需要系统 libffi（Ubuntu 的 `libffi-dev`）；Android 使用固定 devkit 内的实现。

字段值使用 JSON，例如 `42`、`true`、`"text"`、`null`、`{"object":"7"}`、`{"bytes":"0000803f"}`。非托管指针使用 `{"pointer":"0x1234"}`，不会将它当作托管对象解引用。64 位整数以带引号的十进制文本显示 / 输入，避免 Android JSON 数值转换损失精度。方法参数为按签名顺序排列的数组，例如 `[42, "hello"]`。Hook 策略 `{}` 仅计数，`{"arguments":{"0":42},"return":1}` 修改第一个参数并覆盖返回值；增加 `"skip":true` 跳过原方法，非 void 方法必须给定替代返回值。默认在原方法执行后覆盖返回值。

界面中的结果可选择复制。内存扫描页面的报告导出也包含最近一次运行时操作的完整响应。

## 执行与生命周期

管理页面仍为 Compose + Miuix。C++ memory-scanner 插件拥有串行工作线程，负责请求、结果、取消及目标切换。Root app_process 加载独立 injector 原生库，验证 PID 启动时间和普通应用 UID，再用 Frida 原生注入 API 加载 C++ agent。没有关闭 SELinux，也不会尝试隐藏注入。

通信使用本机 abstract Unix socket。代理仅接受 UID 0 的连接；Root 端校验服务端 PID / UID，并在操作前后核对 PID 启动时间。每次代理会话有独立标识；旧句柄不能在新会话继续使用。

代理工作线程通过 il2cpp_thread_attach 注册。场景枚举、字段操作和方法调用通过 UnitySynchronizationContext.Post 排到 Unity 线程，使用托管 delegate 和 GC 保活，而非在任意采集线程调用 Unity 场景 API。等待 8 秒仍未开始的任务取消并清空回调；已经开始的任意游戏方法不能安全强行中止。因此超时 / 断连后的写入或调用结果可能未知，不能自动重试。游戏暂停后台时可使用分屏或系统浮动应用窗口让游戏继续运行；本页不是游戏内 ImGui 交互窗口。

断开、切换目标、卸载插件时请求移除 Hook 并释放对象句柄；仍在执行的替换调用会保留闭包及其引用，等待后续清理，避免释放正在执行的代码。最多保留 32 个等待退出的 Hook，达到上限拒绝继续安装。App 生命周期暂停同样卸载此插件。异常退出后代理约 60 秒无请求自动清理，页面存活期间每 15 秒保持会话。为防止已排队 delegate 调到被卸载的机器码，代理映像在目标进程结束前保持驻留。已经写入的值及方法产生的效果不会随断开撤销。

边界：最多 512 个保留对象、32 个 Hook；类型每页检查 2,048 项、场景对象每次最多返回 256 个；成员列表有上限并标记 partial。请求上限 8 KiB，响应通道上限 512 KiB。Hook 到共享泛型原生入口可能影响多个托管方法。

## 兼容性与验证范围

- Android arm64-v8a / x86_64，IL2CPP 已初始化；Mono 不受支持。
- 必须有对应公开 IL2CPP 导出、兼容的托管 delegate / UnitySynchronizationContext 布局。缺失接口、裁剪或混淆会给出具体错误，不伪造成功。
- Root 不保证 ptrace、注入、SELinux 或 Unity 接口一定可用。原生 agent 未在用户的一加真机上验证。
- 原生合成运行时测试覆盖程序集、类、成员、字段读写、方法调用、参数边界、Hook 注册 / 移除及 GC 句柄释放；真实 libffi 闭包测试验证浮点参数覆盖、结构体传参 / 返回和跳过原函数。并非真实 Unity 或原生 Hook 的设备测试。
- Android UI 测试覆盖目标 / 会话与显式字段写入命令。需要后续在可调试的 Unity 示例应用中验证主线程、真实对象生命周期、Root 注入和方法 Hook。

## 依赖与参考

- [Frida 17.2.17](https://github.com/frida/frida/releases/tag/17.2.17)：原生附加和 Gum Hook。CMake 下载官方 devkit 并检查 SHA-256，缓存目录 `.deps/frida/17.2.17/`。内置两种 ABI 的完整 Core 会明显增大 APK。
- libffi 3.4.4：使用 Frida 17.2.17 实际依赖的源码版本 `10bcbcc6295e559b7c952b054e7669a912d3ce06` 生成头文件，链接 devkit 中同版本的符号；不混用宿主机器的 ABI 头文件。许可证见 `assets/licenses/libffi-LICENSE.txt`。
- [nlohmann/json](https://github.com/nlohmann/json/tree/e8b747e4bc0b38fa86ecd0b7972bfcf4119b02a7)：JSON 协议，MIT。
- [PicoShot/Il2CPP-Base](https://github.com/PicoShot/Il2CPP-Base)：交互和能力参考，未复制其游戏特定示例功能。
- [frida-il2cpp-bridge](https://github.com/vfsfitvnm/frida-il2cpp-bridge)：参考 SynchronizationContext / delegate 调度方式，运行时代码在本项目用 C++ 实现。

许可证随 APK 放在 assets/licenses。新增两份原生库只供 Root helper / 目标进程使用，不在普通管理界面初始化 Frida。
