#include "ipc.h"
#include <cerrno>
#include <cstring>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
namespace sitl {
volatile std::sig_atomic_t stop_requested=0;
using Clock=std::chrono::steady_clock;
uint64_t Reader::get(size_t bytes) {
    if (bytes>8 || bytes>data.size()-pos) throw std::runtime_error("truncated payload");
    uint64_t value=0; for(size_t i=0;i<bytes;++i) value=(value<<8)|data[pos++]; return value;
}
Bytes Reader::take(size_t bytes) {
    if(bytes>data.size()-pos) throw std::runtime_error("truncated payload");
    Bytes out(data.begin()+pos,data.begin()+pos+bytes);pos+=bytes;return out;
}
void Reader::end() const { if(pos!=data.size()) throw std::runtime_error("unexpected payload bytes"); }
void put(Bytes &data,uint64_t value,size_t bytes) {
    for(size_t i=bytes;i;--i) data.push_back((value>>(8*(i-1)))&255);
}
Bytes encode(const Message &m) {
    if(m.data.size()>max_payload) throw std::runtime_error("oversized response");
    Bytes b; put(b,magic,4);put(b,version,2);put(b,m.op,2);put(b,m.data.size(),4);
    put(b,m.seq,4);put(b,m.time,8);b.insert(b.end(),m.data.begin(),m.data.end());return b;
}
static bool read_exact(int fd,Bytes &out,size_t n,bool boundary,Clock::time_point deadline) {
    out.resize(n);size_t offset=0;
    while(offset<n) {
        if(stop_requested)throw std::runtime_error("stopped");
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<=0)throw std::runtime_error("IPC message deadline exceeded");
        pollfd ready{fd,POLLIN,0};auto status=poll(&ready,1,static_cast<int>(remaining));
        if(status<0 && errno==EINTR && !stop_requested)continue;
        if(status<=0)throw std::runtime_error("IPC message timed out or interrupted");
        auto count=recv(fd,out.data()+offset,n-offset,0);
        if(count<0 && errno==EINTR && !stop_requested) continue;
        if(count==0 && offset==0 && boundary) return false;
        if(count<=0) throw std::runtime_error(count==0?"truncated IPC message":std::strerror(errno));
        offset+=count;
    }
    return true;
}
bool read_message(int fd,Message &m) {
    auto deadline=Clock::now()+std::chrono::seconds(5);
    Bytes h;if(!read_exact(fd,h,24,true,deadline))return false;Reader r{h};
    if(r.get(4)!=magic || r.get(2)!=version)throw std::runtime_error("IPC magic/version mismatch");
    m.op=r.get(2);auto n=r.get(4);m.seq=r.get(4);m.time=r.get(8);
    if(n>max_payload)throw std::runtime_error("oversized IPC payload");
    read_exact(fd,m.data,n,false,deadline);return true;
}
void write_all(int fd,const Bytes &data) {
    size_t sent=0;while(sent<data.size()) {
        auto n=send(fd,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);
        if(n<0 && errno==EINTR && !stop_requested)continue;
        if(n<=0)throw std::runtime_error("IPC peer disconnected or timed out");
        sent+=n;
    }
}
}
