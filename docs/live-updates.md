# 流体云 / Live Updates

性能插件设置中的「流体云 / 实时活动」默认关闭。开启后，运行 HUD 时可从此设置页或常驻通知点击「收起 HUD」。采集继续；通知卡片提供「展开 HUD」和「停止」。系统胶囊的点击行为（展开卡片等）由固件决定，使用卡片上的展开按钮恢复 HUD。每次新启动监测默认展开。

Android 16+ 使用公共 Live Updates 接口请求系统提升，支持实现该接口的 ColorOS 流体云。通知为标准 BigTextStyle、ongoing、低重要性渠道，声明 POST_PROMOTED_NOTIFICATIONS，使用公开 extra `android.requestPromotedOngoing`（36.1，兼容 OEM 回移）和 API 36 shortCriticalText；不伪装通话、导航或媒体播放。不使用 ColorOS 私有 SDK、服务端推送、Root 修改白名单或模拟胶囊。

插件设置显示系统返回的提升标志、允许申请但尚未提升、禁止通知、不支持等状态。允许提升不等于实际显示胶囊。固件可以附加准入条件，用户也可将实时活动降级。没有提升时保留普通通知卡片；Android 15 及以下只有该回退方式。入口「系统通知设置」优先打开实时活动授权页，没有对应页面则打开应用通知设置。

C++ Session 持有临时收起状态和持久化开关（配置版本 7，旧配置默认关闭）。Java 仅将 C++ 已格式化快照交给通知系统，不读取硬件。摘要来自与 ImGui 相同的快照及过期策略，保留 GPU 单位；延迟采样不再将旧 FPS 放入短胶囊标题。通知按原生采样发布序号更新，只有收起时请求提升；系统可能对高频通知限流。收起暂停 ImGui 提交，Root SurfaceControl hide / show 与普通 WindowManager alpha 分别控制显示，采集线程不停止。

通知或渠道被禁用、通知发布失败、关闭插件开关时恢复 HUD。通知划掉即停止会话，不循环重发。过期通知操作不会启动新会话。停止和异常退出使用原有线程取消 / join / Surface 释放路径。

参考：
- [Android Live Updates](https://developer.android.com/develop/ui/views/notifications/live-update)
- [公开通知 extra](https://developer.android.com/reference/androidx/core/app/NotificationCompat#EXTRA_REQUEST_PROMOTED_ONGOING)
- [Android 16 与 OPPO / OnePlus Live Alerts](https://blog.google/products-and-platforms/platforms/android/android-16/)

验证包含原生配置迁移、通知操作与版本回退、Android Debug/Release 构建。尚无连接的真机；需要在 ColorOS 16 上启用应用通知与流体云后确认系统实际提升、横竖屏、普通与 Root 模式的收起恢复，以及锁屏 / 权限撤销行为。
