#include "client.h"
#include "transport.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace harness {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json Parse(const std::string& text) { return Json(ParseJsonMessage(text.data(),text.size()),cJSON_Delete); }
std::string String(const cJSON* o,const char* key) {
    const auto* v=cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v)&&v->valuestring ? v->valuestring : "";
}
bool Uint(const cJSON* o,const char* key,uint32_t& out) {
    const auto* v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!cJSON_IsNumber(v)||v->valuedouble<1||v->valuedouble>UINT32_MAX||std::floor(v->valuedouble)!=v->valuedouble)return false;
    out=static_cast<uint32_t>(v->valuedouble);return true;
}
bool Id(const std::string& s) {
    if(s.empty()||s.size()>128)return false;
    for(unsigned char c:s)if(c<=32||c>=127)return false;
    return true;
}
std::string Print(const cJSON* o) {
    char* raw=cJSON_PrintUnformatted(o);if(!raw)return {};
    std::string result(raw);cJSON_free(raw);return result;
}
constexpr size_t kQueueBytes=65536,kQueueCount=32,kResourceBytes=131072,kResourceCount=64;
bool EventType(const std::string& type) {
    static const char* names[]={"run.accepted","run.state","run.progress","run.completed","run.failed","run.cancelled",
        "asr.final","message.delta","message.completed","approval.requested","approval.resolved","session.updated"};
    for(const auto* name:names)if(type==name)return true;
    return false;
}
bool Seq(const std::string& s,uint64_t& n) {
    if(s.empty()||s.size()>20||(s.size()>1&&s[0]=='0'))return false;
    n=0;for(char c:s){if(c<'0'||c>'9'||n>(UINT64_MAX-(c-'0'))/10)return false;n=n*10+(c-'0');}
    return n!=0;
}
}  // namespace
bool ValidConfig(const Config& c) {
    const size_t start=c.base_url.rfind("https://",0)==0?8:c.base_url.rfind("http://",0)==0?7:0;
    if(!start||c.base_url.size()<=start||c.base_url.size()>256||!Id(c.device_id)||
       c.firmware_version.empty()||c.firmware_version.size()>64||!Id(c.command_prefix)||c.command_prefix.size()>80)return false;
    const std::string authority=c.base_url.substr(start);
    for(unsigned char ch:authority)if(ch<=32||ch>=127||ch=='/'||ch=='?'||ch=='#'||ch=='@'||ch=='\\')return false;
    const auto colon=authority.find(':');
    const auto host=authority.substr(0,colon);
    if(host.empty())return false;
    for(char ch:host)if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='-'))return false;
    if(colon!=std::string::npos) {
        auto port=authority.substr(colon+1);unsigned n=0;if(port.empty()||port.size()>5)return false;
        for(char ch:port){if(ch<'0'||ch>'9')return false;n=n*10+(ch-'0');}
        if(n==0||n>65535)return false;
    }
    return true;
}
std::string UrlEncode(const std::string& value) {
    const char* hex="0123456789ABCDEF";std::string result;
    for(unsigned char c:value) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')result+=c;
        else{result+='%';result+=hex[c>>4];result+=hex[c&15];}
    }
    return result;
}
const char* ConnectionLabel(ConnectionState state) {
    switch(state) {
        case ConnectionState::Unconfigured:return "接收（未配置）";
        case ConnectionState::WaitingNetwork:return "接收（等待网络）";
        case ConnectionState::Connecting:return "接收（连接中）";
        case ConnectionState::Hello:return "接收（握手中）";
        case ConnectionState::Syncing:return "接收（同步中）";
        case ConnectionState::Ready:return "接收（已连接）";
        case ConnectionState::Backoff:return "接收（重连中）";
        default:return "接收（未连接）";
    }
}
Client::Client(ClientIo& io,Config config,Clock clock,
               std::function<void(const ConnectionSnapshot&)> observer)
    :io_(io),config_(std::move(config)),clock_(std::move(clock)),observer_(std::move(observer)) {
    valid_config_=ValidConfig(config_);
    state_=config_.base_url.empty()?ConnectionState::Unconfigured:
        valid_config_?ConnectionState::WaitingNetwork:ConnectionState::Unconfigured;
    if(!config_.base_url.empty()&&!valid_config_)error_="invalid_config";
    Publish();
}
Client::~Client(){Stop();}
void Client::Enqueue(uint32_t g,Input input) {
    if(g!=generation_.load())return;
    std::lock_guard<std::mutex> lock(input_mutex_);
    if(g!=generation_.load())return;
    if(input_.size()>=kQueueCount||input.text.size()>kMaxJsonBytes||input_bytes_+input.text.size()>kQueueBytes){overflow_=true;return;}
    input.received_at=clock_();
    input_bytes_+=input.text.size();input_.push_back(std::move(input));
}
void Client::Publish() {
    ConnectionSnapshot notification;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state=state_;status_.error=error_;status_.connection_id=connection_id_;
        const auto now=clock_();
        status_.retry_in_ms=state_==ConnectionState::Backoff&&retry_at_>now?static_cast<uint32_t>(retry_at_-now):0;
        if(state_==ConnectionState::Ready) {
            if(resources_dirty_) {
                status_.runs=status_.approvals=0;
                published_.clear();
                for(const auto& pair:pending_) {
                    published_.push_back(pair.second);
                    if(pair.second.kind=="run")++status_.runs;else ++status_.approvals;
                }
                resources_dirty_=false;
            }
        } else {
            published_.clear();
            status_.runs=status_.approvals=0;
            resources_dirty_=true;
        }
        notification=status_;
    }
    if(observer_)observer_(notification);
}
ConnectionSnapshot Client::Status() const {std::lock_guard<std::mutex> lock(status_mutex_);return status_;}
std::vector<Resource> Client::Resources() const {std::lock_guard<std::mutex> lock(status_mutex_);return published_;}
std::vector<std::string> Client::DrainEvents() {
    std::vector<std::string> out;if(state_!=ConnectionState::Ready)return out;
    while(!events_.empty()){out.push_back(std::move(events_.front()));events_.pop_front();}event_bytes_=0;return out;
}
void Client::Stop() {
    if(stopped_)return;
    stopped_=true;++generation_;io_.Close();
    {std::lock_guard<std::mutex> lock(input_mutex_);input_.clear();input_bytes_=0;overflow_=false;}
    pending_.clear();versions_.clear();events_.clear();event_bytes_=0;state_=ConnectionState::Stopped;connection_id_.clear();Publish();
}
void Client::Retry(const std::string& reason) {
    ++generation_;io_.Close();
    {std::lock_guard<std::mutex> lock(input_mutex_);input_.clear();input_bytes_=0;overflow_=false;}
    static const uint32_t delays[]={1000,2000,4000,8000,16000,30000};
    retry_at_=clock_()+delays[std::min<size_t>(failures_,5)];if(failures_<5)++failures_;
    state_=ConnectionState::Backoff;error_=reason.empty()?"protocol_error":reason;connection_id_.clear();sync_token_.clear();
    pending_.clear();versions_.clear();events_.clear();event_bytes_=0;last_seq_=0;reading_snapshot_=false;Publish();
}
std::string Client::NewCommand() {
    ++command_counter_;
    return config_.command_prefix+"_"+std::to_string(command_counter_);
}
bool Client::SendControl(const char* type,const std::string& command,const std::string& data) {
    Json root(cJSON_CreateObject(),cJSON_Delete);auto payload=Parse(data);
    if(!root||!payload)return false;
    cJSON_AddNumberToObject(root.get(),"v",1);cJSON_AddStringToObject(root.get(),"type",type);
    cJSON_AddStringToObject(root.get(),"command_id",command.c_str());cJSON_AddItemToObject(root.get(),"data",payload.release());
    auto body=Print(root.get());return !body.empty()&&body.size()<=kMaxJsonBytes&&io_.SendText(body);
}
void Client::Tick(bool network_ready) {
    if(stopped_||!valid_config_)return;
    if(!network_ready) {
        if(state_!=ConnectionState::WaitingNetwork) {
            ++generation_;io_.Close();
            {std::lock_guard<std::mutex> lock(input_mutex_);input_.clear();input_bytes_=0;overflow_=false;}
            state_=ConnectionState::WaitingNetwork;connection_id_.clear();pending_.clear();versions_.clear();events_.clear();event_bytes_=0;Publish();
        }
        return;
    }
    if(state_==ConnectionState::Backoff&&clock_()<retry_at_){Publish();return;}
    if(state_==ConnectionState::WaitingNetwork||state_==ConnectionState::Backoff) {
        state_=ConnectionState::Connecting;error_.clear();Publish();
        const uint32_t g=++generation_;
        Callbacks cb;
        cb.text=[this,g](std::string text){Enqueue(g,{Input::Text,std::move(text)});};
        cb.pong=[this,g](){Enqueue(g,{Input::Pong,{}});};
        cb.disconnected=[this,g](){Enqueue(g,{Input::Closed,{}});};
        cb.error=[this,g](){Enqueue(g,{Input::Error,{}});};
        if(!io_.Connect(config_,std::move(cb))){Retry("connect_failed");return;}
        state_=ConnectionState::Hello;deadline_=clock_()+10000;hello_command_=NewCommand();last_seq_=0;
        Json hello(cJSON_CreateObject(),cJSON_Delete);
        cJSON_AddStringToObject(hello.get(),"device_id",config_.device_id.c_str());
        cJSON_AddStringToObject(hello.get(),"firmware_version",config_.firmware_version.c_str());
        if(!SendControl("hello",hello_command_,Print(hello.get()))){Retry("send_failed");return;}
        Publish();
    }
    bool overflow;
    {std::lock_guard<std::mutex> lock(input_mutex_);overflow=overflow_;}
    if(overflow){Retry("receive_queue_full");return;}
    for(size_t n=0;n<kQueueCount;n++) {
        Input input;
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            if(input_.empty())break;
            input=std::move(input_.front());
            input_.pop_front();
            input_bytes_-=input.text.size();
        }
        if(input.kind==Input::Closed||input.kind==Input::Error){Retry("connection_lost");return;}
        if(input.kind==Input::Pong)last_pong_=std::max(last_pong_,input.received_at);else Handle(input.text);
        if(state_==ConnectionState::Backoff)return;
    }
    if(state_==ConnectionState::Hello&&clock_()>=deadline_){Retry("hello_timeout");return;}
    if(state_==ConnectionState::Syncing) {
        if(clock_()>=sync_deadline_){Retry("sync_timeout");return;}
        if(!reading_snapshot_&&!sync_ack_&&clock_()>=deadline_){Retry("ack_timeout");return;}
    }
    if(state_==ConnectionState::Syncing||state_==ConnectionState::Ready) {
        if(clock_()-last_pong_>=heartbeat_timeout_){Retry("heartbeat_timeout");return;}
        if(clock_()-last_ping_>=heartbeat_interval_){last_ping_=clock_();io_.Ping();}
    }
    if(state_==ConnectionState::Syncing&&reading_snapshot_)ReadSnapshotPage();
    Publish();
}
bool Client::ApplyResource(const std::string& kind,const cJSON* data) {
    if(!cJSON_IsObject(data))return false;
    Resource item;item.kind=kind;item.id=String(data,kind=="run"?"run_id":"approval_id");
    item.session_id=String(data,"session_id");
    if(!Id(item.id)||!Id(item.session_id)||!Uint(data,kind=="run"?"state_version":"approval_version",item.version))return false;
    const auto status=String(data,"status");
    const bool active=kind=="run" ? (status=="queued"||status=="transcribing"||status=="running"||status=="waiting_approval"||status=="cancelling")
                                   : status=="pending";
    const bool terminal=kind=="run" ? (status=="completed"||status=="failed"||status=="cancelled")
                                    : (status=="approved"||status=="rejected"||status=="expired"||status=="cancelled");
    if(!active&&!terminal)return false;
    const std::string key=kind+":"+item.id;
    const auto old=versions_.find(key);
    if(old!=versions_.end()&&old->second>=item.version)return true;
    if(old==versions_.end()&&versions_.size()>=256)return false;
    versions_[key]=item.version;
    resources_dirty_=true;
    if(!active){pending_.erase(key);return true;}
    item.json=Print(data);if(item.json.empty())return false;
    pending_[key]=std::move(item);
    size_t bytes=0;for(const auto& pair:pending_)bytes+=pair.second.json.size();
    return pending_.size()<=kResourceCount&&bytes<=kResourceBytes;
}
bool Client::ApplyEvent(const std::string& type,const cJSON* root) {
    const auto* data=cJSON_GetObjectItemCaseSensitive(root,"data");
    if(!cJSON_IsObject(data)||!Id(String(root,"session_id")))return false;
    if(type!="session.updated"&&!Id(String(root,"run_id")))return false;
    if(type.rfind("run.",0)==0&&type!="run.progress") {
        if(String(data,"run_id")!=String(root,"run_id")||String(data,"session_id")!=String(root,"session_id"))return false;
        return ApplyResource("run",data);
    }
    if(type.rfind("approval.",0)==0) {
        if(String(data,"run_id")!=String(root,"run_id")||String(data,"session_id")!=String(root,"session_id"))return false;
        return ApplyResource("approval",data);
    }
    return true;
}
void Client::Handle(const std::string& text) {
    auto root=Parse(text);
    if(!root||!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root.get(),"v"))||
       cJSON_GetObjectItemCaseSensitive(root.get(),"v")->valuedouble!=1){Retry("invalid_message");return;}
    auto type=String(root.get(),"type");const auto* data=cJSON_GetObjectItemCaseSensitive(root.get(),"data");
    if(type=="error"){Retry(String(cJSON_GetObjectItemCaseSensitive(root.get(),"error"),"code"));return;}
    if(state_==ConnectionState::Hello) {
        if(type!="hello"||String(root.get(),"command_id")!=hello_command_||!cJSON_IsObject(data)){Retry("invalid_hello");return;}
        connection_id_=String(data,"connection_id");sync_token_=String(data,"sync_token");
        if(!Id(connection_id_)||!Id(sync_token_)||!Uint(data,"heartbeat_interval_ms",heartbeat_interval_)||
           !Uint(data,"heartbeat_timeout_ms",heartbeat_timeout_)||heartbeat_interval_>60000||
           heartbeat_timeout_<=heartbeat_interval_||heartbeat_timeout_>120000){Retry("invalid_hello");return;}
        last_pong_=last_ping_=clock_();sync_deadline_=clock_()+120000;
        pending_.clear();versions_.clear();events_.clear();event_bytes_=0;cursor_.clear();seen_cursors_.clear();
        reading_snapshot_=true;sync_ack_=false;state_=ConnectionState::Syncing;return;
    }
    if(type=="ack") {
        if(state_!=ConnectionState::Syncing||reading_snapshot_||String(root.get(),"command_id")!=sync_command_){Retry("unexpected_ack");return;}
        if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"ok"))||String(data,"status")!="syncing"){Retry("sync_rejected");return;}
        sync_ack_=true;return;
    }
    if(type=="sync.ready") {
        if(state_!=ConnectionState::Syncing||reading_snapshot_||!sync_ack_){Retry("premature_ready");return;}
        state_=ConnectionState::Ready;failures_=0;error_.clear();return;
    }
    if(!EventType(type)||reading_snapshot_||(state_!=ConnectionState::Ready&&!sync_ack_)){Retry("unexpected_event");return;}
    uint64_t seq;
    if(String(root.get(),"connection_id")!=connection_id_||!Seq(String(root.get(),"seq"),seq)){Retry("invalid_event");return;}
    if(seq<=last_seq_)return;
    if(last_seq_==UINT64_MAX||seq!=last_seq_+1){Retry("sequence_gap");return;}
    if(!ApplyEvent(type,root.get())){Retry("invalid_resource");return;}
    if(events_.size()>=kQueueCount||event_bytes_+text.size()>kQueueBytes){Retry("event_queue_full");return;}
    events_.push_back(text);event_bytes_+=text.size();last_seq_=seq;
}
void Client::ReadSnapshotPage() {
    std::string path="/api/v1/sync?sync_token="+UrlEncode(sync_token_);
    if(!cursor_.empty())path+="&cursor="+UrlEncode(cursor_);
    auto reply=io_.Request("GET",path);
    if(clock_()>=sync_deadline_){Retry("sync_timeout");return;}
    if(!reply.error.empty()||reply.status!=200){Retry(reply.error.empty()?"http_"+std::to_string(reply.status):reply.error);return;}
    auto root=Parse(reply.body);if(!root){Retry("invalid_snapshot");return;}
    const auto* items=cJSON_GetObjectItemCaseSensitive(root.get(),"items");
    const auto* next=cJSON_GetObjectItemCaseSensitive(root.get(),"next_cursor");
    if(!cJSON_IsArray(items)||(!cJSON_IsNull(next)&&!cJSON_IsString(next))){Retry("invalid_snapshot");return;}
    const cJSON* item;
    cJSON_ArrayForEach(item,items) {
        const auto kind=String(item,"kind");
        if((kind!="run"&&kind!="approval")||!ApplyResource(kind,cJSON_GetObjectItemCaseSensitive(item,"data"))){Retry("snapshot_capacity_or_schema");return;}
    }
    if(cJSON_IsString(next)) {
        std::string value=next->valuestring?next->valuestring:"";
        if(value.empty()||value.size()>1024||seen_cursors_.size()>=128||
           std::find(seen_cursors_.begin(),seen_cursors_.end(),value)!=seen_cursors_.end()){Retry("invalid_cursor");return;}
        seen_cursors_.push_back(value);cursor_=value;return;
    }
    reading_snapshot_=false;sync_command_=NewCommand();deadline_=clock_()+10000;
    Json payload(cJSON_CreateObject(),cJSON_Delete);cJSON_AddStringToObject(payload.get(),"sync_token",sync_token_.c_str());
    if(!SendControl("sync.complete",sync_command_,Print(payload.get())))Retry("send_failed");
}
}  // namespace harness
