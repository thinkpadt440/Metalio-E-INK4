# Companion 阶段2：连接与同步实现、验证记录

日期：2026-11-24。分支：feature/companion。协议依据：[接口约定 v1](device-harness-api-v1.md)、[完整报文示例 v1](device-harness-api-v1-examples.md)。

## 已实现

- 新增 main/harness/client：与硬件/UI 分离的连接状态机。网络接收回调只投递有界队列，由独立工作任务执行解析、HTTP、重连和状态变更。
- 按 hello → GET /sync 分页 → sync.complete → ack → 缓存事件 → sync.ready 恢复。同步结果在 ready 前不对业务层公开；失败丢弃本轮快照，重新握手，不发布半份结果。
- 维护 connection_id、连续十进制字符串 seq、唯一 command_id，以及活动 Run/待审批对象的版本。重复 seq 忽略、跳号重同步；终态版本保留，避免旧事件把任务恢复成运行中。
- 心跳按 hello 参数发送 Ping，按 Pong 接收时间检查超时；连接失败后按 1/2/4/8/16/30 秒退避。旧连接回调不能污染新连接。
- 新增 SocketIo，复用现有 NetworkInterface 工厂。独立 WebSocket 使用 8192 字节帧/完整消息上限；HTTP 统一请求头、状态码、JSON 类型、响应长度、截断和读失败处理，所有退出路径关闭请求。
- 启动时读取编译配置；没有服务地址则不建立连接。助手聊天标题显示未配置、等待网络、连接中、握手中、同步中、已连接或重连中，网络操作不在 LVGL 回调中执行。

本阶段只发送 hello 和 sync.complete 两类控制命令，其等待超时为 10 秒；录音、审批和取消等业务命令及其确认仍按后续阶段实现。HTTP 封装支持 GET/PUT/PATCH，目前实际调用的是 GET /sync。

## 配置后端地址

在 WSL 执行：

~~~bash
cd /home/sven/proj/eink4
. /home/sven/esp/esp-idf/export.sh
idf.py menuconfig
~~~

打开 Harness Companion：

| 配置 | 填写方式 |
| --- | --- |
| Fixed HTTP(S) server origin | 后端实际地址，例如 http://实际主机:实际端口；不要附加 /api/v1 或 /ws |
| Device identifier | 可留空；运行时生成 metalio_ 加去除冒号的设备 MAC |

设备自动派生 HTTP /api/v1/... 和 WS /api/v1/ws 地址，HTTPS 对应 WSS。当前校验支持域名/IPv4，可带端口，暂不支持 IPv6 字面量。HTTP 和 WS 均携带 X-Device-ID，不发送 Authorization。配置后重新执行 idf.py build。

仓库中的服务地址仍为空；SSH 主机地址不代表已交付的 Harness 接口地址。HTTPS 复用固件现有 TLS 能力；本阶段本地集成测试使用 HTTP/WS。

## 资源和异常边界

| 项目 | 当前限制与处理 |
| --- | --- |
| 单个 JSON 响应/完整 WS 文本 | 最多 8192 字节 |
| 接收队列、待消费业务事件队列 | 各最多 32 项、合计正文各 64KiB |
| 活动 Run + 待审批缓存 | 最多 64 个、JSON 正文合计 128KiB |
| 对象版本记录 | 每轮连接最多 256 个对象，含终态记录 |
| 同步游标 | 单个最多 1024 字节，检测重复；最多接受 128 次非空续页游标 |
| 同步总期限 | 收到 hello 后 120 秒 |
| HTTP | 设置 10 秒请求超时并按剩余读取预算收紧；每次新建请求并关闭 |

达到队列、对象或同步边界会明确失败并重新同步，不静默丢失事件。后端持续返回超出容量的快照时会持续重试；这些是当前设备实现容量，不能视为无限历史缓存。

底层 TCP 建连/HTTP 响应头等待仍使用现有驱动的阻塞方式，因此 10 秒不是整个网络调用的绝对墙钟上限。它们运行在独立工作任务中；真实网络异常下的耗时、内存、任务栈和按键响应仍需真机验证。

## 电源与实现范围

配置有效服务地址时，工作任务持有现有 PowerNeed::UiKeepNet，并在停止时释放；没有修改共享 PowerPolicy 默认规则。进入现有待机页时关闭本轮连接、等待网络，唤醒后重新连接同步。本阶段不保证待机持续在线接收审批；整机在线/续航方案属于阶段8。

当前已经保存活动任务/审批快照，接收并排队协议事件，但尚未把它们渲染成真实业务页面。暂未接入业务消费的事件队列满时会重新同步；后续阶段需及时消费，不能把现阶段固件当成已完成的长任务终端。

Dashboard、Session 列表、模型/授权和聊天内容仍为原分支本地示例。BOOT 仍仅本地采集 PCM；没有上传/ASR，旧缓冲保护也留到阶段5。标题“已连接”只表示通信与同步已就绪，不表示这些业务已接入。

## 验证结果

| 验证 | 结果 |
| --- | --- |
| 客户端状态机 | 30 项通过，覆盖握手、同步原子性、分页、过期、退避、Pong、版本、序号、旧回调和队列溢出 |
| HTTP/WS 适配层 | 10 项通过，覆盖请求头、无鉴权、8KB 边界、截断、异常和资源关闭 |
| localhost 模拟后端 | 通过：真实 WS 解析器/客户端/适配层，双页快照、UTF-8 分片、重复事件、Pong、断线后第二次同步 |
| 阶段1 WebSocket 回归 | 36 项通过 |
| 480×800 UI 预览 | 通过，含连接状态切换与阶段1数值回归 |
| ESP-IDF 固件构建 | 通过；应用 0x35a480 字节，5MiB 应用分区剩余约 33% |

主机 C++ 测试启用了 AddressSanitizer 和 UndefinedBehaviorSanitizer。模拟后端使用 POSIX TCP/HTTP 测试实现及加速时钟；没有替代 ESP32 的真实 HTTP/TLS、Wi-Fi/4G 调度、电源、麦克风或墨水屏验收。复现命令见 [客户端测试说明](../tools/harness_client_tests/README.md) 与 [WebSocket 回归说明](../tools/ws_transport_tests/README.md)。

本阶段没有烧录设备，没有提交或推送代码；feature/dashboard 分支指向未变。板级引脚、显示/音频驱动、分区表和 PowerPolicy 源码未改。sdkconfig 仅新增两个空的 Harness 配置项；阶段1对共享 WS 的改动继续保留。

新增了可双击启动的本机模拟服务，覆盖当前 hello/sync 和下一阶段目录、Dashboard、Session、消息、设置接口；见 [模拟后端使用说明](../tools/mock_backend/README.md)。Windows 本机 Python 上的 HTTP 路由、设置版本校验和 WebSocket hello/sync.ready 共 3 项冒烟测试通过。\n\n下一阶段：接入 Dashboard、最近 Session、固定闲聊入口、模型和授权目录及配置变更；后端/真机可用后再补通信实测。
