#pragma once

#include <cstddef>
#include <memory>
#include <web_socket.h>

class NetworkInterface;
struct cJSON;

namespace harness {
constexpr size_t kMaxJsonBytes = 8192;
constexpr size_t kMaxAudioChunkBytes = 4096;

// Configure a separate Harness connection; does not connect or change old clients.
std::unique_ptr<WebSocket> CreateWebSocket(NetworkInterface& network, int connect_id);

// Parses a complete object within the supplied range. Caller owns/deletes the result.
cJSON* ParseJsonMessage(const char* data, size_t length);
}  // namespace harness
