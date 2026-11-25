# WebSocket transport host regression tests

Builds the real components/esp-ml307/src/web_socket.cc and main/harness/transport.cc against a deterministic fake TCP transport and tiny RTOS/logging stubs. JSON parsing uses the same cJSON source as ESP-IDF. No device or backend is needed.

~~~bash
cd /home/sven/proj/eink4
. /home/sven/esp/esp-idf/export.sh
cmake -S tools/ws_transport_tests -B build/ws-transport-tests
cmake --build build/ws-transport-tests -j 4
ctest --test-dir build/ws-transport-tests --output-on-failure
~~~

Alternatively pass -DCJSON_ROOT=/path/to/esp-idf/components/json/cJSON when configuring without export.sh. AddressSanitizer and UndefinedBehaviorSanitizer are enabled by default for host tests; -DENABLE_SANITIZERS=OFF disables them.

36 scenarios cover default 2048-byte frames, the separate 65535-byte legacy reassembly cap, Harness frame/message limits of 8192 bytes, UTF-8 JSON, arbitrary TCP splits/coalescing, continuation frames, interleaved sockets, Ping/Pong/Close, malformed headers/control frames, early oversize rejection, reconnect after partial input/errors, exact-length JSON with no NUL, trailing garbage, and repeated recovery. The mixed-input case sends 200 fragmented binary messages through reproducible random TCP chunk boundaries.

The fake transport is synchronous and the RTOS wait stub reads current event bits without actually waiting. Tests validate parser/state behavior, not real TCP scheduling, TLS/cellular drivers, elapsed heartbeat timeouts, Wi-Fi power management or on-device memory/stack use. OnData/OnError/OnPong execute in the receive context: callbacks must enqueue work; the owning task performs transport destruction/reconnect. Receive errors mark the connection unusable and notify OnError; its owner must dispose/reconnect the transport. This avoids destroying or joining a TLS receive task from inside itself.

No HTTP/WS hello, sync lifecycle, reconnect backoff or application commands are implemented at this stage.
