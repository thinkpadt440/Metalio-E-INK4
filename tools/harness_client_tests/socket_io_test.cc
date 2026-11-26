#include "harness/socket_io.h"
#include <network_interface.h>
#include <web_socket.h>
#include <algorithm>
#include <map>
#include <iostream>
#include <stdexcept>
using namespace harness;
static void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct Data {
    std::string body="{\"items\":[],\"next_cursor\":null}",type="application/json",length="35",url,method,content;
    std::map<std::string,std::string> headers;
    std::string date = "Wed, 25 Nov 2026 09:14:03 GMT";
    unsigned closes=0,reads=0;int status=200,timeout=0,id=0;
    bool open=true,read_error=false,keep_alive=true;
};
class TestHttp:public Http {
    Data& d;size_t pos=0;
public:
    explicit TestHttp(Data& data):d(data){}
    void SetTimeout(int n)override{d.timeout=n;}
    void SetHeader(const std::string& k,const std::string& v)override{d.headers[k]=v;}
    void SetContent(std::string&& value)override{d.content=value;}
    void SetKeepAlive(bool v)override{d.keep_alive=v;}
    bool Open(const std::string& method,const std::string& url)override{d.method=method;d.url=url;return d.open;}
    void Close()override{++d.closes;}
    int Read(char* buffer,size_t size)override{
        ++d.reads;if(d.read_error)return -1;
        auto n=std::min({size,d.body.size()-pos,size_t(127)});d.body.copy(buffer,n,pos);pos+=n;return n;
    }
    int Write(const char*,size_t)override{return -1;}
    int GetStatusCode()override{return d.status;}
    std::string GetResponseHeader(const std::string& key) const override {
        if (key == "content-type") return d.type;
        if (key == "content-length") return d.length;
        return key == "date" ? d.date : "";
    }
    size_t GetBodyLength()override{return d.body.size();}
    std::string ReadAll()override{throw std::runtime_error("unbounded ReadAll must not be used");}
    int GetLastError()override{return 0;}
};
struct TestTcp:Tcp {
    std::string request;
    bool Connect(const std::string&,int)override{connected_=true;return true;}
    void Disconnect()override{connected_=false;}
    int Send(const std::string& data)override{
        if(data.rfind("GET ",0)==0){request=data;stream_callback_("HTTP/1.1 101 Switching Protocols\r\n\r\n");}
        return data.size();
    }
    int GetLastError()override{return 0;}
};
struct Network:NetworkInterface {
    Data data;TestTcp* tcp=nullptr;int websocket_id=-1;
    std::unique_ptr<Http> CreateHttp(int id)override{data.id=id;return std::make_unique<TestHttp>(data);}
    std::unique_ptr<Tcp> CreateTcp(int id)override{Check(id==2,"wrong websocket socket id");auto p=std::make_unique<TestTcp>();tcp=p.get();return p;}
    std::unique_ptr<Tcp> CreateSsl(int id)override{return CreateTcp(id);}
    std::unique_ptr<WebSocket> CreateWebSocket(int id)override{websocket_id=id;return std::make_unique<WebSocket>(this,id);}
};
struct Fixture{
    Network n;SocketIo io{n};
    Fixture(){Config c{"http://127.0.0.1:18080","metalio_test","test","test_boot"};Check(io.Connect(c,{}),"adapter connect");n.data.length=std::to_string(n.data.body.size());}
};
int main(){
    unsigned passed=0,failed=0;
    auto test=[&](const char* name,auto fn){try{fn();++passed;std::cout<<"PASS "<<name<<"\n";}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<"\n";}};
    test("HTTP and WS headers have device identity without authentication",[]{
        Fixture f;auto reply=f.io.Request("GET","/api/v1/sync?sync_token=x");
        Check(reply.error.empty()&&reply.status==200&&reply.body==f.n.data.body,"GET failed");
        Check(f.n.tcp->request.find("X-Device-ID: metalio_test")!=std::string::npos,"WS device id missing");
        Check(f.n.tcp->request.find("Authorization")==std::string::npos,"WS auth added");
        Check(f.n.data.headers["X-Device-ID"]=="metalio_test"&&f.n.data.headers.count("Authorization")==0,"HTTP headers");
        Check(f.n.data.closes==1&&f.n.data.id==4&&f.n.websocket_id==2,"resource cleanup/socket ids");
        Check(!f.n.data.keep_alive&&f.n.data.timeout>0&&f.n.data.timeout<=10000,"HTTP timeout/keepalive");
    });
    test("server date comes from a successful bounded response", [] {
        Network network;
        network.data.length = std::to_string(network.data.body.size());
        unsigned calls = 0;
        SocketIo io(network, [&](const std::string& date) {
            ++calls;
            Check(date == network.data.date, "date header lost");
        });
        Check(io.Connect({"http://127.0.0.1:18080", "metalio_test", "test", "test_boot"}, {}), "connect");
        Check(io.Request("GET", "/api/v1/sync").error.empty() && calls == 1, "date not published");
        network.data.status = 503;
        Check(io.Request("GET", "/api/v1/sync").status == 503 && calls == 1, "error response published date");
    });
    test("PATCH encodes body and PUT carries no body",[]{
        Fixture f;auto reply=f.io.Request("PATCH","/api/v1/sessions/s/settings","{\"model_id\":\"x\"}");
        Check(reply.error.empty()&&f.n.data.content=="{\"model_id\":\"x\"}"&&f.n.data.headers["Content-Type"]=="application/json","PATCH body");
        Fixture g;Check(g.io.Request("PUT","/api/v1/devices/x/default-casual-session").error.empty()&&g.n.data.content.empty(),"PUT body");
    });
    test("8192 byte HTTP response is allowed",[]{
        Fixture f;f.n.data.body=std::string(8192,'x');f.n.data.length="8192";
        auto reply=f.io.Request("GET","/api/v1/sync");Check(reply.error.empty()&&reply.body.size()==8192,"8K rejected");
    });
    test("known oversized HTTP body rejected before reading",[]{
        Fixture f;f.n.data.length="8193";auto reply=f.io.Request("GET","/api/v1/sync");
        Check(reply.error=="http_too_large"&&f.n.data.reads==0&&f.n.data.closes==1,"oversize not rejected early");
    });
    test("unknown length body remains bounded",[]{
        Fixture f;f.n.data.length="";f.n.data.body=std::string(8193,'x');
        auto reply=f.io.Request("GET","/api/v1/sync");Check(reply.error=="http_too_large"&&reply.body.size()<=8192,"unknown body unbounded");
    });
    test("truncated body detected",[]{
        Fixture f;f.n.data.length="100";Check(f.io.Request("GET","/api/v1/sync").error=="http_truncated","truncation missed");
    });
    test("open read and content errors close the HTTP object",[]{
        Fixture a;a.n.data.open=false;Check(a.io.Request("GET","/api/v1/sync").error=="http_transport"&&a.n.data.closes==1,"open failure leaked");
        Fixture b;b.n.data.read_error=true;Check(b.io.Request("GET","/api/v1/sync").error=="http_read"&&b.n.data.closes==1,"read failure leaked");
        Fixture c;c.n.data.type="text/html";Check(c.io.Request("GET","/api/v1/sync").error=="http_content_type"&&c.n.data.closes==1,"content failure leaked");
    });
    test("status errors are not disguised as success",[]{
        Fixture f;f.n.data.status=503;auto reply=f.io.Request("GET","/api/v1/sync");Check(reply.status==503,"status lost");
    });
    test("invalid method body and header injection are rejected",[]{
        Fixture f;Check(f.io.Request("POST","/api/v1/sync").error=="invalid_http_request","POST accepted");
        Check(f.io.Request("GET","https://other.example/").error=="invalid_http_request","absolute URL accepted");
        Check(f.io.Request("GET","/api/v1/x\r\nY: z").error=="invalid_http_request","header injection");
        Check(f.io.Request("GET","/api/v1/sync","{}").error=="invalid_http_request","GET body accepted");
    });
    test("invalid or enormous content length rejected",[]{
        for(const char* len:{"-1","NaN","184467440737095516160","9999999999999999999999999"}) {
            Fixture f;f.n.data.length=len;Check(!f.io.Request("GET","/api/v1/sync").error.empty(),"bad length accepted");
        }
    });
    std::cout<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
