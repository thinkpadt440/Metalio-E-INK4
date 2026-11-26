#pragma once

#include "json.h"
#include <memory>
#include <web_socket.h>

class NetworkInterface;

namespace harness {
constexpr size_t kMaxAudioChunkBytes = 4096;

// Configure a separate Harness connection; does not connect or change old clients.
std::unique_ptr<WebSocket> CreateWebSocket(NetworkInterface& network, int connect_id);

}  // namespace harness
