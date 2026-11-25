#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <random>
#include "network_interface.h"
#include "web_socket.h"
#include "harness/transport.h"
#include <cJSON.h>

static void Check(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
static const std::string kHandshake="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
static std::string Frame(unsigned opcode,const std::string& payload,bool fin=true,int encoding=0) {
    std::string f(1,static_cast<char>((fin?0x80:0)|opcode));
    if(encoding==127 || payload.size()>65535) {
        f.push_back(127);for(int i=7;i>=0;--i)f.push_back(static_cast<char>((uint64_t(payload.size())>>(i*8))&255));
    } else if(encoding==126 || payload.size()>=126) {
        f.push_back(126);f.push_back(static_cast<char>(payload.size()>>8));f.push_back(static_cast<char>(payload.size()));
    } else f.push_back(static_cast<char>(payload.size()));
    return f+payload;
}
struct FakeTcp : Tcp {
    std::vector<std::string> sent;
    std::string pending,handshake=kHandshake;
    bool queue_early=true;
    bool Connect(const std::string&,int) override {connected_=true;return true;}
    void Disconnect() override {bool old=connected_;connected_=false;if(old&&disconnect_callback_)disconnect_callback_();}
    int Send(const std::string& data) override {
        sent.push_back(data);
        if(data.rfind("GET ",0)==0)Emit(handshake);
        return static_cast<int>(data.size());
    }
    int GetLastError() override {return 0;}
    void Emit(const std::string& data) {if(stream_callback_)stream_callback_(data);else if(queue_early) pending+=data;}
    void OnStream(std::function<void(const std::string&)> cb) override {
        Tcp::OnStream(cb);if(cb&&!pending.empty()){auto data=std::move(pending);pending.clear();cb(data);}
    }
};
struct FakeNetwork : NetworkInterface {
    FakeTcp* tcp=nullptr;
    std::string response=kHandshake;
    bool queue_early=true;
    std::unique_ptr<Tcp> CreateTcp(int) override {auto p=std::make_unique<FakeTcp>();p->handshake=response;p->queue_early=queue_early;tcp=p.get();return p;}
    std::unique_ptr<Tcp> CreateSsl(int id) override {return CreateTcp(id);}
    std::unique_ptr<WebSocket> CreateWebSocket(int id) override {return std::make_unique<WebSocket>(this,id);}
};
struct Fixture {
    FakeNetwork network;
    WebSocket socket{&network,1};
    std::vector<std::string> received;
    std::vector<bool> binary;
    int errors=0,disconnected=0;
    Fixture(size_t limit=0) {
        if(limit){socket.SetReceiveBufferSize(limit);socket.SetMessageSizeLimit(limit);}
        socket.OnData([this](const char* d,size_t n,bool b){received.emplace_back(d,n);binary.push_back(b);});
        socket.OnError([this](int){++errors;});
        socket.OnDisconnected([this](){++disconnected;});
    }
    void Connect(){Check(socket.Connect("ws://localhost:8080/api/v1/ws"),"handshake failed");}
    void Emit(const std::string& data){network.tcp->Emit(data);}
};
static std::pair<unsigned,std::string> DecodeClient(const std::string& f) {
    Check(f.size()>=6,"short client control frame");
    size_t n=static_cast<unsigned char>(f[1])&127;
    Check((static_cast<unsigned char>(f[1])&128)!=0,"unmasked client frame");
    Check(n<=125 && f.size()==n+6,"bad client control length");
    std::string p;for(size_t i=0;i<n;i++)p.push_back(f[i+6]^f[2+i%4]);
    return {static_cast<unsigned char>(f[0])&15,p};
}
int main(){
    unsigned failures=0,passed=0;
    auto test=[&](const char* name,auto body){try{body();++passed;std::cout<<"PASS "<<name<<"\n";}catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<"\n";}};
    test("default limit stays 2048",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,std::string(2048,'a')));
        Check(f.received.size()==1,"2048 rejected");
        f.Emit(Frame(1,std::string(2049,'a')));
        Check(f.errors==1&&!f.socket.IsConnected(),"default limit not enforced");
    });
    test("configured 8192 byte JSON",[]{
        Fixture f(8192);f.Connect();
        auto json=std::string("{\"text\":\"")+std::string(8181,'x')+"\"}";
        Check(json.size()==8192,"test JSON size");f.Emit(Frame(1,json));
        Check(f.received==std::vector<std::string>{json},"8192 JSON mismatch");
    });
    test("TCP splits at every header and payload boundary",[]{
        auto wire=Frame(1,std::string(128,'x'));
        for(size_t split=1;split<wire.size();++split){
            Fixture f(8192);f.Connect();f.Emit(wire.substr(0,split));
            Check(f.received.empty(),"delivered incomplete frame");f.Emit(wire.substr(split));
            Check(f.received==std::vector<std::string>{std::string(128,'x')},"split decode failed");
        }
    });
    test("many coalesced valid frames",[]{
        Fixture f;f.Connect();std::string wire;for(int i=0;i<6000;i++)wire+=Frame(1,"x");
        f.Emit(wire);Check(f.received.size()==6000&&f.errors==0,"coalesced receive overflow");
    });
    test("UTF8 split across continuation frames",[]{
        Fixture f(8192);f.Connect();std::string text=u8"开始审批🙂，请确认。";
        f.Emit(Frame(1,text.substr(0,2),false)+Frame(0,text.substr(2),true));
        Check(f.received==std::vector<std::string>{text},"UTF8 bytes changed");
    });
    test("fragment state isolated between sockets",[]{
        Fixture a(8192),b(8192);a.Connect();b.Connect();
        a.Emit(Frame(1,"hello ",false));b.Emit(Frame(1,"other"));
        a.Emit(Frame(0,"world"));
        Check(a.received==std::vector<std::string>{"hello world"},"fragment state shared");
        Check(b.received==std::vector<std::string>{"other"},"second connection corrupted");
    });
    test("assembled message cannot exceed limit",[]{
        Fixture f(8192);f.Connect();f.Emit(Frame(1,std::string(5000,'a'),false));f.Emit(Frame(0,std::string(3193,'b')));
        Check(f.errors==1&&f.received.empty()&&!f.socket.IsConnected(),"oversized assembled message accepted");
    });
    test("oversize frame rejected from header",[]{
        Fixture f(8192);f.Connect();auto wire=Frame(1,std::string(8193,'a'));
        f.Emit(wire.substr(0,4));Check(f.errors==1&&!f.socket.IsConnected(),"waited for oversize payload");
    });
    test("64 bit lengths use unsigned bytes",[]{
        Fixture f(8192);f.Connect();std::string wire=Frame(1,std::string(128,'x'),true,127);
        for(char c:wire)f.Emit(std::string(1,c));
        Check(f.received==std::vector<std::string>{std::string(128,'x')},"64 bit length decode failed");
    });
    test("reconnect clears incomplete frame and fragments",[]{
        Fixture f(8192);f.Connect();f.Emit(Frame(1,"old",false));f.Emit(std::string(1,static_cast<char>(0x80)));
        f.network.tcp->Disconnect();f.Connect();f.Emit(Frame(1,"new"));
        Check(f.received==std::vector<std::string>{"new"}&&f.errors==0,"reconnect kept stale receive state");
    });
    test("ping interleaved between fragments preserves message",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,"a",false)+Frame(9,"ping")+Frame(0,"b"));
        Check(f.received==std::vector<std::string>{"ab"},"ping damaged fragmented message");
        auto reply=DecodeClient(f.network.tcp->sent.back());Check(reply.first==10&&reply.second=="ping","wrong pong payload");
    });
    test("remote close drops trailing data",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,"old",false)+Frame(8,"")+Frame(1,"ignored"));
        Check(f.disconnected==1&&f.received.empty()&&!f.socket.IsConnected(),"data delivered after close");
    });
    test("local close clears receive state",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,"old",false));f.socket.Close();f.Emit(Frame(0,"ignored"));
        Check(!f.socket.IsConnected()&&f.received.empty(),"local close kept receiver active");
    });
    test("continuation without start rejected",[]{
        Fixture f;f.Connect();f.Emit(Frame(0,"orphan"));Check(f.errors==1&&f.received.empty(),"orphan continuation accepted");
    });
    test("new data frame during fragmentation rejected",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,"a",false)+Frame(1,"b"));Check(f.errors==1&&f.received.empty(),"invalid interleave accepted");
    });
    test("fragmented control frame rejected",[]{
        Fixture f;f.Connect();f.Emit(Frame(9,"x",false));Check(f.errors==1,"fragmented ping accepted");
    });
    test("oversized control frame rejected",[]{
        Fixture f;f.Connect();f.Emit(Frame(9,std::string(126,'x')));Check(f.errors==1,"oversized ping accepted");
    });
    test("binary message retains embedded null",[]{
        Fixture f;f.Connect();std::string pcm("\0\1\0\2",4);f.Emit(Frame(2,pcm));
        Check(f.received==std::vector<std::string>{pcm}&&f.binary[0],"binary bytes corrupted");
    });
    test("handshake response can include first frame",[]{
        Fixture f;f.network.response=kHandshake+Frame(1,"welcome");f.Connect();
        Check(f.received==std::vector<std::string>{"welcome"},"first frame lost");
    });
    test("bad handshake does not connect",[]{
        Fixture f;f.network.response="HTTP/1.1 403 Forbidden\r\n\r\n";
        Check(!f.socket.Connect("ws://localhost/ws")&&!f.socket.IsConnected(),"403 accepted");
    });
    test("immediate handshake is not lost before callback registration",[]{
        Fixture f;f.network.queue_early=false;f.Connect();Check(f.socket.IsConnected(),"lost fast handshake");
    });
    test("Harness factory configures only its own 8K connection",[]{
        FakeNetwork n;auto socket=harness::CreateWebSocket(n,7);Check(bool(socket),"factory returned null");
        unsigned count=0;socket->OnData([&](const char*,size_t len,bool){Check(len==8192,"factory length");++count;});
        Check(socket->Connect("ws://localhost/ws"),"factory handshake");
        n.tcp->Emit(Frame(1,std::string(8192,'a')));Check(count==1,"factory did not set 8K");
        Fixture old;old.Connect();old.Emit(Frame(1,std::string(2049,'a')));Check(old.errors==1,"old default changed");
    });
    test("fragmented message exactly at limit",[]{
        Fixture f(8192);f.Connect();f.Emit(Frame(1,std::string(4096,'a'),false)+Frame(0,std::string(4096,'b')));
        Check(f.received==std::vector<std::string>{std::string(4096,'a')+std::string(4096,'b')},"limit fragment rejected");
    });
    test("Pong control callback is separate from application data",[]{
        Fixture f;unsigned pongs=0;f.socket.OnPong([&](){++pongs;});f.Connect();
        f.Emit(Frame(10,"")+Frame(1,"ok"));Check(pongs==1&&f.received==std::vector<std::string>{"ok"},"pong notification missing");
    });
    test("close from message callback prevents trailing delivery",[]{
        Fixture f;f.Connect();unsigned count=0;
        f.socket.OnData([&](const char*,size_t,bool){++count;f.socket.Close();});
        f.Emit(Frame(1,"one")+Frame(1,"two"));Check(count==1&&!f.socket.IsConnected(),"callback close unsafe");
    });
    test("transport close before handshake unblocks connect",[]{
        Fixture f;f.network.response="";Check(!f.socket.Connect("ws://localhost/ws"),"empty handshake connected");
    });
    test("invalid reserved opcode and RSV rejected",[]{
        Fixture a;a.Connect();a.Emit(Frame(3,"bad"));Check(a.errors==1,"reserved opcode accepted");
        Fixture b;b.Connect();auto f=Frame(1,"bad");f[0]|=0x40;b.Emit(f);Check(b.errors==1,"unnegotiated compression accepted");
    });
    test("JSON parser respects exact length without NUL terminator",[]{
        const char bytes[]={'{','"','v','"',':','1','}'};
        cJSON* root=harness::ParseJsonMessage(bytes,sizeof(bytes));Check(root!=nullptr,"bounded object parse failed");
        Check(cJSON_GetObjectItem(root,"v")->valueint==1,"wrong field");cJSON_Delete(root);
    });
    test("JSON parser rejects trailing data and embedded NUL",[]{
        Check(!harness::ParseJsonMessage("{}junk",6),"trailing garbage accepted");
        Check(!harness::ParseJsonMessage("{}{}",4),"two objects accepted");
        Check(!harness::ParseJsonMessage("{}\0garbage",10),"embedded NUL accepted");
        Check(!harness::ParseJsonMessage("[]",2),"array accepted");
        Check(!harness::ParseJsonMessage(nullptr,0),"null input accepted");
        auto text=std::string("{\"v\":\"")+std::string(8200,'x')+"\"}";
        Check(!harness::ParseJsonMessage(text.data(),text.size()),"oversized JSON accepted");
        cJSON* root=harness::ParseJsonMessage("{} \n",4);Check(root!=nullptr,"whitespace rejected");cJSON_Delete(root);
    });
    test("full size UTF8 JSON through Harness parser",[]{
        std::string text="{\"text\":\"";
        for(int i=0;i<2700;i++)text+=u8"中";
        text+=std::string(81,'a')+"\"}";
        Check(text.size()==8192,"UTF8 test size");
        cJSON* root=harness::ParseJsonMessage(text.data(),text.size());Check(root!=nullptr,"UTF8 8K parse failed");cJSON_Delete(root);
    });
    test("legacy 2K frames can still assemble longer messages",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,std::string(2000,'a'),false)+Frame(0,std::string(2000,'b')));
        Check(f.received==std::vector<std::string>{std::string(2000,'a')+std::string(2000,'b')},"legacy fragment cap regressed");
    });
    test("legacy assembly has finite 65535 byte cap",[]{
        Fixture f;f.Connect();f.Emit(Frame(1,std::string(2000,'a'),false));
        for(int i=0;i<32;i++)f.Emit(Frame(0,std::string(2000,'b'),false));
        Check(f.errors==1&&f.received.empty(),"legacy assembly unbounded");
    });
    test("Harness factory enforces full message cap",[]{
        FakeNetwork n;auto s=harness::CreateWebSocket(n,7);unsigned errors=0;
        s->OnError([&](int){++errors;});Check(s->Connect("ws://localhost/ws"),"factory connect");
        n.tcp->Emit(Frame(1,std::string(5000,'a'),false)+Frame(0,std::string(3193,'b')));
        Check(errors==1&&!s->IsConnected(),"factory missing assembly cap");
    });
    test("repeat errors notify once then recover on reconnect",[]{
        Fixture f(8192);
        for(int i=0;i<25;i++){
            f.Connect();f.Emit(Frame(0,"invalid")+Frame(0,"ignored"));
            Check(f.errors==i+1,"error duplicated");
        }
        f.Connect();f.Emit(Frame(1,"recovered"));Check(f.received==std::vector<std::string>{"recovered"},"error recovery failed");
    });
    test("handshake header length bound",[]{
        Fixture f;f.network.response="HTTP/1.1 101 Switching Protocols\r\nX: "+std::string(8192,'a');
        Check(!f.socket.Connect("ws://localhost/ws")&&f.errors==1,"unbounded handshake accepted");
    });
    test("mixed messages with random TCP chunk boundaries",[]{
        Fixture f(8192);f.Connect();std::mt19937 random(267);
        std::vector<std::string> expected;std::string wire;
        for(int n=0;n<200;n++){
            std::string payload(random()%8193,'x');for(char& c:payload)c=static_cast<char>(random()%256);
            expected.push_back(payload);size_t mid=payload.size()/2;
            wire+=Frame(2,payload.substr(0,mid),false)+Frame(9,"heartbeat")+Frame(0,payload.substr(mid));
        }
        for(size_t pos=0;pos<wire.size();){
            size_t count=std::min(wire.size()-pos,size_t(1+random()%12000));f.Emit(wire.substr(pos,count));pos+=count;
        }
        Check(f.errors==0&&f.received==expected,"mixed chunk roundtrip corrupted");
        for(bool b:f.binary)Check(b,"binary flag lost");
    });
    std::cout<<passed<<" passed, "<<failures<<" failed\n";return failures?1:0;
}
