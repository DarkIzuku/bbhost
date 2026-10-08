#include "net/shadnet_wire.h"
#include <cassert>
#include <iostream>
using namespace net::shadwire;
template<class F> void rejects(F f) {bool rejected=false;try {f();}catch(const std::runtime_error&) {rejected=true;}assert(rejected);}
int main() {
    // ServerInfo is the deployed server's literal 19-byte handshake.
    const std::string handshake("\x03\0\0\x13\0\0\0\0\0\0\0\0\0\0\0\x01\0\0\0",19);
    auto h=header(handshake);assert(h.type==3 && h.size==19 && h.id==0 && le(handshake,15,4)==1);
    const std::string login("\x0a\x05Izuku\x12\x04test",13);
    auto p=Proto::parse(login);assert(p.text(1)=="Izuku" && p.text(2)=="test");
    Encode e;e.text(1,"Izuku").text(2,"test");assert(e.bytes==login);
    auto request=packet(0,0,0x0123456789abcdefull,blob(login));
    h=header(request);assert(h.size==32 && h.id==0x0123456789abcdefull && unblob(std::string_view(request).substr(15))==login);
    // Additive fields and all protobuf scalar forms retain v1 compatibility.
    e.number(400,UINT64_MAX).text(500,"optional future field");p=Proto::parse(e.bytes);
    assert(p.text(1)=="Izuku" && p.number(400)==UINT64_MAX && p.number(999,7)==7);
    rejects([]{header(std::string(14,'\0'));});
    auto bad=handshake;bad[3]=14;rejects([&]{header(bad);});
    bad=handshake;bad[3]=0;bad[4]=0;bad[5]=0;bad[6]=1;rejects([&]{header(bad);});
    bad=handshake;bad[0]=4;rejects([&]{header(bad);});
    rejects([]{Proto::parse(std::string("\x0a\x20x",3));});
    rejects([]{Proto::parse(std::string("\x08\x80",2));});
    rejects([]{Proto::parse(std::string("\0",1));});
    rejects([]{Proto::parse(std::string("\x08\xff\xff\xff\xff\xff\xff\xff\xff\xff\x02",11));});
    rejects([]{unblob(std::string("\x05\0\0\0x",5));});
    // Protobuf's last singular field wins, rather than an older server value.
    Encode duplicates;duplicates.number(1,0).number(1,1);assert(Proto::parse(duplicates.bytes).number(1)==1);
    std::cout<<"shadNet v1 framing/protobuf checks passed\n";
}
