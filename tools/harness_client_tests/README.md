# Harness 客户端主机测试

测试实际 main/harness/client.cc、socket_io.cc、transport.cc 和共享 WebSocket 代码，JSON 使用 ESP-IDF 的 cJSON。无需设备或已交付的后端。

在 WSL Ubuntu 22.04 执行：

~~~bash
cd /home/sven/proj/eink4
. /home/sven/esp/esp-idf/export.sh
cmake -S tools/harness_client_tests -B build/harness-client-tests
cmake --build build/harness-client-tests -j 4
ctest --test-dir build/harness-client-tests --output-on-failure
~~~

没有加载 export.sh 时可传入 -DCJSON_ROOT=/home/sven/esp/esp-idf/components/json/cJSON。需要主机 C++17 编译器、CMake 和 Python3；默认启用 ASan/UBSan，配置时 -DENABLE_SANITIZERS=OFF 可关闭。

- harness_client：30 项确定性状态机用例。用可控时钟及假 IO 验证握手/确认期限、快照分页与原子发布、120秒过期、1–30秒退避、心跳、seq、对象版本、旧回调、断网和资源上限。
- harness_socket_io：10 项适配层用例。使用假 HTTP/TCP 验证实际请求路径/头、PATCH/PUT、X-Device-ID、无 Authorization、8192字节上限、长度异常、错误和 Close。
- harness_localhost：Python 自动启动仅绑定 127.0.0.1 的临时 HTTP/WS 服务，运行真实 Client/SocketIo/WebSocket，再清理服务。检查两页快照、需 URL 编码的 token/cursor、UTF-8 拆分帧、重复 seq、Ping/Pong，以及服务断开后重新同步。命令 ID 在两次连接间不能重复。

最后一项的 NetworkInterface 由测试专用 POSIX TCP/HTTP 实现提供，心跳时间经过加速；不运行固件的真实 HTTP/TLS/4G 驱动。RTOS stub 不模拟真实任务调度。这些检查不能代替真机的网络阻塞时间、栈/堆、电源和按键响应检查。

- harness_approval_*：验证允许、拒绝、分段展示所需的完整详情、网页限定、过期、网页竞争、版本变化、确认丢失、取消、重连、跨会话审批。使用模拟后端计数，确认只有真实允许后才执行一次示例操作。
- harness_run_*：验证网页任务、同会话并发、旧进度、取消与完成竞争、取消不支持、确认丢失，以及重连快照和重启后补查终态。主机模拟序列化恢复，实际 NVS 写入和设备重启需真机验证。
- harness_audio_*：使用模拟后端校验 PCM 长度、分块与 SHA-256，覆盖录音接受去重、超时重连、断线重传、忙时手动重试、设置冲突、明确放弃和转写顺序。主机使用已知摘要及 PCM 样本，ESP32 上的 SHA-256 和麦克风仍需真机验证。
- harness_catalog_localhost：使用 Windows 模拟工具同一份 Python 服务源码，验证 GET Dashboard 各周期、Session 和目录、固定闲聊 PUT、设置 PATCH 的版本冲突、无变化保存及 session.updated。还验证历史游标、长消息 UTF-8 分段、流式增量去重与消息完成。

- harness_standby_localhost：模拟在线待机，在当前连接接收新审批。模拟两次轮询之间完成浅睡和唤醒，验证旧连接失效并通过快照和详情恢复同一笔审批。板上的实际睡眠、网络供电和按键需真机验证。

阶段1的 36 项共享 WebSocket 回归另见 ../ws_transport_tests/README.md。
