#include "net/shadnet.h"
#include "net/shadnet_wire.h"
#include "net/shadnet_rooms.h"
#include "net/account.h"
#include "net/http.h"
#include "core/config.h"
#include "host/options.h"
#include "log.h"
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/select.h>
#endif
#if defined(BBHOST_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace net {
namespace {
using namespace shadwire;
using Clock=std::chrono::steady_clock;
std::mutex mu;
struct Credentials {std::string name,password,validation;};
std::string endpoint() {return config_value("online.shadnet_server");}
std::string credential_file() {return host_options_file()+".shadnet";}
bool credential_store(const Credentials& c,std::string& error) {
#if defined(_WIN32)
    json::Value j=json::Value::make_object();j.set("server",endpoint());j.set("name",c.name);j.set("password",c.password);j.set("validation",c.validation);
    auto plain=json::dump(j,0);DATA_BLOB input{static_cast<DWORD>(plain.size()),reinterpret_cast<BYTE*>(plain.data())},encrypted{};
    if(!CryptProtectData(&input,L"Bloodborne shadNet",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)) {error="Windows could not protect the account password";return false;}
    SecureZeroMemory(plain.data(),plain.size());
    const std::string file=credential_file(),temp=file+".tmp";
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<char*>(encrypted.pbData),encrypted.cbData);out.close();LocalFree(encrypted.pbData);
    if(!out || !MoveFileExW(std::filesystem::path(temp).c_str(),std::filesystem::path(file).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {error="Protected account could not be saved";return false;}
    return true;
#else
    (void)c;error="Remembering shadNet passwords requires Windows DPAPI";return false;
#endif
}
bool credential_load(Credentials& c,std::string& error) {
#if defined(_WIN32)
    const auto file=credential_file();std::error_code ec;const auto size=std::filesystem::file_size(file,ec);
    if(ec || size>65536) {error="Sign in to shadNet in the launcher first";return false;}
    std::ifstream in(file,std::ios::binary);std::string encrypted(size,'\0');in.read(encrypted.data(),encrypted.size());
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),reinterpret_cast<BYTE*>(encrypted.data())},plain{};
    if(!in || !CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain)) {error="Protected account belongs to another Windows user or is damaged";return false;}
    std::string text(reinterpret_cast<char*>(plain.pbData),plain.cbData);SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
    json::Value j;bool ok=json::parse(text,j,error);SecureZeroMemory(text.data(),text.size());
    if(!ok || str_of(j,"server")!=endpoint()) {error="Sign in to this shadNet server in the launcher";return false;}
    c={str_of(j,"name"),str_of(j,"password"),str_of(j,"validation")};return true;
#else
    error="Sign in is not persisted on this platform";return false;
#endif
}
std::string error_message(unsigned code) {
    switch(code) {
    case 6:return "This account is already connected on another client";
    case 7:case 8:return "Invalid username or password (username is case sensitive)";
    case 9:return "This server requires the account validation token";
    case 14:return "The room no longer exists";
    case 16:return "The room is full";
    case 23:return "The server requires a new sign-in";
    case 33:return "This server has disabled the requested feature";
    default:return "shadNet refused the request (code "+std::to_string(code)+")";
    }
}
struct Client {
#if defined(BBHOST_HAVE_CURL)
    CURL* connection=nullptr;
#endif
    std::string spec,input;
    std::uint64_t next_id=0;
    bool authenticated=false,matching=false;
    unsigned last_error=0;
    std::uint64_t room_id=0,event_cursor=0;
    unsigned member_id=0;
    json::Value room_cache;
    std::deque<std::pair<unsigned,std::string>> notifications;
    ~Client() {close();}
    void close() {
#if defined(BBHOST_HAVE_CURL)
        if(connection) curl_easy_cleanup(connection);connection=nullptr;
#endif
        authenticated=false;matching=false;input.clear();notifications.clear();spec.clear();room_id=0;member_id=0;room_cache={};
    }
#if defined(BBHOST_HAVE_CURL)
    bool wait(bool write,Clock::time_point deadline,std::string& error) {
        curl_socket_t socket=CURL_SOCKET_BAD;curl_easy_getinfo(connection,CURLINFO_ACTIVESOCKET,&socket);
        if(socket==CURL_SOCKET_BAD) {error="shadNet connection closed";return false;}
#if !defined(_WIN32)
        if(socket>=FD_SETSIZE) {error="shadNet socket cannot be polled";return false;}
#endif
        fd_set fds;FD_ZERO(&fds);FD_SET(socket,&fds);
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0) {error="shadNet request timed out";return false;}
        timeval t{static_cast<long>(left/1000),static_cast<long>((left%1000)*1000)};
        const int ready=select(static_cast<int>(socket+1),write?nullptr:&fds,write?&fds:nullptr,nullptr,&t);
        if(ready<=0) {error=ready==0?"shadNet request timed out":"shadNet socket error";return false;}return true;
    }
    bool send(std::string_view data,Clock::time_point deadline,std::string& error) {
        while(!data.empty()) {
            std::size_t n=0;const auto rc=curl_easy_send(connection,data.data(),data.size(),&n);
            if(rc==CURLE_AGAIN) {if(!wait(true,deadline,error)) return false;continue;}
            if(rc!=CURLE_OK || !n) {error="shadNet connection lost while sending";return false;}
            data.remove_prefix(n);
        }return true;
    }
    bool receive(std::string& result,Clock::time_point deadline,std::string& error) {
        for(;;) {
            if(input.size()>=header_size) {const auto h=header(input);if(input.size()>=h.size) {result=input.substr(0,h.size);input.erase(0,h.size);return true;}}
            char buf[16384];std::size_t n=0;const auto rc=curl_easy_recv(connection,buf,sizeof(buf),&n);
            if(rc==CURLE_AGAIN) {if(!wait(false,deadline,error)) return false;continue;}
            if(rc!=CURLE_OK || !n) {error="shadNet connection lost while receiving";return false;}
            if(input.size()+n>max_packet) {error="Oversized shadNet receive buffer";return false;}input.append(buf,n);
        }
    }
#endif
    bool connect(std::string& error,int timeout) {
        if(spec==endpoint()
#if defined(BBHOST_HAVE_CURL)
            && connection
#endif
            ) return true;
        close();spec=endpoint();
#if defined(BBHOST_HAVE_CURL)
        std::string url;
        if(spec.rfind("tcp://",0)==0) url="http://"+spec.substr(6);
        else if(spec.rfind("tls://",0)==0) url="https://"+spec.substr(6);
        else {error="shadNet: use tcp://host:port or tls://host:port";close();return false;}
        CURLU* parsed=curl_url();
        if(!parsed || curl_url_set(parsed,CURLUPART_URL,url.c_str(),0)!=CURLUE_OK) {if(parsed) curl_url_cleanup(parsed);error="Invalid shadNet endpoint";close();return false;}
        bool valid=true;char* part=nullptr;
        for(auto key:{CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT}) {
            if(curl_url_get(parsed,key,&part,0)==CURLUE_OK) {valid=false;curl_free(part);part=nullptr;}
        }
        if(curl_url_get(parsed,CURLUPART_PATH,&part,0)==CURLUE_OK) {valid &= std::string(part)=="/";curl_free(part);part=nullptr;}
        valid &= curl_url_get(parsed,CURLUPART_PORT,&part,0)==CURLUE_OK;
        if(part) {const long port=std::strtol(part,nullptr,10);valid &= port>0 && port<=65535;curl_free(part);}
        curl_url_cleanup(parsed);
        if(!valid) {error="shadNet endpoint must be an explicit host:port, without credentials or path";close();return false;}
        static std::once_flag once;std::call_once(once,[]{curl_global_init(CURL_GLOBAL_DEFAULT);});
        connection=curl_easy_init();if(!connection) {error="Could not open shadNet";return false;}
        curl_easy_setopt(connection,CURLOPT_URL,url.c_str());curl_easy_setopt(connection,CURLOPT_CONNECT_ONLY,1L);
        // Direct game TCP transport, never an HTTP proxy or a redirect.
        curl_easy_setopt(connection,CURLOPT_PROXY,"");curl_easy_setopt(connection,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(connection,CURLOPT_TIMEOUT_MS,static_cast<long>(timeout));
        curl_easy_setopt(connection,CURLOPT_SSL_VERIFYPEER,config().online_verify_tls?1L:0L);
        curl_easy_setopt(connection,CURLOPT_SSL_VERIFYHOST,config().online_verify_tls?2L:0L);
        if(curl_easy_perform(connection)!=CURLE_OK) {error="Cannot connect to the shadNet TCP server";close();return false;}
        std::string packet;
        if(!receive(packet,Clock::now()+std::chrono::milliseconds(timeout),error)) {close();return false;}
        const auto h=header(packet);
        if(h.type!=3 || h.command || h.id || h.size!=19 || le(packet,15,4)!=1) {error="Unsupported shadNet protocol major; this client requires v1";close();return false;}
        host_log("shadNet: negotiated stable wire protocol v1");return true;
#else
        error="This build has no shadNet TCP transport (libcurl missing)";close();return false;
#endif
    }
    bool call(unsigned command,const std::string& proto,Proto& reply,std::string& error,int timeout=5000,bool encoded=true) {
        last_error=0;
#if defined(BBHOST_HAVE_CURL)
        if(!connect(error,timeout)) return false;
        const auto id=++next_id;
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeout);
        if(!send(packet(0,command,id,encoded?blob(proto):proto),deadline,error)) {close();return false;}
        for(;;) {
            std::string p;if(!receive(p,deadline,error)) {close();return false;}const auto h=header(p);
            if(h.type==2 && !h.id) {
                // Unknown additive notifications do not fill the room queue.
                if(h.command!=10) continue;
                if(notifications.size()>=1024) {error="shadNet notification queue overflow; reconnect required";close();return false;}
                notifications.emplace_back(h.command,p.substr(header_size));continue;
            }
            if(h.type!=1 || h.command!=command || h.id!=id || p.size()<16) {error="Mismatched shadNet reply";close();return false;}
            const unsigned status=le(p,15,1);if(status) {last_error=status;error=error_message(status);return false;}
            reply=p.size()==16?Proto{}:Proto::parse(unblob(std::string_view(p).substr(16)));return true;
        }
#else
        (void)command;(void)proto;(void)reply;(void)timeout;(void)encoded;error="shadNet requires libcurl";return false;
#endif
    }
    bool login(const Credentials& c,std::string& detail,std::string& error) {
        close();if(c.name.empty() || c.name.size()>16 || c.password.empty() || c.password.size()>4096 || c.validation.size()>4096) {error="Enter a shadNet username and password";return false;}
        Encode request;request.text(1,c.name).text(2,c.password).text(3,c.validation).text(4,"CUSA03173").text(5,"Bloodborne");
        Proto reply;if(!call(0,request.bytes,reply,error)) return false;
        if(!reply.number(2)) {error="shadNet returned an invalid account";close();return false;}
        authenticated=true;
        if(!call(39,{},reply,error,5000,false) || reply.text(1).empty() || reply.text(3)!=c.name) {close();return false;}
        account_set(c.name,reply.text(1));
        // Older v1 builds can lack optional feature discovery. Auth remains
        // usable for WebAPI; never assume multiplayer from a missing flag.
        const bool features=call(12,{},reply,error,5000,false);
        if(!features && last_error!=33 && last_error!=2) {close();account_clear();return false;}
        matching=features && reply.number(1)!=0;
        error.clear();detail=matching?"shadNet account connected; Matching2 advertised":"shadNet account connected; this server does not advertise Matching2";
        host_log("shadNet: authenticated; Matching2 capability=%u",matching?1u:0u);return true;
    }
    bool ensure(std::string& error) {
        if(authenticated && spec==endpoint()) return true;
        Credentials c;if(!credential_load(c,error)) return false;std::string detail;
        const bool ok=login(c,detail,error);
#if defined(_WIN32)
        SecureZeroMemory(c.password.data(),c.password.size());
#endif
        if(!ok) account_mark_refused(error);return ok;
    }
};
Client client;
}
bool shadnet_selected() {return config_value("online.transport")=="shadnet";}
bool shadnet_login(const std::string& name,const std::string& password,const std::string& token,bool remember,std::string& detail,std::string& error) {
    std::lock_guard lock(mu);
    try {
        if(!shadnet_selected()) {error="Apply a shadNet server profile first";return false;}
        Credentials c{name,password,token};
        if(!client.login(c,detail,error)) return false;
        c.validation=account_token();
        if(remember && !credential_store(c,error)) {client.close();account_clear();return false;}
        return true;
    } catch(const std::exception& e) {client.close();error=e.what();return false;}
}
void shadnet_logout() {std::lock_guard lock(mu);client.close();std::error_code ec;std::filesystem::remove(credential_file(),ec);account_clear();}
bool shadnet_request(const std::string& path,const json::Value& body,json::Value& reply,std::string& error,int timeout) {
    std::lock_guard lock(mu);
    try {
        if(!client.ensure(error)) return false;
        reply=json::Value::make_object();reply.set("ResKind",0);
        if(path=="/mp/matching2/context_start") {
            Encode request;request.number(1,1);Proto p;if(!client.call(100,request.bytes,p,error,timeout)) return false;
            reply.set("Matching2Enabled",client.matching);return true;
        }
        if(path=="/np/events/ack") return true;
        if(path=="/np/events/poll") {
            Proto p;if(!client.call(12,{},p,error,timeout,false)) return false;
            if(!client.notifications.empty()) {
                const auto n=std::move(client.notifications.front());client.notifications.pop_front();
                reply=room_event(Proto::parse(unblob(n.second)));
                reply.set("ResKind",0);reply.set("HasEvent",true);reply.set("EventId",std::to_string(++client.event_cursor));reply.set("NextCursor",std::to_string(client.event_cursor));
                const auto name=str_of(reply,"Name");
                if(name=="room_destroyed" || name=="room_member_kicked" ||
                   (name=="room_member_left" && int_of(reply,"MemberId",0)==client.member_id)) {client.room_id=0;client.member_id=0;client.room_cache={};}
                return true;
            }
            reply.set("HasEvent",false);reply.set("NextCursor",std::to_string(client.event_cursor));return true;
        }
        if(path=="/mp/matching2/heartbeat") {
            Proto p;if(!client.call(12,{},p,error,timeout,false)) return false;
            reply.set("InRoom",client.room_id!=0 && str_of(body,"SessionId")==std::to_string(client.room_id));return true;
        }
        if(path.rfind("/mp/matching2/session_blob?",0)==0) {
            if(!client.room_id) {error="No active shadNet room";return false;}reply=client.room_cache;return true;
        }
        if(path=="/np/signaling/resolve") {
            Encode request;request.text(1,str_of(body,"OnlineId"));Proto p;
            if(!client.call(105,request.bytes,p,error,timeout)) return false;
            reply.set("Addr",p.text(2));reply.set("Port",static_cast<unsigned>(p.number(3)));return true;
        }
        if(!client.matching) {error="This shadNet server has disabled Matching2";return false;}
        if(path=="/mp/matching2/create_room" || path=="/mp/matching2/join_room") {
            const bool joining=path=="/mp/matching2/join_room";Encode request;Proto p;
            if(joining) request.number(1,int_of(body,"RoomId",0)).number(2,1);
            else request.number(1,1).number(2,int_of(body,"MaxMembers",5)).number(4,1);
            if(!client.call(joining?102:101,request.bytes,p,error,timeout)) return false;
            reply=room_reply(p,joining);client.room_id=p.number(1);client.member_id=p.number(joining?2:5);client.room_cache=reply;return true;
        }
        if(path=="/mp/matching2/leave_room") {
            if(!client.room_id || str_of(body,"SessionId")!=std::to_string(client.room_id)) {error="The shadNet room already ended";return false;}
            Encode request;request.number(1,client.room_id).number(2,1);Proto p;
            if(!client.call(103,request.bytes,p,error,timeout)) return false;
            client.room_id=0;client.member_id=0;client.room_cache={};return true;
        }
        if(path=="/mp/matching2/kick_member") {
            if(!client.room_id || str_of(body,"SessionId")!=std::to_string(client.room_id)) {error="No active shadNet room";return false;}
            Encode request;request.number(1,client.room_id).number(2,1).number(3,int_of(body,"MemberId",0));Proto p;
            return client.call(110,request.bytes,p,error,timeout);
        }
        // Endpoint registration uses shadNet's own UDP discovery on the
        // existing P2P socket, not a fabricated address in an HTTP reply.
        if(path=="/mp/matching2/signaling_update") return true;
        error="Unsupported shadNet operation: "+path;return false;
    } catch(const std::exception& e) {client.close();error=e.what();return false;}
}
}
