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

阶段1的 36 项共享 WebSocket 回归另见 ../ws_transport_tests/README.md。当前测试覆盖连接和同步，没有把尚未实现的录音上传、业务命令确认或审批 UI 算作已完成。
