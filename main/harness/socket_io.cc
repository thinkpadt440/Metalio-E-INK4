#include "socket_io.h"
#include "transport.h"
#include <network_interface.h>
#include <http.h>
#include <web_socket.h>
#include <algorithm>
#include <cctype>
#include <chrono>
namespace harness {
SocketIo::SocketIo(NetworkInterface& network):network_(network){}
SocketIo::~SocketIo(){Close();}
bool SocketIo::Connect(const Config& config,Callbacks cb) {
    Close();if(!ValidConfig(config))return false;config_=config;
    socket_=CreateWebSocket(network_,2);if(!socket_)return false;
    socket_->SetHeader("X-Device-ID",config.device_id.c_str());
    socket_->OnData([text=cb.text,error=cb.error](const char* data,size_t length,bool binary){
        if(binary||length>kMaxJsonBytes){if(error)error();return;}
        if(text)text(std::string(data,length));
    });
    socket_->OnPong(std::move(cb.pong));
    socket_->OnDisconnected(std::move(cb.disconnected));
    socket_->OnError([error=std::move(cb.error)](int){if(error)error();});
    std::string url=config.base_url;url.replace(0,4,"ws");url+="/api/v1/ws";
    return socket_->Connect(url.c_str());
}
bool SocketIo::SendText(const std::string& json) {
    return !json.empty()&&json.size()<=kMaxJsonBytes&&socket_&&socket_->Send(json);
}
void SocketIo::Ping(){if(socket_)socket_->Ping();}
void SocketIo::Close(){if(socket_){socket_->Close();socket_.reset();}}
HttpReply SocketIo::Request(const std::string& method,const std::string& path,const std::string& body) {
    HttpReply result;
    if(!ValidConfig(config_)||(method!="GET"&&method!="PUT"&&method!="PATCH")||
       path.rfind("/api/v1/",0)!=0||path.size()>4096||body.size()>kMaxJsonBytes||
       (method!="PATCH"&&!body.empty())){result.error="invalid_http_request";return result;}
    for(unsigned char c:path)if(c<=32||c==127){result.error="invalid_http_request";return result;}
    const auto started=std::chrono::steady_clock::now();
    auto remaining=[&](){return 10000-std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count();};
    auto http=network_.CreateHttp(4);if(!http){result.error="http_unavailable";return result;}
    struct Guard {Http* http;~Guard(){http->Close();}} guard{http.get()};
    http->SetTimeout(10000);http->SetKeepAlive(false);
    http->SetHeader("X-Device-ID",config_.device_id);
    http->SetHeader("Accept","application/json");
    http->SetHeader("Cache-Control","no-store");
    if(method=="PATCH"){http->SetHeader("Content-Type","application/json");http->SetContent(std::string(body));}
    if(!http->Open(method,config_.base_url+path)){result.error="http_transport";return result;}
    result.status=http->GetStatusCode();
    auto content_type=http->GetResponseHeader("content-type");
    std::transform(content_type.begin(),content_type.end(),content_type.begin(),[](unsigned char c){return std::tolower(c);});
    auto semicolon=content_type.find(';');if(semicolon!=std::string::npos)content_type.resize(semicolon);
    while(!content_type.empty()&&content_type.back()==' ')content_type.pop_back();
    if(content_type!="application/json"){result.error="http_content_type";return result;}
    const auto declared=http->GetResponseHeader("content-length");
    size_t expected=0;
    if(!declared.empty()){
        for(char c:declared){
            if(c<'0'||c>'9'||expected>(kMaxJsonBytes-static_cast<unsigned>(c-'0'))/10){result.error="http_too_large";return result;}
            expected=expected*10+(c-'0');
        }
        if(expected>kMaxJsonBytes){result.error="http_too_large";return result;}
    }
    char chunk[1024];
    while(true){
        const auto budget=remaining();
        if(budget<=0){result.error="http_timeout";return result;}
        http->SetTimeout(static_cast<int>(budget));
        int n=http->Read(chunk,sizeof(chunk));
        if(n<0){result.error="http_read";return result;}
        if(n==0)break;
        if(static_cast<size_t>(n)>sizeof(chunk)||result.body.size()+n>kMaxJsonBytes){result.error="http_too_large";return result;}
        result.body.append(chunk,static_cast<size_t>(n));
    }
    if(!declared.empty()&&result.body.size()!=expected){result.error="http_truncated";return result;}
    return result;
}
}  // namespace harness
