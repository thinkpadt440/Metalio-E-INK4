#pragma once
#include "client.h"
#include <memory>
class NetworkInterface;
class WebSocket;
namespace harness {
// Own and use this adapter from a worker task, not an LVGL callback.
class SocketIo final : public ClientIo {
public:
    explicit SocketIo(NetworkInterface& network);
    ~SocketIo() override;
    bool Connect(const Config& config,Callbacks callbacks) override;
    bool SendText(const std::string& json) override;
    void Ping() override;
    void Close() override;
    HttpReply Request(const std::string& method,const std::string& path,const std::string& body="") override;
private:
    NetworkInterface& network_;
    Config config_;
    std::unique_ptr<WebSocket> socket_;
};
}  // namespace harness
