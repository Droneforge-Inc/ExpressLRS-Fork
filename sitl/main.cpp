#include "firmware.h"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/stat.h>
struct Listener {
    int fd=-1;std::string path;bool owned=false;
    ~Listener(){if(fd>=0)close(fd);if(owned)unlink(path.c_str());}
};
static void request_stop(int) { sitl::stop_requested=1; }
int main(int argc,char **argv) {
    if(argc!=3 || std::string(argv[1])!="--socket") {
        std::cerr<<"Usage: "<<argv[0]<<" --socket /path/in/private/directory/elrs.sock\n";return 2;
    }
    Listener listener;listener.path=argv[2];
    try {
        struct sigaction action{};action.sa_handler=request_stop;sigemptyset(&action.sa_mask);
        sigaction(SIGTERM,&action,nullptr);sigaction(SIGINT,&action,nullptr);
        sockaddr_un addr{};addr.sun_family=AF_UNIX;
        if(listener.path.size()>=sizeof(addr.sun_path))throw std::runtime_error("socket path too long");
        std::memcpy(addr.sun_path,listener.path.c_str(),listener.path.size()+1);
        listener.fd=socket(AF_UNIX,SOCK_STREAM,0);
        if(listener.fd<0)throw std::runtime_error(std::strerror(errno));
        umask(0077);
        // Never unlink an existing socket; it might belong to another simulator.
        if(bind(listener.fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)throw std::runtime_error(std::strerror(errno));
        listener.owned=true;
        if(listen(listener.fd,1)<0)throw std::runtime_error(std::strerror(errno));
        std::cout<<"READY "<<listener.path<<std::endl;
        int fd=accept(listener.fd,nullptr,nullptr);
        if(fd<0)throw std::runtime_error(std::strerror(errno));
        struct Client {int fd;~Client(){close(fd);}} client{fd};
        timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        sitl::Firmware firmware;sitl::Message request{};uint32_t next=1;
        sitl::Bytes previousRequest,previousResponse;
        while(sitl::read_message(fd,request)) {
            auto encoded=sitl::encode(request);
            if(request.seq==next-1 && !previousRequest.empty()) {
                if(encoded!=previousRequest)throw std::runtime_error("sequence reused with different data");
                sitl::write_all(fd,previousResponse);continue; // Retry without repeating firmware side effects.
            }
            if(request.seq!=next || next==UINT32_MAX)throw std::runtime_error("unexpected sequence");
            sitl::Message response{static_cast<uint16_t>(request.op|0x8000),request.seq,request.time,{}};
            bool success=false;
            try {
                auto result=firmware.handle(request.op,request.time,request.data);
                sitl::put(response.data,0,4);response.data.insert(response.data.end(),result.begin(),result.end());success=true;
            }catch(const std::exception &error) {
                sitl::put(response.data,1,4);auto msg=std::string(error.what());response.data.insert(response.data.end(),msg.begin(),msg.end());
            }
            previousRequest=std::move(encoded);previousResponse=sitl::encode(response);++next;
            sitl::write_all(fd,previousResponse);
            if(request.op==sitl::Quit && success)break;
        }
        return 0;
    }catch(const std::exception &e){std::cerr<<"ELRS SITL: "<<e.what()<<'\n';return sitl::stop_requested?0:1;}
}
