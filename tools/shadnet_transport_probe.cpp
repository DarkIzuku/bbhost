// Exercises the production shadNet transport against an isolated server.
// No guest ABI, renderer, user's config, saves or persisted account is used.
// Credentials are private stdin JSON, never command line arguments or output.
#include "core/config.h"
#include "net/account.h"
#include "net/http.h"
#include "net/shadnet.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
namespace {
std::string endpoint,name,token;
HostConfig settings;
}
const HostConfig& config() {return settings;}
std::string config_value(const std::string& key) {return key=="online.transport" ? "shadnet" : key=="online.shadnet_server" ? endpoint : "";}
std::string host_options_file() {return "shadnet-probe-unused-options.toml";}
namespace net {
void account_set(const std::string& n,const std::string& t) {name=n;token=t;}
void account_clear() {name.clear();std::fill(token.begin(),token.end(),'\0');token.clear();}
void account_mark_refused(const std::string&) {account_clear();}
std::string account_token() {return token;}
std::string str_of(const json::Value& object,const char* key,const std::string& fallback) {
    const auto* v=object.find(key);return v && v->type==json::Value::Type::String ? v->string : fallback;
}
long long int_of(const json::Value& object,const char* key,long long fallback) {
    const auto* v=object.find(key);if(!v) return fallback;
    if(v->type==json::Value::Type::String) return std::strtoll(v->string.c_str(),nullptr,10);
    return v->type==json::Value::Type::Number ? static_cast<long long>(v->number) : fallback;
}
}
int main(int argc,char** argv) {
    if(argc!=2) {std::cerr << "Usage: shadnet_transport_probe tcp://host:port < private-input.jsonl\n";return 2;}
    endpoint=argv[1];settings.online_verify_tls=true;
    std::string line;
    while(std::getline(std::cin,line)) {
        if(line.size()>16384) return 3;
        json::Value request,reply=json::Value::make_object();std::string error,detail;
        bool ok=json::parse(line,request,error);
        std::fill(line.begin(),line.end(),'\0');
        const auto action=net::str_of(request,"action");
        if(ok && action=="login") {
            auto password=net::str_of(request,"password"),validation=net::str_of(request,"validation");
            ok=net::shadnet_login(net::str_of(request,"name"),password,validation,false,detail,error);
            std::fill(password.begin(),password.end(),'\0');std::fill(validation.begin(),validation.end(),'\0');
        } else if(ok && action=="logout") {net::shadnet_logout();}
        else if(ok) {
            const auto* body=request.find("body");
            ok=net::shadnet_request(net::str_of(request,"path"),body ? *body : json::Value{},reply,error,3000);
        }
        request={}; // Never echo input or credential/token fields.
        json::Value result=json::Value::make_object();result.set("ok",ok);result.set("error",error);result.set("reply",reply);
        std::cout << json::dump(result,0) << std::endl;
    }
    net::shadnet_logout();return 0;
}
