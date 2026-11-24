# 脱机演示 UI 离线检查

在 WSL/Linux 中，使用项目实际页面源码、LVGL 与 Flash 字体包渲染 Metalio E-Ink4 的480×800竖屏逻辑画布。

~~~bash
cmake -S tools/ui_preview -B build/ui-preview-host
cmake --build build/ui-preview-host -j 8
cd build/ui-preview-host
./preview
~~~

成功输出 PASS，生成 Dashboard、两个 Session、聊天、模型/授权选择、审批、通过后执行/检查/完成、拒绝、BOOT 录音与预设回复的 PPM 图片。

覆盖不同统计周期、按会话保存设置、历史十屏上限、精确两秒审批状态机、实际盖板键名映射、重复决定、批准与拒绝、录音控制器异常路径、待机停止以及60次页面往返的定时器/电源占用释放。状态栏为预览占位值。

stubs/ 替代硬件、Flash映射与RTOS接口；页面、字体加载、录音控制器和有界PCM缓冲均使用项目代码。该检查不代表真实麦克风、触摸、墨水屏刷新或电源硬件已经验证。演示数据和回复均为脚本，与真实后端接入分支 feature/companion 分开。

演示步骤见 [脱机演示指南](../../docs/offline-demo-guide.md)。主机文件不参与ESP-IDF固件构建。
