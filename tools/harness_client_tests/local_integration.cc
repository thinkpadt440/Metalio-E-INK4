#include "harness/client.h"
#include "harness/socket_io.h"
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
int main(int argc,char** argv){
    try{
        Need(argc==2,"provide local mock URL");LocalNetwork network;SocketIo io(network);
        uint64_t now=0;Client client(io,{argv[1],"metalio_test","host-test","local_boot"},[&](){return now;});
        std::set<std::string> connections;unsigned events=0;bool saw_backoff=false;
        auto started=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-started<std::chrono::seconds(12)){
            if(network.tcp)network.tcp->Poll();
            client.Tick(true);auto status=client.Status();
            if(status.state==ConnectionState::Backoff)saw_backoff=true;
            if(client.Ready()){
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
