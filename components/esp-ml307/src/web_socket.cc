#include "web_socket.h"
#include "network_interface.h"
#include <esp_log.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>


#define TAG "WebSocket"

static std::string base64_encode(const unsigned char *data, size_t len) {
    const char *base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    size_t i = 0;
    while (i < len) {
        size_t chunk_size = std::min((size_t)3, len - i);
        
        for (size_t j = 0; j < 3; j++) {
            char_array_3[j] = (j < chunk_size) ? data[i + j] : 0;
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (size_t j = 0; j < 4; j++) {
            if (j <= chunk_size) {
                encoded.push_back(base64_chars[char_array_4[j]]);
            } else {
                encoded.push_back('=');
            }
        }

        i += chunk_size;
    }
    return encoded;
}


WebSocket::WebSocket(NetworkInterface* network, int connect_id) : network_(network), connect_id_(connect_id) {
    handshake_event_group_ = xEventGroupCreate();
}

WebSocket::~WebSocket() {
    // Join/destroy the transport even after a receive error marked us disconnected.
    connected_ = false;
    tcp_.reset();
    if (handshake_event_group_) {
        vEventGroupDelete(handshake_event_group_);
    }
}

void WebSocket::SetHeader(const char* key, const char* value) {
    headers_[key] = value;
}

void WebSocket::SetReceiveBufferSize(size_t size) {
    std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
    if (size > 0 && size <= std::numeric_limits<size_t>::max() - 14)
        receive_buffer_size_ = size;
}

void WebSocket::SetMessageSizeLimit(size_t size) {
    std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
    if (size > 0) message_size_limit_ = size;
}

bool WebSocket::IsConnected() const {
    return connected_;
}

bool WebSocket::Connect(const char* uri) {
    if (uri == nullptr) {
        ESP_LOGE(TAG, "Invalid URI format");
        return false;
    }
    std::string uri_str(uri);
    std::string protocol, host, port, path;
    const size_t scheme_end = uri_str.find("://");
    if (scheme_end == std::string::npos) {
        ESP_LOGE(TAG, "Invalid URI format");
        return false;
    }
    protocol = uri_str.substr(0, scheme_end);
    const size_t host_start = scheme_end + 3;
    // Only search for a port in the authority, never in the path/query.
    // In particular, device_id=10:20:... is not a host:port separator.
    const size_t authority_end = uri_str.find_first_of("/?#", host_start);
    const std::string authority = uri_str.substr(host_start,
        authority_end == std::string::npos ? std::string::npos : authority_end - host_start);
    const size_t port_separator = authority.find(':');
    host = authority.substr(0, port_separator);
    port = port_separator == std::string::npos
        ? ((protocol == "wss" || protocol == "https") ? "443" : "80")
        : authority.substr(port_separator + 1);
    if (host.empty() || port.empty()) {
        ESP_LOGE(TAG, "Invalid URI host or port");
        return false;
    }
    // Validate without throwing std::stoi on malformed configuration.
    int port_number = 0;
    for (char digit : port) {
        if (digit < '0' || digit > '9' || port_number > (65535 - (digit - '0')) / 10) {
            ESP_LOGE(TAG, "Invalid URI port");
            return false;
        }
        port_number = port_number * 10 + (digit - '0');
    }
    if (port_number == 0) {
        ESP_LOGE(TAG, "Invalid URI port");
        return false;
    }
    path = authority_end == std::string::npos ? "/" : uri_str.substr(authority_end);
    const size_t fragment = path.find('#');
    if (fragment != std::string::npos) { path.erase(fragment); }
    if (path.empty() || path.front() == '?') { path.insert(0, "/"); }

    ESP_LOGD(TAG, "Connecting to %s://%s:%s%s", protocol.c_str(), host.c_str(), port.c_str(), path.c_str());

    // 设置 WebSocket 特定的头部
    SetHeader("Upgrade", "websocket");
    SetHeader("Connection", "Upgrade");
    SetHeader("Sec-WebSocket-Version", "13");
    
    // 生成随机的 Sec-WebSocket-Key
    char key[25];
    for (int i = 0; i < 16; ++i) {
        key[i] = rand() % 256;
    }
    std::string base64_key = base64_encode(reinterpret_cast<const unsigned char*>(key), 16);
    SetHeader("Sec-WebSocket-Key", base64_key.c_str());

    // Finish the previous receive task before reusing any parser state.
    connected_ = false;
    tcp_.reset();
    {
        std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
        ResetReceiveState();
        handshake_completed_ = false;
        receive_failed_ = false;
        continuation_ = false;
    }
    xEventGroupClearBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT);
    if (protocol == "wss" || protocol == "https") {
        tcp_ = network_->CreateSsl(connect_id_);
    } else {
        tcp_ = network_->CreateTcp(connect_id_);
    }
    if (!tcp_) return false;

    // Subscribe before sending: a local/fast server may reply immediately.
    tcp_->OnStream([this](const std::string& data) { OnTcpData(data); });
    tcp_->OnDisconnected([this]() { HandleTcpDisconnected(); });
    if (!tcp_->Connect(host, port_number)) {
        ESP_LOGE(TAG, "Failed to connect to server");
        return false;
    }
    std::string request = "GET " + path + " HTTP/1.1\r\n";
    if (headers_.find("Host") == headers_.end()) {
        request += "Host: " + host + ":" + port + "\r\n";
    }
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    request += "\r\n";
    if (tcp_->Send(request) < 0) {
        std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
        FailReceive("Failed to send WebSocket handshake request");
        return false;
    }

    // 等待握手完成，超时时间10秒
    EventBits_t bits = xEventGroupWaitBits(
        handshake_event_group_,
        HANDSHAKE_SUCCESS_BIT | HANDSHAKE_FAILED_BIT,
        pdFALSE,  // 不清除事件位
        pdFALSE,  // 等待任意一个事件位
        pdMS_TO_TICKS(10000)  // 10秒超时
    );

    if ((bits & HANDSHAKE_SUCCESS_BIT) && !(bits & HANDSHAKE_FAILED_BIT) && connected_) {
        if (on_connected_) {
            on_connected_();
        }
        return true;
    } else if (bits & HANDSHAKE_FAILED_BIT) {
        ESP_LOGE(TAG, "WebSocket handshake failed");
        return false;  // FailReceive already reported the error.
    } else {
        std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
        FailReceive("WebSocket handshake timeout");
        return false;
    }
}


bool WebSocket::Send(const std::string& data) {
    return Send(data.data(), data.size(), false);
}

bool WebSocket::Send(const void* data, size_t len, bool binary, bool fin) {
    if (!connected_ || !tcp_ || (len != 0 && data == nullptr)) return false;
    if (len > 65535) {
        ESP_LOGE(TAG, "Data too large, maximum supported size is 65535 bytes");
        return false;
    }

    std::string frame;
    frame.reserve(len + 8);  // 最大可能的帧大小（2字节帧头 + 2字节长度 + 4字节mask）

    // 第一个字节：FIN 位 + 操作码
    uint8_t first_byte = (fin ? 0x80 : 0x00);
    if (binary) {
        first_byte |= 0x02;  // 二进制帧
    } else if (!continuation_) {
        first_byte |= 0x01;  // 文本帧
    } // 否则，操作码为0（延续帧）

    frame.push_back(static_cast<char>(first_byte));

    // 第二个字节：MASK 位 + 有效载荷长度
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));  // 设置MASK位
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));  // 设置MASK位
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    }

    // 生成随机的4字节mask
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并mask处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }

    // 更新continuation_状态
    continuation_ = !fin;

    // 发送帧
    std::lock_guard<std::mutex> lock(send_mutex_);
    return tcp_->Send(frame) >= 0;
}

void WebSocket::Ping() {
    SendControlFrame(0x9, nullptr, 0);
}

void WebSocket::Close() {
    std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
    if (connected_) SendControlFrame(0x8, nullptr, 0);
    HandleTcpDisconnected();
}

void WebSocket::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void WebSocket::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void WebSocket::OnData(std::function<void(const char*, size_t, bool binary)> callback) {
    on_data_ = callback;
}

void WebSocket::OnError(std::function<void(int)> callback) {
    on_error_ = callback;
}

void WebSocket::OnPong(std::function<void()> callback) {
    on_pong_ = std::move(callback);
}

int WebSocket::GetLastError() {
    if (tcp_) {
        return tcp_->GetLastError();
    }
    return 0;  // No error if TCP connection doesn't exist
}

void WebSocket::ResetReceiveState() {
    receive_buffer_.clear();
    current_message_.clear();
    is_fragmented_ = false;
    is_binary_ = false;
}

void WebSocket::FailReceive(const char* reason) {
    if (receive_failed_) return;
    ESP_LOGE(TAG, "%s", reason);
    receive_failed_ = true;
    connected_ = false;
    ResetReceiveState();
    xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
    if (on_error_) on_error_(-1);
}

void WebSocket::HandleTcpDisconnected() {
    std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
    const bool was_connected = connected_.exchange(false);
    receive_failed_ = true;
    ResetReceiveState();
    if (!handshake_completed_)
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
    if (was_connected && on_disconnected_) on_disconnected_();
}

void WebSocket::OnTcpData(const std::string& data) {
    std::lock_guard<std::recursive_mutex> lock(receive_mutex_);
    if (receive_failed_) return;
    // Consume coalesced TCP reads incrementally instead of buffering an unbounded read.
    constexpr size_t kHandshakeLimit = 8192;
    const size_t buffer_limit = std::max(kHandshakeLimit, receive_buffer_size_ + 14);
    size_t offset = 0;
    while (offset < data.size() && !receive_failed_) {
        if (receive_buffer_.size() >= buffer_limit) {
            FailReceive("WebSocket receive buffer overflow");
            return;
        }
        const size_t count = std::min(data.size() - offset, buffer_limit - receive_buffer_.size());
        receive_buffer_.append(data.data() + offset, count);
        offset += count;
        if (!ProcessReceiveBuffer()) return;
    }
}

bool WebSocket::ProcessReceiveBuffer() {
    if (!handshake_completed_) {
        constexpr size_t kHandshakeLimit = 8192;
        const size_t end = receive_buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (receive_buffer_.size() >= kHandshakeLimit)
                FailReceive("WebSocket handshake headers too large");
            return !receive_failed_;
        }
        if (end + 4 > kHandshakeLimit ||
            receive_buffer_.compare(0, 13, "HTTP/1.1 101 ") != 0) {
            FailReceive("WebSocket handshake failed");
            return false;
        }
        receive_buffer_.erase(0, end + 4);
        handshake_completed_ = true;
        connected_ = true;
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_SUCCESS_BIT);
    }
    while (!receive_failed_) {
        const size_t size = receive_buffer_.size();
        if (size < 2) return true;
        const auto* bytes = reinterpret_cast<const unsigned char*>(receive_buffer_.data());
        const unsigned opcode = bytes[0] & 0x0f;
        const bool fin = (bytes[0] & 0x80) != 0;
        const bool control = opcode >= 8;
        if ((bytes[0] & 0x70) || (bytes[1] & 0x80) ||
            (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)) {
            FailReceive("Invalid WebSocket frame header");
            return false;
        }
        uint64_t length = bytes[1] & 0x7f;
        size_t header = 2;
        if (length == 126) {
            if (size < 4) return true;
            length = (static_cast<uint64_t>(bytes[2]) << 8) | bytes[3];
            header = 4;
        } else if (length == 127) {
            if (size < 10) return true;
            if (bytes[2] & 0x80) {
                FailReceive("Invalid WebSocket 64-bit length");
                return false;
            }
            length = 0;
            for (size_t i = 2; i < 10; ++i) length = (length << 8) | bytes[i];
            header = 10;
        }
        // Check declared lengths before waiting for/allocating the payload.
        if ((control && (!fin || length > 125 || (opcode == 8 && length == 1))) ||
            (!control && length > receive_buffer_size_)) {
            FailReceive("WebSocket frame exceeds receive limit");
            return false;
        }
        if (!control) {
            if ((opcode == 0 && !is_fragmented_) || (opcode != 0 && is_fragmented_)) {
                FailReceive("Invalid WebSocket fragmentation");
                return false;
            }
            const size_t accumulated = opcode == 0 ? current_message_.size() : 0;
            if (accumulated > message_size_limit_ || length > message_size_limit_ - accumulated) {
                FailReceive("WebSocket message exceeds receive limit");
                return false;
            }
        }
        if (length > size - header) return true;
        const size_t payload_length = static_cast<size_t>(length);
        // Remove each frame before callbacks, which may call Close().
        if (control) {
            std::string payload(receive_buffer_.data() + header, payload_length);
            receive_buffer_.erase(0, header + payload_length);
            if (opcode == 8) {
                SendControlFrame(8, payload.data(), payload.size());
                HandleTcpDisconnected();
                return false;
            }
            if (opcode == 9 && !SendControlFrame(10, payload.data(), payload.size())) {
                FailReceive("WebSocket pong send failed");
                return false;
            }
            if (opcode == 10 && on_pong_) on_pong_();
        } else {
            if (opcode != 0) {
                current_message_.clear();
                is_binary_ = opcode == 2;
            }
            current_message_.append(receive_buffer_.data() + header, payload_length);
            receive_buffer_.erase(0, header + payload_length);
            is_fragmented_ = !fin;
            if (fin) {
                std::string message = std::move(current_message_);
                current_message_.clear();
                const bool binary = is_binary_;
                if (on_data_) on_data_(message.data(), message.size(), binary);
            }
        }
    }
    return false;
}

bool WebSocket::SendControlFrame(uint8_t opcode, const void* data, size_t len) {
    if (!connected_ || !tcp_ || (len != 0 && data == nullptr)) return false;
    if (len > 125) {
        ESP_LOGE(TAG, "控制帧有效载荷过大");
        return false;
    }

    std::string frame;
    frame.reserve(len + 6);  // 帧头 + 掩码 + 有效载荷

    // 第一个字节：FIN 位 + 操作码
    frame.push_back(static_cast<char>(0x80 | opcode));

    // 第二个字节：MASK 位 + 有效载荷长度
    frame.push_back(static_cast<char>(0x80 | len));

    // 生成随机的4字节掩码
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);

    // 添加并掩码处理有效载荷
    const uint8_t* payload = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }

    // 发送帧
    std::lock_guard<std::mutex> lock(send_mutex_);
    return tcp_->Send(frame) >= 0;
}