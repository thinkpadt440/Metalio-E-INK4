#pragma once
#include "client.h"
#include <memory>
class NetworkInterface;
class WebSocket;
namespace harness {
enum class ServerHealth {
    Healthy, GatewayOnly, HarnessUnavailable, NetworkUnavailable,
    Unreachable, HttpError, InvalidResponse, InvalidAddress
};
struct ServerHealthResult {
    ServerHealth state;
    int http_status = 0;
};

// Own and use this adapter from a worker task, not an LVGL callback.
class SocketIo final : public ClientIo {
public:
    explicit SocketIo(NetworkInterface& network, std::function<void(const std::string&)> on_server_date = {});
    ~SocketIo() override;
    ServerHealthResult CheckHealth(const Config& config);
    bool Connect(const Config& config,Callbacks callbacks) override;
    bool SendText(const std::string& json) override;
    bool SendBinary(const void* data, size_t size) override;
    void Ping() override;
    void Close() override;
    HttpReply Request(const std::string& method,const std::string& path,const std::string& body="") override;
private:
    HttpReply RequestHttp(const Config& config, const std::string& method, const std::string& path,
                          const std::string& body, int timeout_ms, int socket_id);
    NetworkInterface& network_;
    std::function<void(const std::string&)> on_server_date_;
    Config config_;
    std::unique_ptr<WebSocket> socket_;
};
}  // namespace harness
