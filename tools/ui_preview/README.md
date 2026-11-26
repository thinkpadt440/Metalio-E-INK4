# 自定义 UI 离线检查

在 WSL/Linux 中，使用项目实际页面源码、LVGL 与 Flash 字体包渲染 Metalio E-Ink4 的
480×800 竖屏逻辑画布。不需要设备或显示服务器；需要 CMake 和主机 C/C++ 编译器。

运行命令：
. /home/sven/esp/esp-idf/export.sh
cmake -S tools/ui_preview -B build/ui-preview-host
cmake --build build/ui-preview-host -j 8
cd build/ui-preview-host
./preview

成功时输出 PASS，并生成 dashboard、最近 Session 列表、聊天页、模型选择器和 BOOT
录音状态的 PPM 图片。检查包括六项统计与空值、周期切换、会话和模型目录分页、不可用选项、设置冲突与重新读取、固定闲聊、长消息分页、回看时的增量去重和新消息提示、BOOT 录音流程、待确认时阻止覆盖、重试与放弃入口、任务进度与取消确认、审批详情翻页、审批离线禁用、网页限定审批及 60 次导航后的定时器清理。系统状态栏为预览占位值。

离线测试不能验证墨水屏波形、局刷、触摸硬件、充电、网络连接和休眠唤醒。目录与配置由预览测试注入，固件使用 Harness v1 接口。首页没有统计数据时显示 --。HOME 默认分发和硬件按键仍需真机检查。

stubs/ 只替代硬件、Flash 映射与 RTOS 接口；字体加载及页面实现直接编译项目源码。
主机渲染文件不会参与 ESP-IDF 固件构建。

PTT 控制器直接编译 harness_voice.cc，主机模拟任务队列、AudioService 和电源活动计数。
另使用真实的 LocalRecordingBuffer 检查双声道抽取麦克风、容量上限、极值峰值和边界哨兵。
覆盖初始化前/中松手、启动/输入失败、30 秒上限、离页和待机停止；不代表真实 I2S 采集测试通过。
Dashboard 样例图使用测试输入，固件无统计数据时仍显示待接入状态。