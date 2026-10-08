// Shared upstream folder policy for config, filesystem and one-folder preparation.
#include "core/config.h"
#include <cstring>
#include <filesystem>

GameFolders config_game_folders(const std::string& app0) {
    GameFolders g;
    std::string a = app0;
    while (a.size() > 1 && (a.back() == '/' || a.back() == '\\')) a.pop_back();
    if (a.empty()) return g;
    const auto dir = [](const std::string& p) {
        std::error_code ec;
        return std::filesystem::is_directory(std::filesystem::u8path(p), ec);
    };
    static const char* const suffixes[] = {"-UPDATE", "-patch"};
    for (const char* s : suffixes) {
        const std::size_t n = std::strlen(s);
        if (a.size() > n && a.compare(a.size() - n, n, s) == 0 && dir(a.substr(0, a.size() - n) + "/dvdroot_ps4")) {
            g.base = a.substr(0, a.size() - n); g.update = a; return g;
        }
    }
    g.base = a;
    for (const char* s : suffixes) if (dir(a + s)) { g.update = a + s; break; }
    return g;
}
