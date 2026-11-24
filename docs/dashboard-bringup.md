# Metalio E-Ink4 当前固件状态与真机调试

记录当前 feature/dashboard 固件快照，方便构建、烧录和设备端验收；不替代 Harness 接口约定。

- 固件版本：0.1.1-dashboard，源码基于无名科技官方 2.0.63，ESP-IDF v5.5.4。
- 目标设备：Metalio E-Ink4，3.97 英寸 E-Ink、ESP32-S3、16MB Flash。
- 显示：面板原生 800×480；UI 逻辑坐标为 480×800 竖屏。

## 当前已实现

- 开机显示六项 Dashboard：会话数、消息数、总 Token、活跃天数、高峰时段、常用模型；周期选项为全部、30天、7天。
- Dashboard 数据目前没有连接后端，数值显示 --。DashboardScreen::UpdateMetrics() 是本地数据发布入口；周期选项目前只切换 UI 选中态，尚未发起网络查询。
- 助手入口已有最近 Session 选择页和聊天页布局，但列表、消息、模型与授权选项均为本地示例；选择或切换还未写入 Harness 服务。
- 聊天页支持音量键翻页。BOOT 长按超过约 500ms 开始本地录音，松开停止；从 Dashboard 长按会打开助手并使用固定闲聊入口。
- 录音复用 AudioService，采集 16kHz、16bit、单声道 PCM，最长 30 秒，缓冲放在 PSRAM。当前仅本地暂存并显示录音时长，不执行 ASR 或上传；新录音会复用缓冲，重启后录音丢失。
- 已有的 BOOT/音量按键、显示、音频采集和 WebSocket/HTTP 底层属于固件现有能力。Harness 的 HTTP/WS 协议适配、录音上传确认、真实历史、任务流、审批 UI 和断线恢复尚未接入；接口目标见 [接口约定 v1](device-harness-api-v1.md) 与 [完整报文示例 v1](device-harness-api-v1-examples.md)。
- 现有电源策略会在部分空闲/待机场景停止 Wi-Fi。保持 Harness WebSocket 在线和待机期间接收事件需要后续适配电源策略；当前不能依赖设备在待机时收到任务或审批通知。
- 原有设置、配网、待机等应用代码仍保留。此阶段没有替换通用音频驱动、板级驱动或电源管理模块。

## 构建与烧录

在 WSL Ubuntu 22.04 中：

~~~bash
cd /home/sven/proj/eink4
. /home/sven/esp/esp-idf/export.sh
idf.py build
~~~

设备已经刷过兼容的本项目分区和资源镜像时，可只刷应用：

~~~bash
idf.py -p /dev/ttyACM0 app-flash monitor
~~~

首次刷机，或分区表、字体/资源镜像与本地构建不一致时，完整烧录：

~~~bash
idf.py -p /dev/ttyACM0 flash monitor
~~~

build/Metalio-E-Ink4.bin 是应用镜像，不能当完整镜像写入 0x0。完整合并镜像在 build/firmware/，使用前核对生成的烧录地址。设备端口因 USB/WSL 附加状态变化时，以 ls /dev/ttyACM* 的实际结果为准。

## 当前固件真机验收

1. 开机进入 Dashboard，确认六个统计值显示 --、周期选择可操作、状态栏正常。
2. 打开助手，确认最近 Session 列表标有示例数据；进入聊天后确认示例消息和本地模型/授权选择器可显示。
3. 在聊天页使用音量加减键或盖板虚拟 PREV/NEXT 键翻页；边界页重复翻页不能越界。
4. 长按 BOOT 录音并说话，松开后确认录音时长合理；日志应出现 local recording started 和结束样本数/峰值。这里验证的是本地采集，录音尚未发送给服务端。
5. 从 Dashboard 长按 BOOT 并保持按键，检查可进入助手后开始录音；松开停止。
6. 快速松开、离开助手、进入待机时，确认本地录音能停止；等待最长录音时限，确认约 30 秒自动停止。
7. 在 Dashboard 和助手页检查 POWER 待机/唤醒及页面操作。此项只验本地设备行为，不表示网络在空闲或待机期间保持在线。
8. 检查无 panic、watchdog 或异常重启；如需看串口日志，按实际 USB/WSL 状态重新附加串口设备。

## 离线 UI 预览

按 [UI 预览说明](../tools/ui_preview/README.md) 使用实际页面源码渲染 480×800 预览。该检查可验证布局、分页边界和录音状态流程；不能验证真实墨水屏刷新、麦克风硬件、Wi-Fi/HTTP/WS、Harness 服务或休眠唤醒。

后端适配完成后，再依据两份接口文档补充 Dashboard 数据、真实 Session/消息、录音确认、任务进度、审批和断线恢复的端到端验收。
