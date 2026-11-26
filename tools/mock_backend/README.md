# Metalio E-Ink4 模拟后端

本目录可独立删除。服务使用 Windows Python 3 标准库，无需 pip、数据库、Docker、账号或鉴权。已在 Python 3.13 上运行验证。

## 启动

在 Windows 文件资源管理器里双击 start_mock_backend.bat。命令窗口保持打开时服务正在运行；按 Ctrl+C 停止。首次启动若防火墙询问，允许专用网络。

默认监听 8080 端口并绑定电脑所有网卡。窗口会显示本机检查地址和网卡 IPv4 地址。若列出多个地址，优先选择与设备同一网段的电脑 Wi-Fi/以太网地址，忽略 127.0.0.1 和 172.* 虚拟网卡地址。设备和电脑需处于同一局域网。在 idf.py menuconfig 的 Harness Companion 菜单，将服务 origin 设置为窗口给出的 http://电脑局域网IP:8080；不要填 localhost、127.0.0.1、/api/v1 或 /ws，然后重新构建/刷写固件。

端口被占用时，可在启动前设置 HARNESS_MOCK_PORT 环境变量，固件配置端口也要相同。

## 已实现接口

- WebSocket /api/v1/ws：hello、sync、sync.ready、Ping/Pong，设置成功变更后的 session.updated，以及示例消息的 message.delta 和 message.completed。
- WebSocket 录音上传支持 audio.begin、二进制 PCM、audio.end 和 accepted。校验字节数及 SHA-256，保留同一录音 ID 的任务映射，并推送示例转写与回复。
- GET /api/v1/sync 返回活动任务和待审批快照。GET /api/v1/runs/{id} 和 GET /api/v1/approvals/{id} 可查询当前状态及终态。
- GET /api/v1/dashboard/overview?range=all|30d|7d：演示统计。
- GET /api/v1/models：DeepSeek V4.1-Flash / V4.1-Pro。
- GET /api/v1/authorization-levels：请求批准 / 完全访问。
- GET /api/v1/sessions、/sessions/{id}、/sessions/{id}/messages：两条工作会话、固定闲聊和示例消息。工作会话、模型和授权目录支持 limit/cursor。历史支持 limit/before，项目规划包含需按字节分段续取的长回复。
- PUT /api/v1/devices/{device_id}/default-casual-session：为每台设备创建并复用固定闲聊。
- PATCH /api/v1/sessions/{id}/settings：按 expected_settings_version 检查并在内存中更新会话设置。版本冲突时返回最新会话，重复保存相同值保留版本。服务重启恢复初始数据。
- GET /health：从电脑浏览器检查服务。

## 手动触发一条流式回复

设备进入“项目规划”会话后，在 Windows PowerShell 执行以下命令。

~~~powershell
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8080/mock/stream'
~~~

服务分三段推送一条固定回复，并在进程内保存到消息历史。设备停在旧页时会显示“有新消息”。该入口可独立于录音触发消息事件。录音走相同的消息历史和推送通道，转写为固定示例文本，全零 PCM 会触发无语音失败。进程重启会清除任务、去重映射和录音生成的消息。

## 手动触发长任务

在设备进入“项目规划”后，用以下命令启动一个约 9 秒的网页来源示例任务。可以检查进度、取消和断线恢复。

~~~powershell
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8080/mock/run'
~~~

同一会话已有任务时返回 409。追加 ?mode=unsupported 可检查取消被拒绝，?mode=race 可检查任务恰好先完成，?mode=lost_ack 可检查只有任务状态事件的取消。追加 ?mode=lost_terminal 可检查缺少最终事件时通过 HTTP 得到取消结果。服务进程保存任务状态，设备关网或重启后可以重新查询。

## 手动触发审批

在设备进入“项目规划”后，从 PowerShell 启动一条等待审批的示例任务。

~~~powershell
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8080/mock/run?mode=approval'
~~~

| mode 参数 | 观察点 |
| --- | --- |
| approval | 同一任务可允许一次或拒绝，允许后执行模拟检查 |
| approval_long | 用音量键读完整操作，最后一页解锁允许 |
| approval_web_only | 设备保留拒绝入口，允许操作由网页处理 |
| approval_expire | 约 2.5 秒后过期 |
| approval_web_wins | 设备提交时模拟网页先拒绝 |
| approval_version | 首次提交返回新版本，要求重新核对 |
| approval_lost_ack | 省略确认和最终事件，设备用详情接口查结果 |

可以加 session_id=sess_firmware 在另一个会话触发。需要手动模拟网页决定时，可先调用 GET /api/v1/runs/{run_id} 获取 pending_approval_id，再向 /mock/approval/{approval_id}?decision=approve_once（或 reject）发送 POST。示例任务仅生成模拟状态和消息。

## 验证录音确认丢失

设置 HARNESS_MOCK_DROP_AUDIO=end_ack 后启动服务，首次接受录音时会省略 audio.end 的确认。设备应重连，使用原录音 ID 取得已接受的任务。也可设为 begin_ack、body 或 wrong_ack，分别模拟开始确认丢失、上传中断或确认中的录音 ID 错误。每次启动只注入一次。

## 设备连不上时

用电脑网卡的局域网 IPv4 配置设备；127.0.0.1 只表示访问者自身。检查电脑和设备是否连同一 Wi-Fi、启动窗口是否仍打开、Windows 网络是否为专用，以及防火墙是否允许 Python 的专用网络访问。不要将端口转发到公网。

窗口记录 HTTP 请求和 WS 握手/同步状态。
