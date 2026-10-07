#include "core/game_installation.h"
#include "core/sfo.h"
#include "core/sha256.h"
#include "engine/addr.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kLimit = 512ull * 1024 * 1024;

void span(const std::vector<std::uint8_t>& b, std::uint64_t at, std::uint64_t n) {
    if (at > b.size() || n > b.size() - at) throw std::runtime_error("The executable has an invalid segment range.");
}
std::uint64_t number(const std::vector<std::uint8_t>& b, std::uint64_t at, unsigned n) {
    span(b, at, n);
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(b[at + i]) << (i * 8);
    return v;
}
std::vector<std::uint8_t> read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot read the executable in the selected game folder.");
    const auto n = f.tellg();
    if (n < 64 || static_cast<std::uint64_t>(n) > kLimit) throw std::runtime_error("The executable size is unsupported.");
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(b.data()), n)) throw std::runtime_error("The executable could not be read completely.");
    return b;
}
bool dump(const fs::path& p) {
    return fs::is_directory(p / "dvdroot_ps4") && fs::is_regular_file(p / "eboot.bin") &&
           fs::is_regular_file(p / "sce_sys" / "param.sfo");
}
fs::path resolve(const std::string& selected) {
    if (selected.empty()) throw std::runtime_error("Select the Bloodborne folder.");
    fs::path p = fs::weakly_canonical(fs::u8path(selected));
    if (p.filename() == "dvdroot_ps4") p = p.parent_path();
    if (dump(p)) return p;
    std::vector<fs::path> matches;
    if (fs::is_directory(p)) {
        unsigned seen = 0;
        for (const auto& e : fs::directory_iterator(p)) {
            if (++seen > 128) throw std::runtime_error("Select the game folder inside this installation.");
            if (e.is_directory() && dump(e.path())) matches.push_back(e.path());
        }
    }
    if (matches.size() == 1) return fs::weakly_canonical(matches[0]);
    throw std::runtime_error(matches.empty() ? "The folder must contain dvdroot_ps4, sce_sys/param.sfo and eboot.bin."
                                            : "This folder contains several dumps. Select one game folder.");
}
std::string path_text(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
bool beneath(const fs::path& p, const fs::path& root) {
    const auto r = p.lexically_relative(root);
    return !r.empty() && *r.begin() != "..";
}
}  // namespace

std::vector<std::uint8_t> game_extract_self(const std::vector<std::uint8_t>& s) {
    if (number(s, 0, 4) != 0x1d3d154f) throw std::runtime_error("The executable is neither ELF nor a supported SELF.");
    const auto count = number(s, 24, 2);
    if (!count || count > 256) throw std::runtime_error("The SELF segment table is unsupported.");
    const auto base = 32 + count * 32;
    span(s, base, 64);
    if (number(s, base, 4) != 0x464c457f || number(s, base + 4, 3) != 0x010102 ||
        number(s, base + 18, 2) != 62) throw std::runtime_error("The SELF does not contain a little-endian x86-64 ELF.");
    const auto phoff = number(s, base + 32, 8);
    const auto phnum = number(s, base + 56, 2);
    if (number(s, base + 54, 2) != 56 || !phnum || phnum > 255 || phoff > kLimit - phnum * 56)
        throw std::runtime_error("The ELF program headers are unsupported.");
    const auto header_end = phoff + phnum * 56;
    span(s, base, header_end);
    struct Program { std::uint64_t type, offset, size; };
    std::vector<Program> ph;
    std::uint64_t end = header_end;
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const auto at = base + phoff + i * 56;
        Program p{number(s, at, 4), number(s, at + 8, 8), number(s, at + 32, 8)};
        if (p.offset > kLimit || p.size > kLimit - p.offset) throw std::runtime_error("The extracted ELF exceeds its size limit.");
        end = std::max(end, p.offset + p.size);
        ph.push_back(p);
    }
    std::vector<std::uint8_t> elf(static_cast<std::size_t>(end), 0);
    std::copy_n(s.begin() + base, header_end, elf.begin());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{0, header_end}};
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto at = 32 + i * 32;
        const auto flags = number(s, at, 8);
        if (!(flags & 0x800)) continue;
        if (flags & 0xa) throw std::runtime_error("This dump still has encrypted or compressed executable segments. Export a clear game dump from your own console.");
        const auto index = (flags >> 20) & 4095;
        if (index >= ph.size()) throw std::runtime_error("The SELF has an invalid program index.");
        const auto offset = number(s, at + 8, 8), size = number(s, at + 16, 8), mem = number(s, at + 24, 8);
        const Program& p = ph[index];
        if (size != p.size || size != mem) throw std::runtime_error("Blocked SELF segments are unsupported.");
        span(s, offset, size);
        std::copy_n(s.begin() + offset, size, elf.begin() + p.offset);
        ranges.emplace_back(p.offset, p.offset + size);
    }
    for (const Program& p : ph) {
        if (!p.size) continue;
        const bool covered = std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) {
            return r.first <= p.offset && p.offset + p.size <= r.second;
        });
        if (!covered && (p.type == 1 || p.type == 2 || p.type == 0x61000000 || p.type == 0x61000010))
            throw std::runtime_error("A required executable segment is missing from this dump.");
    }
    return elf;
}

GameInstallation game_prepare(const std::string& selected, const std::string& data) {
    GameInstallation r;
    try {
        const fs::path app0 = resolve(selected);
        r.app0 = path_text(app0);
        std::map<std::string, SfoValue> values;
        if (!sfo_read(path_text(app0 / "sce_sys" / "param.sfo"), &values, &r.error)) return r;
        r.version = values["APP_VER"].text;
        r.title_id = values["TITLE_ID"].text;
        if (r.version != "01.09") throw std::runtime_error("Bloodborne 1.09 is required. Detected version: " + r.version);
        const fs::path source = app0 / "eboot.bin";
        const auto bytes = read(source);
        std::vector<std::uint8_t> extracted;
        const bool is_elf = number(bytes, 0, 4) == 0x464c457f;
        if (!is_elf) extracted = game_extract_self(bytes);
        const auto& elf = is_elf ? bytes : extracted;
        r.sha256 = sha256_hex(elf.data(), elf.size());
        if (!eboot_is_109(r.sha256)) throw std::runtime_error("The executable is not a verified Bloodborne 1.09 image (SHA-256 " + r.sha256 + ").");
        if (is_elf) {
            r.eboot = path_text(source);
        } else {
            if (data.empty()) throw std::runtime_error("A writable data folder is required for game preparation.");
            const fs::path cache = fs::weakly_canonical(fs::u8path(data) / "cache" / "game" /
                                   sha256_hex(bytes.data(), bytes.size()));
            if (beneath(cache, app0)) throw std::runtime_error("Choose a data folder outside the original game dump.");
            const fs::path target = cache / "eboot.elf";
            bool valid_cache = false;
            if (fs::is_regular_file(target)) {
                try { const auto saved = read(target); valid_cache = sha256_hex(saved.data(), saved.size()) == r.sha256; } catch (...) {}
            }
            if (!valid_cache) {
                fs::create_directories(cache);
                const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
                const fs::path temp = cache / ("eboot-" + std::to_string(nonce) + ".tmp");
                try {
                    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
                    f.close();
                    if (!f) throw std::runtime_error("Cannot save the prepared executable in the data cache.");
                    std::error_code ec;
                    // Only this derived, verified cache file may be replaced.
                    fs::remove(target, ec);
                    fs::rename(temp, target);
                } catch (...) { std::error_code ec; fs::remove(temp, ec); throw; }
            }
            r.eboot = path_text(target);
            r.prepared = true;
        }
        r.ok = true;
    } catch (const std::exception& e) { r.error = e.what(); }
    return r;
}
