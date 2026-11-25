#include "transport.h"

#include <cstring>
#include <cJSON.h>
#include <network_interface.h>
#include <web_socket.h>

namespace harness {
std::unique_ptr<WebSocket> CreateWebSocket(NetworkInterface& network, int connect_id) {
    auto socket = network.CreateWebSocket(connect_id);
    if (socket) {
        socket->SetReceiveBufferSize(kMaxJsonBytes);
        socket->SetMessageSizeLimit(kMaxJsonBytes);
    }
    return socket;
}

cJSON* ParseJsonMessage(const char* data, size_t length) {
    if (data == nullptr || length == 0 || length > kMaxJsonBytes ||
        std::memchr(data, '\0', length) != nullptr) return nullptr;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(data, length, &end, false);
    if (!root) return nullptr;
    while (end < data + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != data + length || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}
}  // namespace harness
