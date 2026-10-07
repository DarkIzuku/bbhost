#include "host/launcher_bridge.h"
#include "host/launcher_gpu.h"
#include "core/config.h"
#include "core/game_installation.h"
#include "host/options.h"
#include "engine/patch_manifest.h"
#include "bbhost_version.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
const char* boolean(bool v) { return v ? "true" : "false"; }
}  // namespace

bool launcher_bridge_command(int argc, char** argv, int* result) {
    std::string command, game, action, name, code;
    std::vector<char*> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--launcher-state") command = "state";
        else if (arg == "--launcher-gpu") command = "gpu";
        else if (arg == "--prepare-game") {
            command = "prepare";
            if (i + 1 < argc) game = argv[++i];
        } else if (arg == "--launcher-account") {
            command = "account";
            if (i + 1 < argc) action = argv[++i];
        } else if (arg == "--account-name") { if (i + 1 < argc) name = argv[++i]; }
        else if (arg == "--account-code") { if (i + 1 < argc) code = argv[++i]; }
        else args.push_back(argv[i]);
    }
    if (command.empty()) return false;
    if (command == "gpu") { std::puts(launcher_gpu_json().c_str()); *result = 0; return true; }
    HostConfig c;
    std::string err;
    if (!config_load(static_cast<int>(args.size()), args.data(), &c, &err)) {
        std::printf("{\"ok\":false,\"error\":%s}\n", quote(err).c_str());
        *result = 2; return true;
    }
    if (command == "prepare") {
        const GameInstallation r = game_prepare(game, c.data);
        std::printf("{\"ok\":%s,\"prepared\":%s,\"app0\":%s,\"eboot\":%s,\"version\":%s,\"title_id\":%s,\"sha256\":%s,\"error\":%s}\n",
                    boolean(r.ok), boolean(r.prepared), quote(r.app0).c_str(), quote(r.eboot).c_str(),
                    quote(r.version).c_str(), quote(r.title_id).c_str(), quote(r.sha256).c_str(), quote(r.error).c_str());
        *result = r.ok ? 0 : 3; return true;
    }
    host_options_load();
    if (command == "account") {
        int which = action == "create" ? 1 : action == "recover" ? 2 : action == "website" ? 3 : action == "signout" ? 4 : -1;
        if (which < 0) { std::puts("{\"ok\":false,\"error\":\"Unknown account action\"}"); *result = 2; return true; }
        host_account_action(which, name, code);
        // Existing account actions use a worker and open the browser themselves.
        // Do not log account tokens or put them in this machine-readable response.
        std::string last_detail, last_outcome;
        while (host_account_busy()) {
            const std::string detail = host_account_detail(), outcome = host_account_outcome();
            if (detail != last_detail || outcome != last_outcome) {
                std::printf("{\"ok\":true,\"account\":%s,\"outcome\":%s,\"detail\":%s}\n",
                            quote(host_account_signed_in()).c_str(), quote(outcome).c_str(), quote(detail).c_str());
                std::fflush(stdout); last_detail = detail; last_outcome = outcome;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (host_account_take_save()) host_options_save_now();
        std::printf("{\"ok\":true,\"account\":%s,\"outcome\":%s,\"detail\":%s}\n",
                    quote(host_account_signed_in()).c_str(), quote(host_account_outcome()).c_str(), quote(host_account_detail()).c_str());
        *result = 0; return true;
    }
    std::string json = "{\"ok\":true,\"version\":" + quote(BBHOST_VERSION) + ",\"commit\":" + quote(BBHOST_GIT_REV) +
                       ",\"config_file\":" + quote(config_user_file()) + ",\"options_file\":" + quote(host_options_file()) +
                       ",\"app0\":" + quote(c.app0) + ",\"data\":" + quote(c.data) +
                       ",\"mods\":" + quote(c.mods.empty() ? c.data + "/mods" : c.mods) +
                       ",\"account\":" + quote(host_account_signed_in()) + ",\"options\":[";
    bool first = true;
    for (const HostOptionInfo& o : host_options_describe()) {
        if (!first) json += ',';
        first = false;
        json += "{\"key\":" + quote(o.key) + ",\"label\":" + quote(o.label) + ",\"section\":" + quote(o.section) +
                ",\"note\":" + quote(o.note) + ",\"restart\":" + boolean(o.restart) + ",\"index\":" + std::to_string(o.index) + ",\"values\":[";
        for (std::size_t i = 0; i < o.values.size(); ++i) { if (i) json += ','; json += quote(o.values[i]); }
        json += "]}";
    }
    json += "],\"patches\":[";
    first = true;
    for (const PatchListing& p : patch_manifests_list(c.mods.empty() ? c.data + "/mods" : c.mods)) {
        if (!first) json += ',';
        first = false;
        json += "{\"name\":" + quote(p.name) + ",\"description\":" + quote(p.description) + ",\"option\":" + quote(p.option) +
                ",\"enabled\":" + boolean(!patch_manifest_off(p.name)) + '}';
    }
    json += "]}";
    std::puts(json.c_str());
    *result = 0; return true;
}
