#include "transport.h"

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

}  // namespace harness
