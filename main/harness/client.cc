#include "client.h"
#include "transport.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

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
std::string NormalizeServerOrigin(std::string origin) {
    constexpr char whitespace[] = " \t\r\n";
    const auto first = origin.find_first_not_of(whitespace);
    if (first == std::string::npos) return {};
    origin = origin.substr(first, origin.find_last_not_of(whitespace) - first + 1);
    const auto scheme = origin.find("://");
    if (scheme != std::string::npos && origin.size() > scheme + 3 && origin.back() == '/') {
        origin.pop_back();
    }
    return origin;
}

bool ValidServerOrigin(const std::string& origin) {
    const size_t start = origin.rfind("https://", 0) == 0 ? 8 : origin.rfind("http://", 0) == 0 ? 7 : 0;
    if (!start || origin.size() <= start || origin.size() > kMaxServerOriginLength) return false;
    const std::string authority = origin.substr(start);
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

bool ValidConfig(const Config& c) {
    return ValidServerOrigin(c.base_url) && Id(c.device_id) &&
           !c.firmware_version.empty() && c.firmware_version.size() <= 64 &&
           Id(c.command_prefix) && c.command_prefix.size() <= 80;
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
    if (ApprovalPending()) FinishApproval({}, "confirmation_unknown");
    stopped_=true;++generation_;io_.Close();
    {std::lock_guard<std::mutex> lock(input_mutex_);input_.clear();input_bytes_=0;overflow_=false;}
    pending_.clear();versions_.clear();events_.clear();event_bytes_=0;state_=ConnectionState::Stopped;connection_id_.clear();Publish();
}
void Client::Retry(const std::string& reason) {
    if (ApprovalPending()) FinishApproval({}, "confirmation_unknown");
    if (CancelPending()) FinishCancel("confirmation_unknown");
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
bool Client::SendControl(const char* type,const std::string& command,const std::string& data,const RunInfo* run) {
    Json root(cJSON_CreateObject(),cJSON_Delete);auto payload=Parse(data);
    if(!root||!payload)return false;
    cJSON_AddNumberToObject(root.get(),"v",1);cJSON_AddStringToObject(root.get(),"type",type);
    cJSON_AddStringToObject(root.get(),"command_id",command.c_str());cJSON_AddItemToObject(root.get(),"data",payload.release());
    if (run && (!cJSON_AddStringToObject(root.get(), "session_id", run->session_id.c_str()) ||
                !cJSON_AddStringToObject(root.get(), "run_id", run->id.c_str()))) return false;
    auto body=Print(root.get());return !body.empty()&&body.size()<=kMaxJsonBytes&&io_.SendText(body);
}
void Client::Tick(bool network_ready) {
    if(stopped_||!valid_config_)return;
    if(!network_ready) {
        if (ApprovalPending()) FinishApproval({}, "confirmation_unknown");
        if (CancelPending()) FinishCancel("confirmation_unknown");
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
    if (ApprovalPending() && clock_() >= approval_deadline_) FinishApproval({}, "confirmation_unknown");
    if (CancelPending() && clock_() >= cancel_deadline_) FinishCancel("confirmation_unknown");
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
bool Client::SendCancel(const RunInfo& run) {
    if (state_ != ConnectionState::Ready || CancelPending() || !Id(run.id) || !Id(run.session_id)) return false;
    cancel_command_ = NewCommand();
    cancellation_ = {run.session_id, run.id, {}};
    cancel_deadline_ = clock_() + 10000;
    if (!SendControl("run.cancel", cancel_command_, "{}", &run)) FinishCancel("confirmation_unknown");
    return true;
}

void Client::FinishCancel(std::string error) {
    cancel_command_.clear();
    cancellation_.error = std::move(error);
    cancel_result_ = std::move(cancellation_);
}

std::optional<CancelResult> Client::TakeCancelResult() {
    return std::exchange(cancel_result_, std::nullopt);
}

bool Client::HandleCancelAck(const cJSON* root) {
    if (!CancelPending() || String(root, "command_id") != cancel_command_) return false;
    const auto* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsTrue(ok) && String(data, "run_id") == cancellation_.run_id &&
        String(data, "status") == "cancelling") {
        FinishCancel("");
    } else {
        auto error = String(cJSON_GetObjectItemCaseSensitive(root, "error"), "code");
        FinishCancel(cJSON_IsFalse(ok) && !error.empty() ? std::move(error) : "confirmation_unknown");
    }
    return true;
}

bool Client::SendApproval(const ApprovalRequest& request) {
    const auto& info = request.approval;
    if (state_ != ConnectionState::Ready || ApprovalPending() || !info ||
        info->state != ApprovalState::Pending || !info->version ||
        !Id(info->id) || !Id(info->session_id) || !Id(info->run_id) ||
        (request.decision == ApprovalDecision::AllowOnce && !info->actionable)) return false;

    Json payload(cJSON_CreateObject(), cJSON_Delete);
    const char* decision = request.decision == ApprovalDecision::AllowOnce ? "approve_once" : "reject";
    if (!payload || !cJSON_AddStringToObject(payload.get(), "approval_id", info->id.c_str()) ||
        !cJSON_AddNumberToObject(payload.get(), "expected_approval_version", info->version) ||
        !cJSON_AddStringToObject(payload.get(), "decision", decision)) return false;

    RunInfo run;
    run.id = info->run_id;
    run.session_id = info->session_id;
    deciding_ = info;
    deciding_decision_ = request.decision;
    approval_command_ = NewCommand();
    approval_deadline_ = clock_() + 10000;
    if (!SendControl("approval.respond", approval_command_, Print(payload.get()), &run)) {
        FinishApproval({}, "confirmation_unknown");
    }
    return true;
}

void Client::FinishApproval(ApprovalPtr actual, std::string error) {
    approval_command_.clear();
    approval_result_ = ApprovalResult{std::move(deciding_), std::move(actual), std::move(error)};
}

std::optional<ApprovalResult> Client::TakeApprovalResult() {
    return std::exchange(approval_result_, std::nullopt);
}

bool Client::HandleApprovalAck(const cJSON* root, const std::string& text) {
    if (!ApprovalPending() || String(root, "command_id") != approval_command_) return false;
    const auto* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const auto state = String(data, "status");
    uint32_t version = 0;
    const char* expected = deciding_decision_ == ApprovalDecision::AllowOnce ? "approved" : "rejected";
    if (cJSON_IsTrue(ok) && String(data, "approval_id") == deciding_->id && state == expected &&
        Uint(data, "approval_version", version) && version > deciding_->version) {
        auto actual = std::make_shared<ApprovalInfo>();
        actual->id = deciding_->id;
        actual->session_id = deciding_->session_id;
        actual->run_id = deciding_->run_id;
        actual->version = version;
        actual->state = state == "approved" ? ApprovalState::Approved : ApprovalState::Rejected;
        FinishApproval(std::move(actual), {});
    } else if (cJSON_IsFalse(ok)) {
        const auto* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const auto* details = cJSON_GetObjectItemCaseSensitive(error, "details");
        const auto code = String(error, "code");
        const auto wire = RawJsonMember(RawJsonMember(RawJsonMember(text, "error"), "details"), "current_approval");
        FinishApproval(ReadApproval(cJSON_GetObjectItemCaseSensitive(details, "current_approval"), wire),
                       code.empty() ? "confirmation_unknown" : code);
    } else {
        FinishApproval({}, "confirmation_unknown");
    }
    return true;
}

void Client::AdvanceUpload(std::shared_ptr<UploadTicket> ticket) {
    const auto connection = state_ == ConnectionState::Ready ? connection_id_ : "";
    if (upload_.Step(io_, std::move(ticket), connection, clock_(), [this] { return NewCommand(); })) {
        Retry("audio_upload_reconnect");
    }
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
        if (state_ == ConnectionState::Ready) {
            if (HandleCancelAck(root.get()) || HandleApprovalAck(root.get(), text)) return;
            if (upload_.Ack(root.get())) Retry("audio_upload_reconnect");
            return;
        }
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
    if (CancelPending() && type.rfind("run.", 0) == 0 && type != "run.progress" &&
        String(root.get(), "run_id") == cancellation_.run_id &&
        String(root.get(), "session_id") == cancellation_.session_id) {
        const auto state = String(data, "status");
        const auto version = versions_.find("run:" + cancellation_.run_id);
        uint32_t event_version = 0;
        if (version != versions_.end() && Uint(data, "state_version", event_version) &&
            version->second == event_version &&
            (state == "cancelling" || state == "cancelled" || state == "completed" || state == "failed")) {
            FinishCancel("");
        }
    }
    if (ApprovalPending() && type == "approval.resolved" &&
        String(root.get(), "session_id") == deciding_->session_id &&
        String(root.get(), "run_id") == deciding_->run_id) {
        const auto actual = ReadApproval(data, RawJsonMember(text, "data"));
        if (actual && actual->id == deciding_->id && actual->version > deciding_->version &&
            actual->state != ApprovalState::Pending) FinishApproval(actual, {});
    }
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
