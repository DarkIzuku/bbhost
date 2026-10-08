#include "net/shadnet_wire.h"
#include "net/shadnet_rooms.h"
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
    Encode host;host.text(1,"Host").number(2,1).number(4,1).text(10,"192.0.2.1").number(11,9307);
    Encode guest;guest.text(1,"Guest").number(2,2).text(10,"192.0.2.2").number(11,9308);
    Encode details;details.text(2,host.bytes).text(2,guest.bytes).number(3,2).number(4,1);
    Encode room;room.number(1,9007199254740993ull).number(2,2).number(3,5).text(6,details.bytes);
    const auto response=room_reply(Proto::parse(room.bytes),true);
    assert(response.find("RoomId")->string=="9007199254740993" && response.find("SessionId")->string=="9007199254740993");
    assert(response.find("Members")->array.size()==2 && response.find("Members")->array[1].find("Port")->number==9308);
    Encode event;event.number(1,1).number(2,42).number(3,0x1101).text(6,guest.bytes);
    assert(room_event(Proto::parse(event.bytes)).find("Name")->string=="room_member_joined");
    event.number(3,0x1103);assert(room_event(Proto::parse(event.bytes)).find("Name")->string=="room_member_kicked");
    rejects([]{room_reply(Proto{},true);});
    std::cout<<"shadNet v1 framing/protobuf checks passed\n";
}
