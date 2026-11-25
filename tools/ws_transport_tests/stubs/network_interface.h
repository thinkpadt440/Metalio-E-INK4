#pragma once
#include <memory>
#include "tcp.h"
#include "http.h"
class WebSocket;
class NetworkInterface {
public:
 virtual ~NetworkInterface()=default;
 virtual std::unique_ptr<Http> CreateHttp(int=-1){return nullptr;}
 virtual std::unique_ptr<Tcp> CreateTcp(int)=0;
 virtual std::unique_ptr<Tcp> CreateSsl(int)=0;
 virtual std::unique_ptr<WebSocket> CreateWebSocket(int)=0;
};
