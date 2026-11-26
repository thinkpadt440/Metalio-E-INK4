#include "harness/catalog.h"
#include "harness/client.h"
#include "boards/common/power_policy/power_policy.h"
#include "harness/socket_io.h"
#include "harness/json.h"
#include <cJSON.h>
#include <network_interface.h>
#include <web_socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
using namespace harness;
static void Need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static int Dial(const std::string& host,int port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);
    if(inet_pton(AF_INET,host.c_str(),&addr.sin_addr)!=1||connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0){close(fd);return -1;}
    return fd;
}
static bool SendAll(int fd,const std::string& data){
    size_t pos=0;while(pos<data.size()){int n=send(fd,data.data()+pos,data.size()-pos,MSG_NOSIGNAL);if(n<=0)return false;pos+=n;}return true;
}
class LocalTcp:public Tcp {
    int fd_=-1;LocalTcp*& slot_;
public:
    explicit LocalTcp(LocalTcp*& slot):slot_(slot){slot_=this;}
    ~LocalTcp()override{Disconnect();if(slot_==this)slot_=nullptr;}
    bool Connect(const std::string& host,int port)override{fd_=Dial(host,port);connected_=fd_>=0;return connected_;}
    void Disconnect()override{if(fd_>=0){close(fd_);fd_=-1;}connected_=false;}
    int GetLastError()override{return 0;}
    int Send(const std::string& data)override{
        if(!SendAll(fd_,data))return -1;
        if(data.rfind("GET ",0)==0) {
            std::string response;char bytes[1500];
            while(response.find("\r\n\r\n")==std::string::npos){
                int n=recv(fd_,bytes,sizeof(bytes),0);if(n<=0)return -1;response.append(bytes,n);
                if(response.size()>8192)return -1;
            }
            if(stream_callback_)stream_callback_(response);
        }
        return data.size();
    }
    void Poll(){
        if(fd_<0)return;
        pollfd p{fd_,POLLIN,0};
        if(poll(&p,1,2)<=0)return;
        char data[1500];int n=recv(fd_,data,sizeof(data),0);
        if(n>0){if(stream_callback_)stream_callback_(std::string(data,n));}
        else{Disconnect();if(disconnect_callback_)disconnect_callback_();}
    }
};
class LocalHttp:public Http {
    int status_=0;std::string body_,content_;size_t pos_=0;
    std::map<std::string,std::string> request_headers_,response_headers_;
public:
    void SetTimeout(int)override{}
    void SetHeader(const std::string& k,const std::string& v)override{request_headers_[k]=v;}
    void SetContent(std::string&& value)override{content_=value;}
    void SetKeepAlive(bool)override{}
    bool Open(const std::string& method,const std::string& url)override{
        auto start=url.find("://")+3,slash=url.find('/',start),colon=url.find(':',start);
        auto host=url.substr(start,colon-start);int port=std::stoi(url.substr(colon+1,slash-colon-1));
        int fd=Dial(host,port);if(fd<0)return false;
        std::string request=method+" "+url.substr(slash)+" HTTP/1.1\r\nHost: "+host+"\r\nConnection: close\r\n";
        for(const auto& h:request_headers_)request+=h.first+": "+h.second+"\r\n";
        request+="Content-Length: "+std::to_string(content_.size())+"\r\n\r\n"+content_;
        if(!SendAll(fd,request)){close(fd);return false;}
        std::string response;char buffer[2048];int n;
        while((n=recv(fd,buffer,sizeof(buffer),0))>0){response.append(buffer,n);if(response.size()>200000){close(fd);return false;}}
        close(fd);auto end=response.find("\r\n\r\n");if(end==std::string::npos)return false;
        status_=std::stoi(response.substr(9,3));size_t begin=response.find("\r\n")+2;
        while(begin<end){auto line_end=response.find("\r\n",begin);auto split=response.find(':',begin);
            if(split<line_end){auto key=response.substr(begin,split-begin);std::transform(key.begin(),key.end(),key.begin(),::tolower);
                auto value=response.substr(split+1,line_end-split-1);while(!value.empty()&&value[0]==' ')value.erase(0,1);response_headers_[key]=value;}
            begin=line_end+2;
        }
        body_=response.substr(end+4);return true;
    }
    void Close()override{}
    int Read(char* out,size_t n)override{auto count=std::min(n,body_.size()-pos_);body_.copy(out,count,pos_);pos_+=count;return count;}
    int Write(const char*,size_t)override{return -1;}
    int GetStatusCode()override{return status_;}
    std::string GetResponseHeader(const std::string& key)const override{auto it=response_headers_.find(key);return it==response_headers_.end()?"":it->second;}
    size_t GetBodyLength()override{return body_.size();}
    std::string ReadAll()override{return body_;}
    int GetLastError()override{return 0;}
};
struct LocalNetwork:NetworkInterface{
    LocalTcp* tcp=nullptr;
    std::unique_ptr<Http> CreateHttp(int)override{return std::make_unique<LocalHttp>();}
    std::unique_ptr<Tcp> CreateTcp(int)override{return std::make_unique<LocalTcp>(tcp);}
    std::unique_ptr<Tcp> CreateSsl(int)override{return nullptr;}
    std::unique_ptr<WebSocket> CreateWebSocket(int id)override{return std::make_unique<WebSocket>(this,id);}
};
static void CheckHistory(LocalNetwork& network, Client& client, SocketIo& io, const std::string& test_origin) {
    const std::string session = "sess_planning";
    auto fetch = [&](const std::string& cursor = {}) {
        return FetchCatalog(io, "metalio_test", {CatalogOperation::History, session, cursor, {}, 0});
    };
    auto latest = std::get<HistoryPage>(fetch().data);
    Need(latest.messages.size() == 1 && latest.messages.front().offset > 0 &&
         latest.messages.front().ends_message, "latest response should contain the long message tail");
    const auto& tail = latest.messages.front();
    const auto tail_offset = tail.offset;
    const auto previous = std::get<HistoryPage>(fetch(latest.before).data);
    const auto& head = previous.messages.back();
    Need(head.id == tail.id && head.End() == tail.offset && !head.ends_message &&
         (head.content + tail.content).find("99. 核对") != std::string::npos, "UTF-8 continuation boundary");
    Need(fetch("expired/+?").error == CatalogError::CursorExpired, "expired history cursor");

    HistoryEvent gap;
    gap.kind = HistoryEvent::Kind::Delta;
    gap.message = {"gap", "中", "2026-11-24T03:20:00Z", 3, true, false};
    Need(MergeHistoryEvent(latest, std::move(gap)) == MergeResult::Refresh, "noncontiguous stream must refresh");
    const auto status = client.Status();
    auto http = network.CreateHttp(0);
    Need(http->Open("POST", test_origin + "/mock/stream?session_id=sess_planning") &&
         http->GetStatusCode() == 202, "start mock history stream");

    HistoryInbox inbox(session);
    unsigned deltas = 0;
    bool completed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!completed && std::chrono::steady_clock::now() < deadline) {
        network.tcp->Poll();
        client.Tick(true);
        Need(client.Status().connection_id == status.connection_id && client.Ready(), "live history connection");
        for (const auto& raw : client.DrainEvents()) inbox.Publish(raw);
        for (auto& event : inbox.Take()) {
            auto duplicate = event;
            const auto kind = event.kind;
            const auto result = MergeHistoryEvent(latest, std::move(event));
            Need(result != MergeResult::Refresh, "stream should match history");
            Need(MergeHistoryEvent(latest, std::move(duplicate)) == MergeResult::Unchanged,
                 "duplicate UTF-8 span appended twice");
            if (kind == HistoryEvent::Kind::Delta) {
                ++deltas;
                Need(result == MergeResult::Changed, "append expected");
            }
            completed |= kind == HistoryEvent::Kind::Completed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto persisted = std::get<HistoryPage>(fetch().data);
    Need(completed && deltas == 3 && latest.messages.back().content == persisted.messages.back().content,
         "streamed message should match persisted history");
    const auto stable = std::get<HistoryPage>(fetch(latest.before).data);
    Need(stable.messages.back().End() == tail_offset, "original cursor remains stable after new output");
    std::cout << "PASS live history: latest tail, stable before cursor, UTF-8, gap and duplicate handling, streamed completion\n";
}

static void CheckCatalog(LocalNetwork& network, Client& client, SocketIo& io) {
    using Op = CatalogOperation;
    const auto fetch = [&](Op op, std::string key = {}, std::string value = {}, uint32_t version = 0) {
        return FetchCatalog(io, "metalio_test", {op, std::move(key), {}, std::move(value), version});
    };
    const auto all = std::get<Overview>(fetch(Op::Overview, "all").data);
    Need(all.sessions == 53 && all.messages == 5980 && all.total_tokens == 1900000000 &&
         all.peak_hour == "14:00" && all.favorite_model == "DeepSeek V4.1-Pro", "dashboard overview");
    Need(std::get<Overview>(fetch(Op::Overview, "30d").data).sessions == 18, "30-day dashboard");
    Need(std::get<Overview>(fetch(Op::Overview, "7d").data).sessions == 5, "7-day dashboard");
    const auto models = std::get<Directory>(fetch(Op::Models).data);
    const auto levels = std::get<Directory>(fetch(Op::Authorizations).data);
    Need(models.items.size() == 2 && levels.items.size() == 2, "catalog directories");

    const auto list = std::get<Sessions>(fetch(Op::Sessions).data);
    Need(list.items.size() == 2, "session list");
    const auto work = std::get<Session>(fetch(Op::Session, "sess_firmware").data);
    const auto casual = std::get<Session>(fetch(Op::Casual).data);
    Need(work.title == "固件开发" && casual.title == "固定闲聊" &&
         casual.id != work.id && std::get<Session>(fetch(Op::Casual).data).id == casual.id,
         "fixed casual session reuse");

    const auto flash = models.items.front().id;
    const auto updated = std::get<Session>(fetch(Op::SetModel, work.id, flash, work.settings_version).data);
    Need(updated.model_id == flash && updated.settings_version == work.settings_version + 1, "model update");
    const auto full_access = levels.items.back().id;
    const auto stale = fetch(Op::SetAuthorization, work.id, full_access, work.settings_version);
    Need(stale.error == CatalogError::Conflict &&
         std::get<Session>(stale.data).settings_version == updated.settings_version, "conflict snapshot");
    const auto saved = std::get<Session>(fetch(Op::SetAuthorization, work.id, full_access, updated.settings_version).data);
    Need(saved.authorization_id == full_access && saved.model_id == flash, "authorization update");
    const auto reread = std::get<Session>(fetch(Op::Session, work.id).data);
    Need(reread.authorization_id == full_access && reread.settings_version == saved.settings_version,
         "reread confirmed settings");
    Need(std::get<Session>(fetch(Op::SetModel, work.id, flash, saved.settings_version).data).settings_version ==
         saved.settings_version, "reselecting same value is idempotent");
    Need(std::get<Session>(fetch(Op::Session, casual.id).data).settings_version == 1, "independent settings");

    unsigned updates = 0;
    for (int i = 0; i < 25 && updates < 2; ++i) {
        network.tcp->Poll();
        client.Tick(true);
        Need(client.Ready(), "stream became unavailable after settings update");
        for (const auto& event : client.DrainEvents()) updates += IsSessionUpdate(event);
    }
    Need(updates == 2, "two successful writes should generate session.updated events");
    std::cout << "PASS real localhost catalog: all ranges, sessions, directories, casual reuse, version conflict and WS updates\n";
}

static void CheckAudio(LocalNetwork& network, Client& client, SocketIo& io,
                       uint64_t& now, const std::string& scenario) {
    const std::string sid = "sess_firmware";
    constexpr const char* kUtterance = "utt_retry_test";
    const bool silent = scenario == "silent";
    size_t total = 6502;
    if (scenario == "max") total = 960000;
    if (scenario == "short") total = 6398;
    if (scenario == "odd") total = 6501;
    std::string digest = "ac34800cac2440cabbc6d8c4c0fd1657cc0187fa4b7fb84212c3b01ac63ce629";
    if (scenario == "max") digest = "18a4c8254d2cc607c5cfda7ff0e67feb1f92453c942ad1502a983ea0db9939b8";
    if (silent) digest = "c24ca9d2199e50b63a61dccdbf8c0a0d74ae914cc67c671b25e9102c09bfd723";
    if (scenario == "bad_digest") digest.assign(64, '0');
    unsigned releases = 0;
    unsigned hashes = 0;
    std::vector<size_t> offsets;
    auto ticket = std::make_shared<UploadTicket>(sid, 1, kUtterance, RecordingSource{
        total,
        [&](size_t offset, void* out, size_t count) {
            Need(!releases, "read after release");
            Need(count > 0 && count <= 4096 && count % 2 == 0 && offset + count <= total,
                 "PCM chunk is bounded and sample aligned");
            offsets.push_back(offset);
            auto* data = static_cast<uint8_t*>(out);
            for (size_t i = 0; i < count; ++i) data[i] = !silent && i % 2 == 0 ? 42 : 0;
            return true;
        },
        [&] { ++hashes; return digest; },
        [&] { ++releases; },
    });

    std::set<std::string> connections;
    bool patched = false;
    bool retried = false;
    bool saw_transcript = false;
    bool saw_assistant = false;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(9);
    while (std::chrono::steady_clock::now() < end) {
        if (network.tcp) network.tcp->Poll();
        client.Tick(true);
        if (client.Ready()) connections.insert(client.Status().connection_id);
        client.AdvanceUpload(ticket);
        if (scenario == "conflict" && offsets.size() == 1 && !patched) {
            auto changed = FetchCatalog(io, "metalio_test",
                {CatalogOperation::SetAuthorization, sid, {}, "full_access", 1});
            Need(changed.error == CatalogError::None, "simulated second client settings write");
            patched = true;
        }
        const auto status = ticket->Status();
        if (status.phase == UploadPhase::RetryRequired) {
            if (scenario == "busy_retry" && !retried) {
                Need(status.error == "session_busy" && !releases && ticket->Request(UploadAction::Retry),
                     "busy recording is retained and can be retried explicitly");
                retried = true;
                continue;
            }
            const bool invalid_pcm = scenario == "short" || scenario == "odd" || scenario == "bad_digest";
            Need((invalid_pcm && status.error == "invalid_audio") ||
                 (scenario == "busy" && status.error == "session_busy") ||
                 (scenario == "conflict" && status.error == "settings_conflict"), "upload rejected as expected");
            Need(!releases, "rejected audio must remain retained");
            Need(status.Retryable() == (scenario == "busy"), "retry permissions");
            Need(ticket->Request(UploadAction::Discard), "explicit discard");
            client.AdvanceUpload(ticket);
            Need(releases == 1 && ticket->Status().phase == UploadPhase::Discarded, "discard releases once");
            std::cout << "PASS audio rejection and explicit discard: " << scenario << "\n";
            return;
        }
        if (client.Ready()) {
            for (const auto& event : client.DrainEvents()) {
                ticket->Observe(event);
                auto update = ReadHistoryEvent(event, sid);
                if (!update || update->kind != HistoryEvent::Kind::Delta) continue;
                if (update->message.assistant) saw_assistant = true;
                else {
                    saw_transcript = true;
                    Need(ticket->Status().transcript_id == update->message.id,
                         "ASR for the accepted utterance identifies the displayed transcript");
                }
                Need(!saw_assistant || saw_transcript, "ASR should precede the assistant response");
            }
        }
        if (status.phase == UploadPhase::Accepted && (saw_assistant || ticket->Status().error == "asr_no_speech")) {
            Need(!status.run_id.empty() && releases == 1 && hashes == 1, "confirm then release once");
            Need(saw_transcript != silent && saw_assistant != silent, "ASR and assistant are ordered");
            if (scenario == "begin_ack" || scenario == "body" || scenario == "end_ack" ||
                scenario == "wrong_ack") {
                Need(connections.size() >= 2, "fault must recover on a new connection");
            }
            if (scenario == "end_ack" || scenario == "begin_ack" || scenario == "wrong_ack")
                Need(offsets.size() == 2, "server should receive only one complete upload");
            if (scenario == "body")
                Need(std::count(offsets.begin(), offsets.end(), 0) >= 2, "partial upload retried from offset zero");
            if (scenario == "max") Need(offsets.back() == 958464, "full-size final block");
            if (scenario == "busy_retry") Need(retried && offsets.size() == 4, "manual retry sends retained PCM");
            std::cout << "PASS recording upload / original utterance recovery: " << scenario << "\n";
            return;
        }
        Need(!releases || status.phase == UploadPhase::Accepted, "do not release before acceptance");
        now += 140;
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    std::cerr << "upload last state " << int(ticket->Status().phase) << " " << ticket->Status().error << "\n";
    throw std::runtime_error("audio integration did not complete");
}

static void CheckRun(LocalNetwork& network, Client& client, SocketIo& io,
                     uint64_t& now, const std::string& origin, const std::string& scenario) {
    const std::string session = "sess_planning";
    const std::string identity = origin + "\nmetalio_test";
    RunTracker runs;
    std::string connection;
    auto pump = [&] {
        if (network.tcp) network.tcp->Poll();
        client.Tick(true);
        if (client.Ready()) {
            const auto status = client.Status();
            if (connection != status.connection_id) {
                runs.Snapshot(client.Resources());
                connection = status.connection_id;
            }
            for (const auto& event : client.DrainEvents()) runs.Observe(event);
        } else connection.clear();
        if (auto result = client.TakeCancelResult()) runs.FinishCancel(*result);
        if (!client.CancelPending()) {
            if (auto run = runs.TakeCancel()) Need(client.SendCancel(*run), "cancel request send");
        }
        if (scenario == "lost_terminal" && client.Ready()) {
            if (auto pending = runs.NextLookup(now)) {
                auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Run, pending->id, {}, {}, 0});
                runs.LookupResult(*pending, std::get<RunInfo>(reply.data), false);
            }
        }
        now += scenario == "unknown" || scenario == "lost_terminal" ? 500 : 20;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    };
    auto wait_for = [&](const auto& condition, const char* error) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!condition()) {
            Need(std::chrono::steady_clock::now() < end, error);
            pump();
        }
    };
    auto http = network.CreateHttp(0);
    const auto url = origin + "/mock/run?session_id=" + session + "&mode=" +
        (scenario == "recover" || scenario == "cancel" ? "normal" : scenario);
    Need(http->Open("POST", url) && http->GetStatusCode() == 202, "start a web task");
    auto competing = network.CreateHttp(0);
    Need(competing->Open("POST", url) && competing->GetStatusCode() == 409, "same session rejects competing task");
    wait_for([&] {
        const auto task = runs.View(session);
        return !task.run.id.empty() && task.run.state == RunState::Running;
    }, "web task should be observed");
    RunView task = runs.View(session);
    const auto id = task.run.id;
    Need(!task.confirming && task.run.version == 2, "initial task state");
    runs.Observe("{\"type\":\"run.progress\",\"session_id\":\"sess_planning\",\"run_id\":\"" +
                 id + "\",\"data\":{\"state_version\":1,\"text\":\"old progress\"}}");
    Need(runs.View(session).run.progress != "old progress", "old version must not overwrite progress");

    if (scenario == "recover") {
        wait_for([&] { return runs.View(session).run.progress == "正在读取会话"; }, "receive progress update");
        const auto saved = runs.SaveIds(identity);
        RunTracker rebooted;
        rebooted.RestoreIds("another backend", saved);
        Need(rebooted.View(session).run.id.empty(), "storage identity isolation");
        rebooted.RestoreIds(identity, saved);
        Need(rebooted.View(session).confirming && rebooted.View(session).run.id == id,
             "restore tracked ID after device restart");
        client.Tick(false);
        const auto first_connection = connection;
        wait_for([&] { return client.Ready() && connection != first_connection; }, "reconnect to active snapshot");
        rebooted.Snapshot(client.Resources());
        Need(!rebooted.View(session).confirming && rebooted.View(session).run.id == id,
             "active snapshot restores task");
        client.Tick(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        const auto second_connection = connection;
        wait_for([&] { return client.Ready() && connection != second_connection; }, "reconnect after completion");
        Need(client.Status().runs == 0, "terminal task not in active snapshot");
        rebooted.Snapshot(client.Resources());
        auto pending = rebooted.NextLookup(now);
        Need(pending && pending->id == id, "formerly active task requires HTTP recovery");
        auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Run, id, {}, {}, 0});
        auto result = std::get<RunInfo>(reply.data);
        rebooted.LookupResult(*pending, result, false);
        Need(rebooted.View(session).run.state == RunState::Completed &&
             !rebooted.View(session).confirming &&
             rebooted.SaveIds(identity).find(id) == std::string::npos, "recovered terminal state");
        std::cout << "PASS web task progress, reconnect, reboot and missing terminal recovery\n";
        return;
    }

    Need(runs.QueueCancel(session, id), "queue cancel");
    Need(!runs.QueueCancel(session, id), "reject duplicate cancel");
    if (scenario == "unknown") {
        wait_for([&] { return runs.View(session).cancel_error == "confirmation_unknown"; }, "cancel timeout");
        auto pending = runs.NextLookup(now);
        Need(pending && pending->id == id && !client.CancelPending(), "cancel uncertainty triggers lookup");
        auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Run, id, {}, {}, 0});
        auto actual = std::get<RunInfo>(reply.data);
        Need(actual.state == RunState::Running, "server retained actual running state");
        runs.LookupResult(*pending, actual, false);
        Need(!runs.View(session).confirming && !runs.View(session).cancelling,
             "request is reconciled with running server state");
    } else if (scenario == "unsupported") {
        wait_for([&] { return runs.View(session).cancel_error == "cancel_not_supported"; }, "unsupported cancel");
        Need(runs.View(session).run.state == RunState::Running &&
             !runs.QueueCancel(session, id), "unsupported leaves task running");
    } else {
        wait_for([&] { return runs.View(session).run.Finished(); }, "authoritative terminal event");
        const auto expected = scenario == "race" ? RunState::Completed : RunState::Cancelled;
        Need(runs.View(session).run.state == expected && !client.CancelPending(), "terminal state wins cancel race");
        const auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Run, id, {}, {}, 0});
        Need(std::get<RunInfo>(reply.data).state == expected, "terminal event matches HTTP");
        runs.Observe("{\"type\":\"run.state\",\"data\":{\"run_id\":\"" + id +
                     "\",\"session_id\":\"sess_planning\",\"state_version\":1,\"status\":\"running\"}}");
        Need(runs.View(session).run.state == expected, "stale run cannot resurrect task");
    }
    std::cout << "PASS cancel: " << scenario << "\n";
}

static void CheckApproval(LocalNetwork& network, Client& client, SocketIo& io,
                          uint64_t& now, const std::string& origin, const std::string& scenario) {
    ApprovalTracker approvals;
    std::string connection;
    std::string last_result;
    auto pump = [&] {
        if (network.tcp) network.tcp->Poll();
        client.Tick(true);
        if (client.Ready()) {
            const auto current = client.Status().connection_id;
            if (connection != current) {
                approvals.Snapshot(client.Resources());
                connection = current;
            }
            for (const auto& event : client.DrainEvents()) approvals.Observe(event);
        } else {
            if (!connection.empty()) approvals.Offline();
            connection.clear();
        }
        if (auto result = client.TakeApprovalResult()) {
            last_result = result->error;
            approvals.Finish(std::move(*result));
        }
        if (!client.ApprovalPending()) {
            if (const auto request = approvals.TakeRequest()) {
                Need(client.SendApproval(*request), "send approval decision");
            }
        }
        if (client.Ready() && !client.ApprovalPending()) {
            if (auto id = approvals.NextLookup(now)) {
                auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Approval, *id, {}, {}, 0});
                auto* info = std::get_if<ApprovalPtr>(&reply.data);
                approvals.LookupResult(*id, info ? *info : ApprovalPtr{}, reply.error == CatalogError::NotFound);
            }
        }
        now += scenario == "lost_ack" ? 250 : 30;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    };
    const auto await = [&](const auto& condition, const char* error) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!condition()) {
            Need(std::chrono::steady_clock::now() < deadline, error);
            pump();
        }
    };
    const auto start = [&](const std::string& session, const std::string& mode) {
        auto http = network.CreateHttp(0);
        Need(http->Open("POST", origin + "/mock/run?session_id=" + session + "&mode=" + mode) &&
             http->GetStatusCode() == 202, "start mock approval job");
    };
    const bool special = scenario == "long" || scenario == "web_only" || scenario == "expire" ||
        scenario == "web_wins" || scenario == "lost_ack" || scenario == "version";
    start("sess_planning", special ? "approval_" + scenario : "approval");
    if (scenario == "multi") start("sess_firmware", "approval");
    await([&] { return approvals.Pending().size() == (scenario == "multi" ? 2u : 1u); },
          "waiting approval event");
    const auto pending = approvals.Pending();
    const auto initial = pending.front();
    const std::string id = initial->id;
    const auto detail = FetchCatalog(io, "metalio_test", {CatalogOperation::Approval, id, {}, {}, 0});
    auto reread = std::get<ApprovalPtr>(detail.data);
    Need(reread && reread->operation == initial->operation && reread->version == 1, "lookup returns exact details");
    if (scenario == "web_only") {
        Need(!initial->actionable && initial->operation.empty(), "web-only payload");
        Need(!approvals.Queue(initial, ApprovalDecision::AllowOnce) &&
             !client.SendApproval({initial, ApprovalDecision::AllowOnce}), "device allow must be blocked");
        const std::string request = "{\"v\":1,\"type\":\"approval.respond\",\"command_id\":\"forced_web_only\","
            "\"session_id\":\"" + initial->session_id + "\",\"run_id\":\"" + initial->run_id +
            "\",\"data\":{\"approval_id\":\"" + id + "\",\"expected_approval_version\":1,\"decision\":\"approve_once\"}}";
        Need(io.SendText(request), "simulate web-only rejection by backend");
    } else {
        Need(initial->actionable && initial->operation.find("9007199254740993") != std::string::npos,
             "approval displays original integer without rounding");
        if (scenario == "long") {
            Need(initial->operation.find("64 项") != std::string::npos, "complete long detail");
        }
    }

    ApprovalState expected = ApprovalState::Approved;
    if (scenario == "expire") {
        expected = ApprovalState::Expired;
    } else if (scenario == "cancel") {
        RunInfo run;
        run.id = initial->run_id;
        run.session_id = initial->session_id;
        Need(client.SendCancel(run), "cancel while approval is pending");
        expected = ApprovalState::Cancelled;
    } else if (scenario == "reconnect") {
        client.Tick(false);
        approvals.Offline();
        Need(approvals.Pending().empty() && !approvals.Queue(initial, ApprovalDecision::AllowOnce),
             "offline blocks approval and hides old reminders");
        const auto first = connection;
        await([&] { return client.Ready() && connection != first && approvals.Pending().size() == 1; },
              "refetch approval after reconnect");
        Need(approvals.View(id).info->operation == initial->operation && !approvals.View(id).checking,
             "review recovered intact");
        client.Tick(false);
        approvals.Offline();
        auto http = network.CreateHttp(0);
        Need(http->Open("POST", origin + "/mock/approval/" + id + "?decision=reject") &&
             http->GetStatusCode() == 200, "web rejects while device is offline");
        expected = ApprovalState::Rejected;
    } else {
        const bool reject = scenario == "reject" || scenario == "web_only";
        const auto decision = reject ? ApprovalDecision::Reject : ApprovalDecision::AllowOnce;
        Need(approvals.Queue(initial, decision), "queue a single decision");
        Need(!approvals.Queue(initial, decision), "duplicates cannot queue another command");
        if (scenario == "web_wins" || reject) expected = ApprovalState::Rejected;
        if (scenario == "version") {
            await([&] { return approvals.View(id).info->version == 2; }, "stale version refreshed");
            Need(!approvals.Queue(initial, decision), "stale reviewed object cannot submit");
            Need(approvals.Queue(approvals.View(id).info, decision), "new version may be reviewed and sent");
        }
    }
    await([&] {
        const auto view = approvals.View(id);
        return view.info && view.info->state != ApprovalState::Pending;
    }, "authoritative approval outcome");
    const auto outcome = approvals.View(id).info;
    Need(outcome->state == expected && outcome->version > 1 && !client.ApprovalPending(),
         "decision reflects actual server state");
    if (scenario == "lost_ack") Need(last_result == "confirmation_unknown", "missing confirmations require lookup");
    if (scenario == "multi") {
        Need(approvals.Pending().size() == 1 && approvals.Pending().front()->id != id,
             "other session retains its queued approval");
    }
    if (expected == ApprovalState::Approved) {
        await([&] {
            auto run = FetchCatalog(io, "metalio_test", {CatalogOperation::Run, initial->run_id, {}, {}, 0});
            return std::get<RunInfo>(run.data).state == RunState::Completed;
        }, "tool can run only after approval");
    }
    std::cout << "PASS authoritative approval: " << scenario << "\n";
}

static void CheckStandby(LocalNetwork& network, Client& client, SocketIo& io, uint64_t& now,
                         const std::string& origin) {
    PowerPolicy::HarnessNetworkState power{true, 0};
    auto observed_epoch = power.sleep_epoch;
    ApprovalTracker approvals;
    std::string connection;
    const auto pump = [&] {
        if (network.tcp) network.tcp->Poll();
        client.Tick(power.AllowsWorkAt(observed_epoch));
        observed_epoch = power.sleep_epoch;
        const auto status = client.Status();
        if (status.state == ConnectionState::Ready) {
            if (connection != status.connection_id) approvals.Snapshot(client.Resources());
            connection = status.connection_id;
            for (const auto& event : client.DrainEvents()) approvals.Observe(event);
            if (const auto id = approvals.NextLookup(now)) {
                auto reply = FetchCatalog(io, "metalio_test", {CatalogOperation::Approval, *id, {}, {}, 0});
                auto* detail = std::get_if<ApprovalPtr>(&reply.data);
                approvals.LookupResult(*id, detail ? *detail : ApprovalPtr{}, reply.error == CatalogError::NotFound);
            }
        } else {
            approvals.Offline();
            connection.clear();
        }
        now += 25;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    };
    const auto first_connection = client.Status().connection_id;
    const auto await = [&](const auto& condition, const char* error) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (!condition()) {
            Need(std::chrono::steady_clock::now() < deadline, error);
            pump();
        }
    };
    pump();
    // Online standby leaves the radio available and the current connection valid.
    auto http = network.CreateHttp(0);
    Need(http->Open("POST", origin + "/mock/run?mode=approval") && http->GetStatusCode() == 202,
         "start approval while online standby stays connected");
    http->Close();
    await([&] { return approvals.Pending().size() == 1; }, "online standby receives pending approval");
    Need(connection == first_connection, "online standby preserves connection");
    const auto pending = approvals.Pending().front();

    // The worker observes only the post-wake state when sleep finishes between polls.
    ++power.sleep_epoch;
    Need(!power.AllowsWorkAt(observed_epoch), "awake state from a new epoch rejects stale readiness");
    pump();
    Need(!client.Ready() && approvals.Pending().empty(), "first post-wake tick drops stale WS and approval");
    await([&] { return client.Ready() && approvals.Pending().size() == 1; }, "resync after shallow sleep");
    const auto restored = approvals.Pending().front();
    Need(connection != first_connection && restored->id == pending->id && restored->operation == pending->operation,
         "new connection restores the same pending operation");

    power.allowed = false;
    pump();
    Need(!client.Ready() && approvals.Pending().empty(), "no WS or local approval while power blocks networking");
    std::cout << "PASS online standby delivery, sleep epoch disconnect and approval recovery\n";
}

int main(int argc,char** argv){
    try{
        const bool catalog = argc==3 && std::strcmp(argv[2], "--catalog")==0;
        const bool audio = argc==4 && std::strcmp(argv[2], "--audio")==0;
        const bool run = argc==4 && std::strcmp(argv[2], "--run")==0;
        const bool approval = argc==4 && std::strcmp(argv[2], "--approval")==0;
        const bool standby = argc == 3 && std::strcmp(argv[2], "--standby") == 0;
        Need(argc==2 || catalog || audio || run || approval || standby, "provide local mock URL");
        LocalNetwork network;SocketIo io(network);
        uint64_t now=0;Client client(io,{argv[1],"metalio_test","host-test","local_boot"},[&](){return now;});
        std::set<std::string> connections;unsigned events=0;bool saw_backoff=false;
        auto started=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-started<std::chrono::seconds(12)){
            if(network.tcp)network.tcp->Poll();
            client.Tick(true);auto status=client.Status();
            if(status.state==ConnectionState::Backoff)saw_backoff=true;
            if(client.Ready()){
                if (standby) {
                    CheckStandby(network, client, io, now, argv[1]);
                    client.Stop();
                    return 0;
                }
                if (approval) {
                    CheckApproval(network, client, io, now, argv[1], argv[3]);
                    client.Stop();
                    return 0;
                }
                if (run) {
                    CheckRun(network, client, io, now, argv[1], argv[3]);
                    client.Stop();
                    return 0;
                }
                if (audio) {
                    CheckAudio(network, client, io, now, argv[3]);
                    client.Stop();
                    return 0;
                }
                if (catalog) {
                    CheckCatalog(network, client, io);
                    CheckHistory(network, client, io, argv[1]);
                    client.Stop();
                    return 0;
                }
                connections.insert(status.connection_id);
                Need(status.runs==1&&status.approvals==1,"restored snapshot counts");
                events+=client.DrainEvents().size();
                if(connections.size()>=2&&events>=4&&saw_backoff){
                    client.Stop();std::cout<<"PASS real localhost HTTP/WS: paged sync, fragmented UTF8, replay, Pong and reconnect\n";return 0;
                }
            }
            now+=250;std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        std::cerr<<"last state "<<int(client.Status().state)<<" error "<<client.Status().error<<"\n";
        throw std::runtime_error("localhost integration did not complete");
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
