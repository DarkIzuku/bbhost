#pragma once
#include "replay/json.h"
#include <string>
namespace net {
bool shadnet_selected();
// Login is a website account, never a bbhost recovery code. Passwords enter
// through stdin and are persisted only with Windows' current-user DPAPI.
bool shadnet_login(const std::string& name,const std::string& password,const std::string& validation_token,
                   bool remember,std::string& detail,std::string& error);
void shadnet_logout();
bool shadnet_request(const std::string& path,const json::Value&,json::Value&,std::string& error,int timeout_ms);
}
