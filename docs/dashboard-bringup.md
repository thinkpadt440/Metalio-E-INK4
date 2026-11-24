# Metalio E-Ink4 当前固件状态与真机调试

当前版本：0.1.4，基于官方2.0.63，ESP-IDF v5.5.4；3.97英寸ESP32-S3墨水屏，480×800竖屏UI。

feature/dashboard 现用于脱机演示。feature/companion 保留演示修改前的真实接入开发基线（6729fdb）。后端协议文档保持不变。

0.1.4 修复 Dashboard 会话数、消息数和活跃天数显示为 lu 的问题：数字转换不再依赖 Newlib nano 不支持的64位printf格式，sdkconfig和底层库配置保持不变。

## 当前行为

- Dashboard 显示合理的预设统计，支持全部/30天/7天切换。
- 界面不显示演示提示；模型选项为DeepSeek V4.1-Flash和DeepSeek V4.1-Pro，常用模型为后者。
- 两个会话：日常闲聊、编码助手；模型和授权设置分别保留。
- 编码助手默认完全访问，切换请求审批后两秒出现确认；盖板HOME旁左键允许、右键拒绝。
- 允许后模拟修改、检查、完成；拒绝后停止。BOOT录音后使用预设文本和回复，不调用后端、不识别实际语音。
- 录音仍使用现有AudioService；板级、网络和音频驱动没有修改。页面通过现有接口阻止自动待机，手动POWER仍保留。
- 会话列表可通过“重置测试”恢复初始状态。完整演示顺序、按键限制、模拟范围见 [脱机演示指南](offline-demo-guide.md)。

## 构建与烧录

~~~bash
cd /home/sven/proj/eink4
. /home/sven/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 app-flash monitor
~~~

已有兼容分区与字体资源时只刷应用即可。首次刷机或分区/资源不一致时使用完整flash。应用镜像不是完整镜像，不能写到0x0。USB/WSL附加和端口仍需确认。

## 验收

主机预览与脚本检查见 [UI离线检查](../tools/ui_preview/README.md)。固件构建通过只表示编译和分区尺寸通过；实体按键方向、真实录音、墨水刷新和手动待机唤醒须真机确认。真实后端接入仍按 [设备端实现计划](device-implementation-plan.md) 在 feature/companion 推进。
