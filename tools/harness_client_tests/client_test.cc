#include "harness/client.h"
#include <cJSON.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
using namespace harness;
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static std::string Field(const std::string& json,const char* name){
    auto* o=cJSON_Parse(json.c_str());auto* p=cJSON_GetObjectItem(o,name);
    std::string s=cJSON_IsString(p)?p->valuestring:"";cJSON_Delete(o);return s;
}
struct FakeIo : ClientIo {
    Callbacks callbacks;
    bool connect_ok=true,send_ok=true,hello=true,ack=true,ready=true,respond_ping=true;
    unsigned connects=0,closes=0,pings=0;
    std::vector<std::string> sent,paths;
    std::deque<HttpReply> pages;
    std::string connection="conn_1",token="token_1";
    std::function<void()> during_http;
    bool Connect(const Config&,Callbacks cb) override {callbacks=std::move(cb);++connects;return connect_ok;}
    void Emit(std::string json){callbacks.text(std::move(json));}
    bool SendText(const std::string& json) override {
        sent.push_back(json);if(!send_ok)return false;
        auto type=Field(json,"type"),id=Field(json,"command_id");
        if(type=="hello"&&hello)
            Emit("{\"v\":1,\"type\":\"hello\",\"command_id\":\""+id+"\",\"data\":{\"connection_id\":\""+connection+
                "\",\"sync_token\":\""+token+"\",\"heartbeat_interval_ms\":20000,\"heartbeat_timeout_ms\":60000}}");
        if(type=="sync.complete") {
            if(ack)Emit("{\"v\":1,\"type\":\"ack\",\"command_id\":\""+id+"\",\"ok\":true,\"data\":{\"status\":\"syncing\"}}");
            if(ready)Emit("{\"v\":1,\"type\":\"sync.ready\",\"data\":{}}");
        }
        return true;
    }
    void Ping() override {++pings;if(respond_ping)callbacks.pong();}
    void Close() override {++closes;}
    HttpReply Request(const std::string& method,const std::string& path,const std::string&) override {
        Check(method=="GET","unexpected method");paths.push_back(path);
        if(during_http)during_http();
        if(pages.empty())return {200,"{\"items\":[],\"next_cursor\":null}",""};
        auto p=pages.front();pages.pop_front();return p;
    }
};
static Config TestConfig(){return {"http://127.0.0.1:18080","metalio_test","0.1.1-dashboard","test_boot"};}
struct Fixture {
    FakeIo io;
    uint64_t now=0;
    Client client;
    Fixture():client(io,TestConfig(),[this](){return now;}){}
    void Tick(){client.Tick(true);}
    void Ready(){Tick();Tick();Check(client.Ready(),"not ready");}
    void Advance(uint64_t ms){now+=ms;Tick();}
};
static std::string RunData(const char* status="running",unsigned version=1) {
    return "{\"run_id\":\"run_1\",\"session_id\":\"sess_1\",\"status\":\""+std::string(status)+"\",\"state_version\":"+std::to_string(version)+"}";
}
static std::string Snapshot(const std::string& cursor="null"){
    return "{\"items\":[{\"kind\":\"run\",\"data\":"+RunData()+"}],\"next_cursor\":"+cursor+"}";
}
static std::string Event(unsigned seq,const char* type="run.progress",const std::string& data="{\"text\":\"working\"}"){
    return "{\"v\":1,\"type\":\""+std::string(type)+"\",\"connection_id\":\"conn_1\",\"seq\":\""+std::to_string(seq)+
        "\",\"session_id\":\"sess_1\",\"run_id\":\"run_1\",\"data\":"+data+"}";
}
int main(){
    unsigned passed=0,failed=0;
    auto test=[&](const char* name,auto fn){try{fn();++passed;std::cout<<"PASS "<<name<<"\n";}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<"\n";}};
    test("empty configuration makes no connection",[]{
        FakeIo io;auto c=TestConfig();c.base_url="";Client client(io,c,[](){return 0;});client.Tick(true);
        Check(io.connects==0&&client.Status().state==ConnectionState::Unconfigured,"connected without address");
    });
    test("invalid endpoint and identity rejected",[]{
        for(auto url:{"ftp://host","http://user:pass@host","http://host/path","http://host:65536","http://host:0","http://host\r\n"}) {
            auto c=TestConfig();c.base_url=url;Check(!ValidConfig(c),"invalid URL accepted");
        }
        auto c=TestConfig();c.device_id="bad\nid";Check(!ValidConfig(c),"header injection accepted");
    });
    test("empty hello sync ready exchange",[]{
        Fixture f;Check(!f.client.Ready(),"initial ready");f.Tick();
        Check(!f.client.Ready()&&f.client.Status().state==ConnectionState::Syncing,"ready before ack");
        f.Tick();Check(f.client.Ready()&&f.client.Resources().empty(),"empty sync failed");
        Check(Field(f.io.sent[0],"type")=="hello"&&Field(f.io.sent[1],"type")=="sync.complete","command order");
        Check(f.io.paths[0]=="/api/v1/sync?sync_token=token_1","sync path");
    });
    test("HTTP snapshot pagination is atomic and cursors encoded",[]{
        Fixture f;f.io.token="a+/=";
        f.io.pages.push_back({200,Snapshot("\"c+/=\""),""});
        f.io.pages.push_back({200,"{\"items\":[],\"next_cursor\":null}",""});
        f.Tick();Check(f.client.Resources().empty()&&!f.client.Ready(),"partial snapshot published");
        f.Tick();Check(f.client.Resources().empty(),"published before ready");
        f.Tick();Check(f.client.Ready()&&f.client.Resources().size()==1,"snapshot lost");
        Check(f.io.paths[0]=="/api/v1/sync?sync_token=a%2B%2F%3D","token not encoded");
        Check(f.io.paths[1].find("cursor=c%2B%2F%3D")!=std::string::npos,"cursor not encoded");
    });
    test("failed later page discards partial state",[]{
        Fixture f;f.io.pages.push_back({200,Snapshot("\"next\""),""});f.io.pages.push_back({503,"{}",""});
        f.Tick();f.Tick();Check(f.client.Status().state==ConnectionState::Backoff&&f.client.Resources().empty(),"partial state committed");
    });
    test("sync rejection and expiry recover",[]{
        Fixture f;f.io.pages.push_back({409,"{\"error\":{\"code\":\"sync_expired\"}}",""});f.Tick();
        Check(f.client.Status().state==ConnectionState::Backoff,"expired sync not retried");
        f.Advance(1000);f.Tick();Check(f.client.Ready(),"sync did not recover");
    });
    test("hello ack timeout and failed send",[]{
        Fixture f;f.io.hello=false;f.Tick();f.Advance(10000);
        Check(f.client.Status().error=="hello_timeout","hello timeout missing");
        Fixture g;g.io.send_ok=false;g.Tick();Check(g.client.Status().error=="send_failed","send error missing");
    });
    test("sync ack timeout",[]{
        Fixture f;f.io.ack=false;f.io.ready=false;f.Tick();f.Advance(10000);
        Check(f.client.Status().error=="ack_timeout","sync ack not bounded");
    });
    test("ready cannot arrive before sync ack",[]{
        Fixture f;f.io.ack=false;f.Tick();f.Tick();
        Check(f.client.Status().error=="premature_ready"&&!f.client.Ready(),"premature ready accepted");
    });
    test("backoff follows 1 2 4 8 16 30 seconds",[]{
        Fixture f;f.io.connect_ok=false;unsigned previous=0;
        for(unsigned delay:{1000,2000,4000,8000,16000,30000,30000}) {
            f.Tick();Check(f.io.connects==++previous,"connect attempt missing");
            Check(f.client.Status().retry_in_ms==delay,"backoff wrong");
            f.now+=delay-1;f.Tick();Check(f.io.connects==previous,"early retry");++f.now;
        }
    });
    test("ready resets retry backoff",[]{
        Fixture f;f.io.connect_ok=false;f.Tick();f.now+=1000;f.Tick();f.io.connect_ok=true;f.now+=2000;
        f.Ready();f.io.callbacks.disconnected();f.Tick();
        Check(f.client.Status().retry_in_ms==1000,"backoff did not reset");
    });
    test("ping and pong keep idle connection ready",[]{
        Fixture f;f.Ready();
        for(int i=0;i<10;i++){f.Advance(20000);f.Tick();Check(f.client.Ready(),"heartbeat disconnected");}
        Check(f.io.pings==10,"ping schedule wrong");
    });
    test("heartbeat timeout ignores ordinary event traffic",[]{
        Fixture f;f.io.respond_ping=false;f.Ready();f.Advance(20000);
        f.io.Emit(Event(1));f.Tick();f.Advance(40000);
        Check(f.client.Status().error=="heartbeat_timeout","non-pong hid heartbeat failure");
    });
    test("connection sequence gaps resync and duplicates ignored",[]{
        Fixture f;f.Ready();f.io.Emit(Event(1));f.io.Emit(Event(1));f.Tick();
        Check(f.client.DrainEvents().size()==1,"duplicate event delivered");
        f.io.Emit(Event(3));f.Tick();Check(f.client.Status().error=="sequence_gap","gap ignored");
    });
    test("stale callbacks from old connection ignored",[]{
        Fixture f;f.Ready();auto old=f.io.callbacks;old.disconnected();f.Tick();
        f.now+=1000;f.Ready();old.text("malformed");old.disconnected();f.Tick();
        Check(f.client.Ready(),"old connection corrupted current");
    });
    test("queue overflow forces resync",[]{
        Fixture f;f.Ready();for(int i=0;i<40;i++)f.io.Emit(Event(1));f.Tick();
        Check(f.client.Status().error=="receive_queue_full","input queue overflow ignored");
    });
    test("unconsumed business event queue is bounded",[]{
        Fixture f;f.Ready();for(unsigned i=1;i<=33;i++){f.io.Emit(Event(i));f.Tick();}
        Check(f.client.Status().error=="event_queue_full","event queue unbounded");
    });
    test("replay is applied before ready and remains hidden during sync",[]{
        Fixture f;f.io.ready=false;f.io.pages.push_back({200,Snapshot(),""});f.Tick();f.Tick();
        Check(!f.client.Ready()&&f.client.Resources().empty(),"sync resources exposed");
        f.io.Emit(Event(1,"run.state",RunData("running",2)));
        f.io.Emit("{\"v\":1,\"type\":\"sync.ready\",\"data\":{}}");f.Tick();
        Check(f.client.Ready()&&f.client.Resources().at(0).version==2,"backlog not applied");
        Check(f.client.DrainEvents().size()==1,"replay event lost");
    });
    test("older resource versions do not overwrite newer",[]{
        Fixture f;f.Ready();f.io.Emit(Event(1,"run.state",RunData("running",3)));f.Tick();
        f.io.Emit(Event(2,"run.state",RunData("running",2)));f.Tick();
        Check(f.client.Resources().at(0).version==3,"old resource overwrite");
    });
    test("terminal resource cannot be resurrected by old state",[]{
        Fixture f;f.Ready();f.io.Emit(Event(1,"run.state",RunData("running",2)));f.Tick();
        f.io.Emit(Event(2,"run.completed",RunData("completed",3)));f.Tick();
        f.io.Emit(Event(3,"run.state",RunData("running",2)));f.Tick();
        Check(f.client.Resources().empty(),"old event resurrected completed run");
    });
    test("repeated snapshot cursor rejected",[]{
        Fixture f;f.io.pages.push_back({200,Snapshot("\"same\""),""});f.io.pages.push_back({200,Snapshot("\"same\""),""});
        f.Tick();f.Tick();Check(f.client.Status().error=="invalid_cursor","cursor loop accepted");
    });
    test("malformed snapshot and protocol messages rejected",[]{
        Fixture f;f.io.pages.push_back({200,"{\"items\":[]}",""});f.Tick();
        Check(f.client.Status().error=="invalid_snapshot","missing cursor accepted");
        Fixture g;g.Ready();g.io.Emit("{} trailing");g.Tick();Check(g.client.Status().error=="invalid_message","bad JSON accepted");
    });
    test("sync deadline expires even during slow HTTP request",[]{
        Fixture f;f.io.during_http=[&](){f.now+=120001;};f.Tick();
        Check(f.client.Status().state==ConnectionState::Backoff,"expired HTTP sync allowed");
    });
    test("network loss clears ready status and waits",[]{
        Fixture f;f.Ready();f.client.Tick(false);
        Check(f.client.Status().state==ConnectionState::WaitingNetwork&&!f.client.Ready(),"network loss ignored");
        unsigned count=f.io.connects;f.client.Tick(false);Check(count==f.io.connects,"network not ready dialed");f.Ready();
    });
    test("stop invalidates callbacks and prevents reconnect",[]{
        Fixture f;f.Ready();auto old=f.io.callbacks;f.client.Stop();old.text("bad");f.Advance(100000);
        Check(f.client.Status().state==ConnectionState::Stopped&&f.io.connects==1,"stopped client reconnected");
    });
    test("connection status observer sees connecting before blocking IO",[]{
        FakeIo io;uint64_t now=0;std::vector<ConnectionState> states;
        Client client(io,TestConfig(),[&](){return now;},[&](const ConnectionSnapshot& s){states.push_back(s.state);});
        client.Tick(true);client.Tick(true);
        Check(std::find(states.begin(),states.end(),ConnectionState::Connecting)!=states.end(),"connecting state not published");
        Check(client.Ready(),"observer prevented connection");
    });
    test("old queued pong does not extend heartbeat deadline",[]{
        Fixture f;f.io.respond_ping=false;f.Ready();
        f.io.callbacks.pong();f.now=60000;f.Tick();
        Check(f.client.Status().error=="heartbeat_timeout","stale pong treated as current");
    });
    test("server sync error triggers fresh handshake",[]{
        Fixture f;f.Ready();f.io.Emit("{\"v\":1,\"type\":\"error\",\"error\":{\"code\":\"sync_expired\"}}");f.Tick();
        Check(f.client.Status().error=="sync_expired","server error lost");f.now+=1000;f.Ready();
    });
    test("resource envelope cannot cross sessions",[]{
        Fixture f;f.Ready();auto payload=RunData();auto p=payload.find("sess_1");payload.replace(p,6,"sess_2");
        f.io.Emit(Event(1,"run.state",payload));f.Tick();
        Check(f.client.Status().error=="invalid_resource","cross session event accepted");
    });
    test("unknown resource status rejected",[]{
        Fixture f;f.Ready();f.io.Emit(Event(1,"run.state",RunData("unknown",1)));f.Tick();
        Check(f.client.Status().error=="invalid_resource","unknown state accepted");
    });
    std::cout<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
