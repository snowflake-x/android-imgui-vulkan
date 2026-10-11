# 内存扫描与布局分析插件

入口：「插件列表 → 内存扫描 → 启用 → 打开内存扫描」。也可从主页进入已启用插件。插件默认关闭；加载、刷新主页和打开页面都不会自动扫描。需要现有 Root 授权；通过「刷新正在运行的应用进程」选择 PID，同一包的子进程分别列出，支持 Android 多用户。

## 操作

- 精确搜索：UTF-8 文本、十六进制字节、Int32、Int64、Float32。整数和浮点数按目标平台的小端表示精确匹配，包括未对齐地址；没有模糊值或变化值反复筛选。
- 内存查看：输入十六进制地址，读取 256 B、1 KiB 或 4 KiB。十六进制 / ASCII 视图按同一 PID、启动时间和地址的连续快照标出变化字节。可跟随可读指针候选继续读取。
- 布局推断：显示字段偏移及 i32/u32/i64/f32/f64 的替代解释；识别可读指针、ASCII、基本 UTF-16LE 文本和零值 / 填充候选。根据目标 ELF 识别 32/64 位指针宽度。所有候选均不等同于已确认的字段声明。
- Unity：在所选进程存在 IL2CPP 库时，扫描运行中内存中的标准元数据头，校验字符串表、类型表、类型 token 和记录大小，导出命名空间与类型名称。支持标准 metadata v24（104/100/92 字节记录）、v27/v29/v31（88 字节记录）。不把任意内存字符串当成类名。
- Unity 对象头：通过对象的 klass / monitor，以及常见 Il2CppClass 前缀中的 image / name / namespace 指针，尝试识别对象类型；也显示 System.String 和数组的候选头偏移。还会在类结构前 256 字节中寻找 FieldInfo 表指针，以 parent / token / name / type 交叉校验最多 64 个字段候选，展示字段名、类型码、实例偏移及已读取字节；静态字段单独标记，不当成实例偏移。这是版本相关的启发式判断，不是运行时 API 确认或完整实例字段布局。
- 导出：通过 Android 文档选择器保存本次保留的完整文本结果。页面报告预览最多 150 行。

元数据导出列出元数据定义的类型，不证明类型已经加载或实例化，不包含方法源码、方法体或加密元数据解密；运行时字段偏移仅在内存查看器中另作候选推断。Mono 暂不支持。目标继续运行，读取不是冻结进程的一致性快照。

## 限制与资源生命周期

每项任务在独立 Root app_process 工作进程内执行；C++ 使用只读 /proc/PID/maps 和 /proc/PID/mem。只列出普通应用 UID，排除系统和隔离 UID；不写入目标内存、不注入库、不调用目标进程函数。SELinux 或固件拒绝读取时明确报错。

Root 启动所用原生库路径由应用 ClassLoader.findLibrary 解析，支持解压库和 APK（包括 ABI split）内的 `APK!/lib/ABI/libvulkanscope.so`。不能直接拼接 nativeLibraryDir：当前打包设置不解压原生库，该目录下可能没有实际文件。

- 原生任务期限 15 秒；Root 桥接进程设 25 秒硬期限；宿主命令期限 30 秒。
- 搜索读量最多 512 MiB，最多 1,024 个匹配；Unity 搜索最多检查 32 个元数据头候选。
- 类型导出最多 10,000 行 / 480 KiB，结果通道上限 512 KiB。达到时间、读量、输出限制或存在不可读页时标记部分结果。部分搜索无匹配不等于目标中不存在。
- 读取前后校验 PID 启动时间，防止退出 / PID 复用后混入另一进程的结果。
- 插件作用域拥有单个工作线程、取消标志和快照；重复启动不会排队。取消关闭命令通道，Root 工作进程检测输出管道断开并自行退出，另有硬期限兜底。
- memory-scanner 通过 PluginSpec 显式登记，无运行时插件依赖；宿主注入仅捕获 APK / 原生库路径值的执行器。类型事件传递请求和结果副本；不保存 Context、JNIEnv 或宿主指针。
- 卸载插件和管理宿主关闭会取消并 join 工作线程；加载回滚先注册清理再取得资源。性能 HUD 采样不与扫描任务共享线程。

## GitHub 参考

本实现借鉴格式与交互，未引入第三方注入模块或复制完整编辑器。

- [Perfare/Il2CppDumper](https://github.com/Perfare/Il2CppDumper)：参考 Il2Cpp/MetadataClass.cs 的元数据结构与版本差异。
- [Perfare/Zygisk-Il2CppDumper](https://github.com/Perfare/Zygisk-Il2CppDumper)：参考进程内 runtime API 的程序集 / 类型枚举路线，与当前跨进程元数据读取明确区分。
- [ocornut/imgui_club — Memory Editor](https://github.com/ocornut/imgui_club/tree/main/imgui_memory_editor)：参考十六进制 / ASCII 并排显示、地址导航和多类型数据解释。
- [Unity 2020 IL2CPP 头文件镜像](https://github.com/4ch12dy/il2cpp/blob/master/unity_2020_x/libil2cpp/il2cpp-class-internals.h)：核对 Il2CppClass 前缀和 FieldInfo 的 parent / offset / token 结构；不假设后续版本完整 ABI 不变。
- [ReClassNET/ReClass.NET](https://github.com/ReClassNET/ReClass.NET)：参考节点自动分析与指针追踪；没有套用 Windows RTTI 或将候选布局包装成确定的 Unity 类型。

界面继续使用 App 的 Kotlin Compose + Miuix；扫描、二进制解析、布局解释、任务边界和生命周期全部由 C++ 实现。Android Java 仅提供独立 Root 进程的加载与期限桥接。

## 验证

memory_scanner_test 覆盖数值编码、跨块 / 跨相邻映射匹配、空洞、读量限制、取消、元数据多版本布局、非法表偏移和 token、内存解释、插件卸载及不完整协议。manager_test 验证插件开关持久化和 Root 门控。

Android 真机仍需验证实际 su / SELinux、目标进程内存访问、标准 Unity IL2CPP 示例应用导出及文档保存；宿主测试不代表已验证游戏或所有 Unity 版本。
