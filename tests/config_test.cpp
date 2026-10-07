// core/config.cpp: a per-user bbhost.toml as older releases left it, loaded
// by this one - notes after quoted values, the live server's move to https,
// and servers of the player's own left as they are.
#include "core/config.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

namespace fs = std::filesystem;

fs::path g_dir;

std::string read(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// The file as the player's config, loaded the way bbhost starts.
HostConfig load(const std::string& toml) {
    {
        std::ofstream out(g_dir / "config" / "bbhost.toml", std::ios::trunc);
        out << toml;
    }
    char arg0[] = "config_test";
    char* argv[] = {arg0, nullptr};
    HostConfig c;
    std::string err;
    CHECK(config_load(1, argv, &c, &err));
    return c;
}

bool is_live_https(const HostConfig& c) {
    return c.online_host == "thehuntersdream.com" && c.online_scheme == "https" && c.online_verify_tls &&
           c.online_require_account && c.auth_server == "https://thehuntersdream.com";
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/bbhost-config-test-XXXXXX";
    if (!mkdtemp(tmpl)) return 1;
    g_dir = tmpl;
    fs::create_directories(g_dir / "config");
    fs::create_directories(g_dir / "work");
    setenv("BBHOST_CONFIG_DIR", (g_dir / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (g_dir / "data").c_str(), 1);
    // No bbhost.toml in the working directory to layer over the user's.
    if (chdir((g_dir / "work").c_str()) != 0) return 1;

    // A note after a quoted value is not part of it.
    {
        HostConfig c = load("[online]\nhost = \"192.168.1.50\"  # added by bbhost v0.2.0: the server\n"
                            "scheme = \"http\" # a comment\n");
        CHECK(c.online_host == "192.168.1.50");
        CHECK(c.online_scheme == "http");
    }
    // A Windows path keeps its backslashes, a trailing one included.
    {
        HostConfig c = load("[paths]\napp0 = \"C:\\Games\\Bloodborne\\\"  # the dump\n");
        CHECK(c.app0 == "C:\\Games\\Bloodborne\\");
    }
    // The first-start template of older releases: plain http to the live server.
    {
        HostConfig c = load("[online]\nhost = \"thehuntersdream.com\"\nscheme = \"http\"\nverify_tls = false\n"
                            "require_account = false\n# auth_server = \"https://thehuntersdream.com\"\n");
        CHECK(is_live_https(c));
        const std::string f = read(g_dir / "config" / "bbhost.toml");
        CHECK(f.find("scheme = \"https\"") != std::string::npos);
        CHECK(f.find("verify_tls = true") != std::string::npos);
        CHECK(f.find("require_account = true") != std::string::npos);
        CHECK(f.find("\nauth_server = \"https://thehuntersdream.com\"") != std::string::npos);
    }
    // Saved by an older setup window's "Live server".
    {
        HostConfig c = load("[online]\nhost = \"thehuntersdream.com\"\nscheme = \"http\"\nverify_tls = false\n"
                            "require_account = false\nauth_server = \"\"\n\n[bbhost]\nconfig_version = 2\n");
        CHECK(is_live_https(c));
    }
    // No online section at all: the keys bbhost adds, then the live server's.
    {
        HostConfig c = load("[paths]\napp0 = \"PATH-TO-THE-GAME-DUMP\"\n");
        CHECK(is_live_https(c));
    }
    // A server on the player's network: left alone, and a missing scheme is http.
    {
        HostConfig c = load("[online]\nhost = \"192.168.1.50\"\nscheme = \"http\"\nverify_tls = false\n");
        CHECK(c.online_host == "192.168.1.50" && c.online_scheme == "http" && !c.online_verify_tls);
        c = load("[online]\nhost = \"192.168.1.50\"\n");
        CHECK(c.online_scheme == "http");
        CHECK(read(g_dir / "config" / "bbhost.toml").find("scheme = \"http\"") != std::string::npos);
    }
    // The playtest server keeps its own settings.
    {
        HostConfig c = load("[online]\nhost = \"dev.thehuntersdream.com\"\nscheme = \"https\"\nverify_tls = true\n"
                            "require_account = true\nauth_server = \"https://dev.thehuntersdream.com\"\n");
        CHECK(c.online_host == "dev.thehuntersdream.com" && c.auth_server == "https://dev.thehuntersdream.com");
    }
    // Saved by this release's setup window: a choice, not rewritten.
    {
        HostConfig c = load("[online]\nhost = \"thehuntersdream.com\"\nscheme = \"http\"\n\n[bbhost]\nconfig_version = 3\n");
        CHECK(c.online_scheme == "http");
    }

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("config_test: ok\n");
    return 0;
}
