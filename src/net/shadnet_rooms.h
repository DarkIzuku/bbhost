#pragma once
#include "net/shadnet_wire.h"
#include "replay/json.h"
namespace net::shadwire {
inline json::Value member(const Proto& p) {
    json::Value r=json::Value::make_object();r.set("OnlineId",p.text(1));r.set("MemberId",static_cast<unsigned>(p.number(2)));
    r.set("Addr",p.text(10));r.set("Port",static_cast<unsigned>(p.number(11)));
    r.set("LocalAddr",p.text(10));r.set("LocalPort",static_cast<unsigned>(p.number(11)));return r;
}
inline json::Value room_reply(const Proto& p,bool joining) {
    const auto id=p.number(1),mid=p.number(joining?2:5),slots=p.number(joining?3:6);
    if(!id || id>INT64_MAX || !mid || mid>65535 || !slots || slots>65535) throw std::runtime_error("Invalid shadNet room identity");
    json::Value r=json::Value::make_object();r.set("ResKind",0);r.set("RoomId",std::to_string(id));r.set("SessionId",std::to_string(id));
    r.set("MemberId",static_cast<unsigned>(mid));r.set("MaxMembers",static_cast<unsigned>(slots));
    const auto details=Proto::parse(p.text(joining?6:9));
    r.set("OwnerMemberId",static_cast<unsigned>(details.number(4,joining?1:mid)));
    json::Value members=json::Value::make_array();
    for(const auto& f:details.fields) if(f.tag==2 && f.wire==2) members.push(member(Proto::parse(f.bytes)));
    r.set("Members",std::move(members));return r;
}
inline json::Value room_event(const Proto& p) {
    json::Value r=p.text(6).empty()?json::Value::make_object():member(Proto::parse(p.text(6)));
    const auto id=p.number(2);if(!id || id>INT64_MAX) throw std::runtime_error("Invalid shadNet event room");
    r.set("RoomId",std::to_string(id));
    switch(p.number(3)) {
    case 0x1101:r.set("Name","room_member_joined");break;
    // shadNet/Matching2's KICKOUT_ACTION is 2 (LEAVE_ACTION is 1).
    case 0x1102:r.set("Name","room_member_left");r.set("Reason",p.number(4)==2?"kicked":"left");break;
    case 0x1103:r.set("Name","room_member_kicked");r.set("Reason","kicked");break;
    case 0x1104:r.set("Name","room_destroyed");r.set("Reason","host_left");break;
    default:r.set("Name","shadnet_room_update");break;
    }return r;
}
}
