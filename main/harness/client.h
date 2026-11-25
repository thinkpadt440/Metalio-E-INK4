#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct cJSON;

namespace harness {
enum class ConnectionState { Unconfigured, WaitingNetwork, Connecting, Hello, Syncing, Ready, Backoff, Stopped };
const char* ConnectionLabel(ConnectionState state);
struct Config {
    std::string base_url;
    std::string device_id;
    std::string firmware_version;
    std::string command_prefix;
};
struct HttpReply { int status=0; std::string body; std::string error; };
struct Callbacks {
    std::function<void(std::string)> text;
    std::function<void()> pong;
    std::function<void()> disconnected;
    std::function<void()> error;
};
class ClientIo {
public:
    virtual ~ClientIo()=default;
    virtual bool Connect(const Config& config, Callbacks callbacks)=0;
    virtual bool SendText(const std::string& json)=0;
    virtual void Ping()=0;
    virtual void Close()=0;
    virtual HttpReply Request(const std::string& method,const std::string& path,
                              const std::string& body="")=0;
};
struct Resource {
    std::string kind,id,session_id,json;
    uint32_t version=0;
};
struct ConnectionSnapshot {
    ConnectionState state=ConnectionState::Unconfigured;
    std::string error;
    std::string connection_id;
    size_t runs=0,approvals=0;
    uint32_t retry_in_ms=0;
};
// Tick/Stop/DrainEvents are owner-task operations. Callbacks only enqueue bounded data;
// UI may read Status/Resources from any thread. No LVGL or hardware calls here.
class Client {
public:
    using Clock=std::function<uint64_t()>;
    Client(ClientIo& io,Config config,Clock clock,
           std::function<void(const ConnectionSnapshot&)> observer={});
    ~Client();
    void Tick(bool network_ready);
    void Stop();
    ConnectionSnapshot Status() const;
    std::vector<Resource> Resources() const;
    std::vector<std::string> DrainEvents();
    bool Ready() const { return Status().state==ConnectionState::Ready; }
private:
    struct Input { enum Kind { Text,Pong,Closed,Error } kind; std::string text; uint64_t received_at=0; };
    ClientIo& io_;
    Config config_;
    Clock clock_;
    std::function<void(const ConnectionSnapshot&)> observer_;
    bool resources_dirty_=true;
    mutable std::mutex status_mutex_;
    ConnectionSnapshot status_;
    std::vector<Resource> published_;
    std::mutex input_mutex_;
    std::deque<Input> input_;
    size_t input_bytes_=0;
    bool overflow_=false;
    std::atomic<uint32_t> generation_{0};
    ConnectionState state_=ConnectionState::Unconfigured;
    bool valid_config_=false,stopped_=false;
    std::string error_,connection_id_,sync_token_,cursor_,hello_command_,sync_command_;
    std::map<std::string,Resource> pending_;
    std::map<std::string,uint32_t> versions_;
    std::deque<std::string> events_;
    size_t event_bytes_=0;
    std::vector<std::string> seen_cursors_;
    uint64_t deadline_=0,sync_deadline_=0,retry_at_=0,last_pong_=0,last_ping_=0,last_seq_=0;
    uint32_t heartbeat_interval_=20000,heartbeat_timeout_=60000,failures_=0,command_counter_=0;
    bool sync_ack_=false,reading_snapshot_=false;
    void Enqueue(uint32_t generation,Input input);
    void Publish();
    void Retry(const std::string& reason);
    void Handle(const std::string& text);
    void ReadSnapshotPage();
    bool ApplyResource(const std::string& kind,const cJSON* data);
    bool ApplyEvent(const std::string& type,const cJSON* root);
    bool SendControl(const char* type,const std::string& command,const std::string& data);
    std::string NewCommand();
};
bool ValidConfig(const Config& config);
std::string UrlEncode(const std::string& value);
}  // namespace harness
