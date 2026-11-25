# Metalio E-Ink4 模拟后端

本目录可独立删除。服务使用 Windows Python 3 标准库，无需 pip、数据库、Docker、账号或鉴权。已在 Python 3.13 上运行验证。

## 启动

在 Windows 文件资源管理器里双击 start_mock_backend.bat。命令窗口保持打开时服务正在运行；按 Ctrl+C 停止。首次启动若防火墙询问，允许专用网络。

默认监听 8080 端口并绑定电脑所有网卡。窗口会显示本机检查地址和网卡 IPv4 地址。若列出多个地址，优先选择与设备同一网段的电脑 Wi-Fi/以太网地址，忽略 127.0.0.1 和 172.* 虚拟网卡地址。设备和电脑需处于同一局域网。在 idf.py menuconfig 的 Harness Companion 菜单，将服务 origin 设置为窗口给出的 http://电脑局域网IP:8080；不要填 localhost、127.0.0.1、/api/v1 或 /ws，然后重新构建/刷写固件。

端口被占用时，可在启动前设置 HARNESS_MOCK_PORT 环境变量，固件配置端口也要相同。

## 已实现接口

- WebSocket /api/v1/ws：hello、sync、sync.ready 与 Ping/Pong。
- GET /api/v1/sync：空的活动任务/审批快照。
- GET /api/v1/dashboard/overview?range=all|30d|7d：演示统计。
- GET /api/v1/models：DeepSeek V4.1-Flash / V4.1-Pro。
- GET /api/v1/authorization-levels：请求批准 / 完全访问。
- GET /api/v1/sessions、/sessions/{id}、/sessions/{id}/messages：两条工作会话、固定闲聊和示例消息。
- PUT /api/v1/devices/{device_id}/default-casual-session：复用固定闲聊。
- PATCH /api/v1/sessions/{id}/settings：按 expected_settings_version 检查并在内存中更新会话设置。服务重启恢复初始数据。
- GET /health：从电脑浏览器检查服务。

还未模拟录音上传、ASR、任务进度与审批；可在相应固件功能实现时继续扩展。

## 设备连不上时

用电脑网卡的局域网 IPv4 配置设备；127.0.0.1 只表示访问者自身。检查电脑和设备是否连同一 Wi-Fi、启动窗口是否仍打开、Windows 网络是否为专用，以及防火墙是否允许 Python 的专用网络访问。不要将端口转发到公网。

窗口记录 HTTP 请求和 WS 握手/同步状态。
