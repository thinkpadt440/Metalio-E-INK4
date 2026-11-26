#include "socket_io.h"
#include "json.h"
#include "transport.h"
#include <cJSON.h>
#include <cstring>
#include <network_interface.h>
#include <http.h>
#include <web_socket.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif
namespace harness {
SocketIo::SocketIo(NetworkInterface& network, std::function<void(const std::string&)> on_server_date)
    : network_(network), on_server_date_(std::move(on_server_date)) {}
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
    const bool connected = socket_->Connect(url.c_str());
#ifdef ESP_PLATFORM
    if (!connected) {
        const int transport_error = socket_->GetLastError();
        if (transport_error != 0) {
            ESP_LOGW("Harness", "WebSocket connect failed: %s (transport error %d)", url.c_str(), transport_error);
        } else {
            ESP_LOGW("Harness", "WebSocket connect failed: %s", url.c_str());
        }
    }
#endif
    return connected;
}
bool SocketIo::SendText(const std::string& json) {
    return !json.empty()&&json.size()<=kMaxJsonBytes&&socket_&&socket_->Send(json);
}
bool SocketIo::SendBinary(const void* data, size_t size) {
    return socket_ && data && size > 0 && size <= kMaxAudioChunkBytes && size % 2 == 0 &&
           socket_->Send(data, size, true);
}
void SocketIo::Ping(){if(socket_)socket_->Ping();}
void SocketIo::Close(){if(socket_){socket_->Close();socket_.reset();}}
HttpReply SocketIo::Request(const std::string& method,const std::string& path,const std::string& body) {
    HttpReply result;
    if(!ValidConfig(config_)||(method!="GET"&&method!="PUT"&&method!="PATCH")||
       path.rfind("/api/v1/",0)!=0||path.size()>4096||body.size()>kMaxJsonBytes||
       (method!="PATCH"&&!body.empty())){result.error="invalid_http_request";return result;}
    for(unsigned char c:path)if(c<=32||c==127){result.error="invalid_http_request";return result;}
    return RequestHttp(config_, method, path, body, 10000, 4);
}

ServerHealthResult SocketIo::CheckHealth(const Config& config) {
    if (!ValidConfig(config)) return {ServerHealth::InvalidAddress};
    // Use a separate HTTP socket so the test can run alongside the saved server's connection.
    const auto response = RequestHttp(config, "GET", "/health", "", 5000, 5);
    const int status = response.status;
    if (status <= 0) return {ServerHealth::Unreachable};
    const bool ok = status >= 200 && status < 300;
    if (!response.error.empty()) {
        return {ok ? ServerHealth::InvalidResponse : ServerHealth::HttpError, status};
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        ParseJsonMessage(response.body.data(), response.body.size()), cJSON_Delete);
    const auto* data = cJSON_GetObjectItemCaseSensitive(json.get(), "data");
    const auto* health = cJSON_IsObject(data) ? data : json.get();
    const auto* health_status = cJSON_GetObjectItemCaseSensitive(health, "status");
    const auto* harness = cJSON_GetObjectItemCaseSensitive(health, "harness_connected");
    if (cJSON_IsString(health_status) && cJSON_IsFalse(harness)) {
        return {ServerHealth::HarnessUnavailable, status};
    }
    if (!ok) return {ServerHealth::HttpError, status};
    if (!cJSON_IsString(health_status) || std::strcmp(health_status->valuestring, "ok") != 0 ||
        (harness && !cJSON_IsBool(harness))) return {ServerHealth::InvalidResponse, status};
    return {cJSON_IsTrue(harness) ? ServerHealth::Healthy : ServerHealth::GatewayOnly, status};
}

HttpReply SocketIo::RequestHttp(const Config& config, const std::string& method, const std::string& path,
                               const std::string& body, int timeout_ms, int socket_id) {
    HttpReply result;
    const auto started=std::chrono::steady_clock::now();
    auto remaining=[&](){return timeout_ms-std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count();};
    auto http=network_.CreateHttp(socket_id);if(!http){result.error="http_unavailable";return result;}
    struct Guard {Http* http;~Guard(){http->Close();}} guard{http.get()};
    http->SetTimeout(timeout_ms);http->SetKeepAlive(false);
    http->SetHeader("X-Device-ID",config.device_id);
    http->SetHeader("Accept","application/json");
    http->SetHeader("Cache-Control","no-store");
    if(method=="PATCH"){http->SetHeader("Content-Type","application/json");http->SetContent(std::string(body));}
    if(!http->Open(method,config.base_url+path)){result.error="http_transport";return result;}
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
    if (on_server_date_ && result.status >= 200 && result.status < 300) {
        on_server_date_(http->GetResponseHeader("date"));
    }
    return result;
}
}  // namespace harness
