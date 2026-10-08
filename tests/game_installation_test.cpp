#include "core/game_installation.h"
#include "engine/addr.h"
#include "core/sha256.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
int fails = 0;
void check(bool ok, const char* what) { if (!ok) { ++fails; std::printf("FAIL %s\n", what); } }
void put(std::vector<std::uint8_t>& b, unsigned at, std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::vector<std::uint8_t> clear_self() {
    std::vector<std::uint8_t> b(600, 0);
    put(b, 0, 0x1d3d154f, 4); put(b, 24, 1, 2);
    put(b, 32, 0x800, 8); put(b, 40, 512, 8); put(b, 48, 8, 8); put(b, 56, 8, 8);
    put(b, 64, 0x464c457f, 4); put(b, 68, 0x010102, 3); put(b, 82, 62, 2);
    put(b, 96, 64, 8); put(b, 118, 56, 2); put(b, 120, 1, 2);
    put(b, 128, 1, 4); put(b, 136, 256, 8); put(b, 160, 8, 8);
    for (unsigned i = 0; i < 8; ++i) b[512 + i] = static_cast<std::uint8_t>(i + 1);
    return b;
}
void rejects(std::vector<std::uint8_t> b, const char* why) {
    try { (void)game_extract_self(b); check(false, why); } catch (const std::runtime_error&) {}
}
void sfo(const std::filesystem::path& p, const char* version, const char* title = "CUSA00900") {
    std::string entries, keys, data, head("\0PSF", 4);
    const auto put_string = [](std::string& s, std::uint32_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) s.push_back(static_cast<char>(v >> (8 * i))); };
    for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{{"APP_VER", version}, {"TITLE_ID", title}, {"CATEGORY", "gp"}}) {
        const auto len = static_cast<std::uint32_t>(v.size() + 1), cap = (len + 3) & ~3u;
        put_string(entries, static_cast<std::uint32_t>(keys.size()), 2); put_string(entries, 0x0204, 2);
        put_string(entries, len, 4); put_string(entries, cap, 4); put_string(entries, static_cast<std::uint32_t>(data.size()), 4);
        keys += k + '\0'; data += v + std::string(cap - v.size(), '\0');
    }
    while (keys.size() % 4) keys.push_back('\0');
    put_string(head, 0x0101, 4); put_string(head, 20 + static_cast<std::uint32_t>(entries.size()), 4);
    put_string(head, 20 + static_cast<std::uint32_t>(entries.size() + keys.size()), 4); put_string(head, 3, 4);
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << head << entries << keys << data;
}
void update_folders() {
    namespace fs = std::filesystem;
    const auto owner = fs::weakly_canonical(fs::current_path());
    const auto root = owner / ("game-prepare-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto base = root / "CUSA00900", update = root / "CUSA00900-UPDATE", data = root / "cache-data";
    fs::create_directories(base / "dvdroot_ps4"); sfo(base / "sce_sys/param.sfo", "01.00");
    auto old = clear_self(); old[512] = 12;
    auto current = clear_self();
    const auto write = [](const fs::path& p, const std::vector<std::uint8_t>& bytes) {
        fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    write(base / "eboot.bin", old); write(update / "eboot.bin", current); sfo(update / "sce_sys/param.sfo", "01.09");
    const auto extracted = game_extract_self(current);
    const auto wanted = sha256_hex(extracted.data(), extracted.size());
    for (const auto& selected : {base, update, root, base / "dvdroot_ps4"}) {
        const auto r = game_prepare(selected.generic_string(), data.generic_string());
        check(r.app0 == base.generic_string() && r.update == update.generic_string() && r.version == "01.09" && r.sha256 == wanted,
              "base, update, wrapper and dvdroot selections use the effective update image");
        check(!r.ok && r.error.find("not a verified") != std::string::npos, "sibling update cannot bypass executable hash validation");
    }
    sfo(update / "sce_sys/param.sfo", "01.09", "CUSA99999");
    check(game_prepare(base.generic_string(), data.generic_string()).error.find("different game/title ID") != std::string::npos,
          "reject update for another title ID");
    sfo(update / "sce_sys/param.sfo", "01.08");
    check(game_prepare(base.generic_string(), data.generic_string()).error.find("Detected version: 01.08") != std::string::npos,
          "reject incompatible effective update version");
    sfo(update / "sce_sys/param.sfo", "01.09");
    fs::rename(update, root / "CUSA00900-patch");
    check(game_prepare(base.generic_string(), data.generic_string()).sha256 == wanted, "support upstream -patch update spelling");
    const auto second = root / "CUSA03173"; fs::create_directories(second / "dvdroot_ps4");
    sfo(second / "sce_sys/param.sfo", "01.09", "CUSA03173"); write(second / "eboot.bin", current);
    check(game_prepare(root.generic_string(), data.generic_string()).error.find("several dumps") != std::string::npos,
          "a second distinct base installation remains ambiguous");
    {
        std::ifstream f(base / "eboot.bin", std::ios::binary);
        const std::vector<std::uint8_t> saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(saved == old, "preparation leaves the source untouched");
    }
    if (fs::weakly_canonical(root).parent_path() == owner) fs::remove_all(root);
}
}  // namespace
int main() {
    const auto source = clear_self();
    const auto out = game_extract_self(source);
    check(out.size() == 264 && out[0] == 0x7f && out[256] == 1 && out[263] == 8, "clear SELF extracts headers and program bytes");
    check(source[512] == 1, "input remains untouched");
    auto b = source; put(b, 32, 0x802, 8); rejects(b, "encrypted data rejected");
    b = source; put(b, 32, 0x808, 8); rejects(b, "compressed data rejected");
    b = source; put(b, 32, 0x100800, 8); rejects(b, "invalid program index rejected");
    b = source; put(b, 48, 7, 8); rejects(b, "blocked segment layout rejected");
    b = source; put(b, 40, 598, 8); rejects(b, "source overrun rejected");
    b = source; put(b, 136, 512ull * 1024 * 1024, 8); rejects(b, "output overflow rejected");
    b = source; put(b, 32, 0, 8); rejects(b, "missing load segment rejected");
    b.resize(60); rejects(b, "truncated header rejected");
    check(!eboot_is_109("01.09") && !eboot_is_109(std::string(64, '0')), "version text cannot bypass identity gate");
    check(eboot_hashes_compatible(kEboot109Sha256, kEboot109ClearSelfSha256), "known serializations share patch compatibility");
    check(!eboot_hashes_compatible(kEboot109Sha256, std::string(64, '0')), "unknown hash cannot inherit address patches");
    update_folders();
    if (!fails) std::puts("game_installation_test: all checks passed");
    return fails ? 1 : 0;
}
