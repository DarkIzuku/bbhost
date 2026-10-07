#include "decomp/decomp.h"

#include "core/elf.h"
#include "core/memory.h"
#include "core/portable.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "log.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using ull = unsigned long long;

std::vector<DecompFunction> g_added;

struct Placed {
    const DecompFunction* fn;
    bool compared;
};
std::vector<Placed> g_placed;
bool g_comparing = false;
std::uint64_t g_slide = kPreferredGuestSlide;

// Trampolines are carved from executable pages of our own: the displaced
// instructions, then jmp [rip+0] and the address to resume at.
std::uint8_t* g_page = nullptr;
std::size_t g_page_used = 0;
constexpr std::size_t kPage = 0x1000;

void* trampoline(const std::uint8_t* displaced, std::size_t n, std::uint64_t resume) {
    const std::size_t need = (n + 14 + 15) & ~std::size_t{15};
    if (!g_page || g_page_used + need > kPage) {
        g_page = static_cast<std::uint8_t*>(host_page_alloc(kPage, true));
        g_page_used = 0;
        if (!g_page) return nullptr;
    }
    std::uint8_t* t = g_page + g_page_used;
    g_page_used += need;
    std::memcpy(t, displaced, n);
    t[n] = 0xff;
    t[n + 1] = 0x25;
    std::memset(t + n + 2, 0, 4);
    std::memcpy(t + n + 6, &resume, 8);
    return t;
}

// `name` in a comma-separated list.
bool listed(const char* list, const char* name) {
    if (!list) return false;
    const std::size_t len = std::strlen(name);
    for (const char* p = list; *p;) {
        const char* end = std::strchr(p, ',');
        const std::size_t n = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
        if (n == len && std::strncmp(p, name, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

bool place(ElfImage* image, const DecompFunction& fn, bool compare) {
    const std::uint64_t at = image->mem.slide + (fn.bn - kPreferredGuestSlide);
    if (fn.entry_len < 14 || at < image->mem.slide || at + fn.entry_len > image->mem.slide + image->mem.size) return false;
    auto* p = static_cast<std::uint8_t*>(guest_ptr(image->mem, at));
    if (!p || std::memcmp(p, fn.entry, fn.entry_len) != 0) {
        host_log("decomp: %s refused, 0x%llx does not hold the instructions it was written against; the game's stays",
                 fn.name, static_cast<ull>(fn.bn));
        return false;
    }
    void* original = trampoline(fn.entry, fn.entry_len, at + fn.entry_len);
    if (!original) return false;
    void* dest = compare                              ? thunk_wrap(fn.compare)
                 : fn.kind == DecompKind::Leaf        ? fn.ours
                 : fn.kind == DecompKind::HostedFrame ? thunk_wrap_capture_frame(fn.ours)
                                                      : thunk_wrap(fn.ours);
    const std::uint64_t lo = at & ~0xfffull, hi = (at + fn.entry_len + 0xfff) & ~0xfffull;
    if (!guest_protect_rwx(&image->mem, lo, hi - lo)) return false;
    // The trampoline is published before the jump that can lead to it.
    if (fn.original) *fn.original = original;
    const std::uint64_t d = reinterpret_cast<std::uint64_t>(dest);
    std::uint8_t jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(jump + 6, &d, 8);
    std::memset(p + 14, 0xcc, fn.entry_len - 14);
    std::memcpy(p, jump, sizeof(jump));
    guest_protect_rx(&image->mem, lo, hi - lo);
    return true;
}

}  // namespace

void decomp_add(const DecompFunction& fn) { g_added.push_back(fn); }

bool decomp_comparing() { return g_comparing; }

std::uint64_t decomp_guest(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }

void decomp_install(ElfImage* image) {
    if (!image || !eboot_is_109(image->sha256) || g_added.empty()) return;
    g_slide = image->mem.slide;
    if (const char* e = std::getenv("BBHOST_DECOMP"); e && e[0] == '0') {
        host_log("decomp: off (BBHOST_DECOMP=0): the game's own code for all %zu functions", g_added.size());
        return;
    }
    const char* off = std::getenv("BBHOST_DECOMP_OFF");
    const char* cmp = std::getenv("BBHOST_DECOMP_COMPARE");
    g_comparing = cmp && cmp[0] == '1';
    std::string ours, kept;
    for (const DecompFunction& fn : g_added) {
        if (listed(off, fn.name)) {
            kept += kept.empty() ? fn.name : std::string(", ") + fn.name;
            continue;
        }
        const bool compare = g_comparing && fn.compare && fn.counts;
        if (!place(image, fn, compare)) {
            kept += kept.empty() ? fn.name : std::string(", ") + fn.name;
            continue;
        }
        g_placed.push_back({&fn, compare});
        ours += ours.empty() ? "" : ", ";
        ours += fn.name;
        if (compare) ours += " (compared)";
    }
    host_log("decomp: %zu of %zu functions ours: %s%s%s", g_placed.size(), g_added.size(), ours.empty() ? "none" : ours.c_str(),
             kept.empty() ? "" : "; the game's: ", kept.c_str());
}

void decomp_report() {
    for (const Placed& p : g_placed) {
        if (p.fn->report) p.fn->report();
        if (!p.compared) continue;
        host_log("decomp: %s (%s, 0x%llx): %llu calls compared with the game's, %llu differ", p.fn->name, p.fn->area,
                 static_cast<ull>(p.fn->bn), static_cast<ull>(p.fn->counts->calls.load()),
                 static_cast<ull>(p.fn->counts->differ.load()));
    }
}
