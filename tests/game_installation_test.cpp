#include "core/game_installation.h"
#include "engine/addr.h"
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
    if (!fails) std::puts("game_installation_test: all checks passed");
    return fails ? 1 : 0;
}
