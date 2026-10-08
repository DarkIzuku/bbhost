// Stable shadNet v1 wire boundary. No emulator networking code is imported.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace net::shadwire {
constexpr std::size_t header_size=15, max_packet=8*1024*1024;
inline std::uint64_t le(std::string_view s, std::size_t at, unsigned n) {
    if(n>8 || at>s.size() || n>s.size()-at) throw std::runtime_error("Truncated shadNet packet");
    std::uint64_t v=0; for(unsigned i=0;i<n;++i) v|=std::uint64_t(static_cast<unsigned char>(s[at+i]))<<(8*i); return v;
}
inline void put(std::string& s,std::uint64_t v,unsigned n) {for(unsigned i=0;i<n;++i) s+=char(v>>(8*i));}
struct Header {unsigned type; std::uint16_t command; std::uint32_t size; std::uint64_t id;};
inline Header header(std::string_view s) {
    Header h{static_cast<unsigned>(le(s,0,1)),static_cast<std::uint16_t>(le(s,1,2)),static_cast<std::uint32_t>(le(s,3,4)),le(s,7,8)};
    if(h.type>3 || h.size<header_size || h.size>max_packet) throw std::runtime_error("Invalid shadNet packet header"); return h;
}
inline std::string packet(unsigned type,std::uint16_t command,std::uint64_t id,std::string_view payload) {
    if(type>3 || payload.size()>max_packet-header_size) throw std::runtime_error("Oversized shadNet packet");
    std::string r; put(r,type,1);put(r,command,2);put(r,payload.size()+header_size,4);put(r,id,8);r+=payload;return r;
}
inline std::string blob(std::string_view s) {std::string r;put(r,s.size(),4);r+=s;return r;}
inline std::string_view unblob(std::string_view s) {
    const auto n=le(s,0,4); if(n!=s.size()-4) throw std::runtime_error("Invalid shadNet protobuf length");return s.substr(4);
}
inline void varint(std::string& s,std::uint64_t v) {while(v>127) {s+=char((v&127)|128);v>>=7;}s+=char(v);}
inline std::uint64_t varint(std::string_view s,std::size_t& at) {
    std::uint64_t v=0;for(unsigned i=0;i<10;++i) {
        if(at==s.size()) throw std::runtime_error("Truncated shadNet varint");
        const auto b=static_cast<unsigned char>(s[at++]);
        if(i==9 && b>1) throw std::runtime_error("Overflowing shadNet varint");
        v|=std::uint64_t(b&127)<<(7*i);if(!(b&128)) return v;
    }throw std::runtime_error("Invalid shadNet varint");
}
struct Proto {
    struct Field {unsigned tag,wire;std::uint64_t integer=0;std::string bytes;};
    std::vector<Field> fields;
    static Proto parse(std::string_view s) {
        Proto p;std::size_t at=0;
        while(at<s.size()) {
            const auto key=varint(s,at);if(!key || key>>3>0x1fffffff) throw std::runtime_error("Invalid shadNet protobuf tag");
            Field f{static_cast<unsigned>(key>>3),static_cast<unsigned>(key&7)};
            if(f.wire==0) f.integer=varint(s,at);
            else if(f.wire==1 || f.wire==5) {const unsigned n=f.wire==1?8:4;f.integer=le(s,at,n);at+=n;}
            else if(f.wire==2) {const auto n=varint(s,at);if(n>s.size()-at) throw std::runtime_error("Truncated shadNet field");f.bytes=s.substr(at,n);at+=n;}
            else throw std::runtime_error("Unsupported shadNet protobuf wire type");
            if(p.fields.size()>=65536) throw std::runtime_error("Too many shadNet fields");
            p.fields.push_back(std::move(f));
        }return p;
    }
    std::uint64_t number(unsigned tag,std::uint64_t d=0) const {for(auto i=fields.rbegin();i!=fields.rend();++i) if(i->tag==tag && i->wire==0) return i->integer;return d;}
    std::string text(unsigned tag) const {for(auto i=fields.rbegin();i!=fields.rend();++i) if(i->tag==tag && i->wire==2) return i->bytes;return {};}
};
struct Encode {
    std::string bytes;
    Encode& number(unsigned tag,std::uint64_t v) {varint(bytes,std::uint64_t(tag)<<3);varint(bytes,v);return *this;}
    Encode& text(unsigned tag,std::string_view s) {varint(bytes,(std::uint64_t(tag)<<3)|2);varint(bytes,s.size());bytes+=s;return *this;}
};
}
