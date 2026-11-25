#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "tcp.h"

class NetworkInterface;

class WebSocket {
public:
    WebSocket(NetworkInterface* network, int connect_id);
    ~WebSocket();

    void SetHeader(const char* key, const char* value);
    // Maximum single-frame payload; existing clients keep the 2048-byte default.
    void SetReceiveBufferSize(size_t size);
    // Separate cap so legacy clients can still assemble messages from multiple 2K frames.
    void SetMessageSizeLimit(size_t size);
    bool IsConnected() const;
    bool Connect(const char* uri);
    bool Send(const std::string& data);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    void Ping();
    void Close();

    // Configure callbacks/limits before Connect. Data/error/pong callbacks run on
    // the transport receive task: enqueue work; never destroy/reconnect here.
    // OnData's range is valid only for the duration of its callback.
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnData(std::function<void(const char*, size_t, bool binary)> callback);
    void OnError(std::function<void(int)> callback);
    void OnPong(std::function<void()> callback);

    // 获取最后一次错误码
    int GetLastError();

private:
    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    bool continuation_ = false;
    size_t receive_buffer_size_ = 2048;
    size_t message_size_limit_ = 65535;
    std::string receive_buffer_;
    bool handshake_completed_ = false;
    std::atomic<bool> connected_{false};
    std::string current_message_;
    bool is_fragmented_ = false;
    bool is_binary_ = false;
    bool receive_failed_ = false;
    std::recursive_mutex receive_mutex_;

    // Mutex for sending data and replying pong
    std::mutex send_mutex_;
    
    EventGroupHandle_t handshake_event_group_;
    static const EventBits_t HANDSHAKE_SUCCESS_BIT = BIT0;
    static const EventBits_t HANDSHAKE_FAILED_BIT = BIT1;

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(int)> on_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void()> on_pong_;

    void OnTcpData(const std::string& data);
    void ResetReceiveState();
    void FailReceive(const char* reason);
    void HandleTcpDisconnected();
    bool ProcessReceiveBuffer();
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
};

#endif // WEBSOCKET_H
