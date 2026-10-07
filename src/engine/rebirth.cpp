#include "engine/rebirth.h"

#include "core/config.h"
#include "core/elf.h"
#include "core/portable.h"
#include "core/thunk.h"
#include "engine/addr.h"
#include "engine/event_flags.h"
#include "engine/menu_steps.h"
#include "engine/params.h"
#include "engine/player_data.h"
#include "engine/rebirth_script.h"
#include "engine/world_chr.h"
#include "gcn/container.h"
#include "hle/fs.h"
#include "log.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Bumped whenever the rewrite changes, so installs remake the archive.
constexpr const char* kStamp = "altar-rebirth 2";  // 2: no "Channel Blood Echoes" in the leave choice
constexpr const char* kArchive = "dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx";
constexpr const char* kScript = "t242307.esd";
constexpr std::uint32_t kAltarBlock = 0x18020000;  // m24_02_00_00: the Altar of Despair's map
constexpr std::uint64_t kApplyStats = 0x2013d00;   // the level-up's apply (the YES of "Spend ... Blood Echoes")
constexpr std::uint64_t kWorldChrMan = 0x593e878;  // main player ChrIns at +0x60
constexpr std::int64_t kEchoCap = 999999999;       // the game's own limit
constexpr int kMenuWaitFrames = 120;               // the level-up menu not open by then: give up on it
constexpr const char* kStats[6] = {"vitality", "endurance", "strength", "skill", "bloodtinge", "arcane"};

struct Build {
    std::int64_t level = 0, stat[6] = {}, echoes = 0;
};

bool g_on = false;
std::uint64_t g_slide = 0;
int g_watch = -1;          // the level-up menu's steps (engine/menu_steps.h)
bool g_loading = true;     // no world since the last check: the flags are cleared when it comes back
bool g_have_snapshot = false;
Build g_snapshot;          // the hunter before the rebirth, for "Undo Rebirth"
std::uint64_t g_menu = 0;  // the level-up menu's step, while we hold a reference
int g_menu_wait = 0;
int g_said = 0;

std::uint64_t slot(std::uint64_t bn) { return g_slide + (bn - kPreferredGuestSlide); }
void* code(std::uint64_t a) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(a)); }
template <typename T>
bool rd(std::uint64_t at, T* out) {
    return host_read_safe(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), out, sizeof(T));
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const std::vector<std::uint8_t>& b) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

// An archive entry's name (UTF-16 as stored) ends with `tail`.
bool name_ends_with(const std::vector<std::uint8_t>& name, const char* tail) {
    std::string s;
    for (std::size_t i = 0; i + 1 < name.size(); i += 2) s.push_back(name[i + 1] ? '?' : static_cast<char>(name[i]));
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

bool read_build(Build& b) {
    bool ok = player_stat_get("level", &b.level) && player_stat_get("echoes", &b.echoes);
    for (int i = 0; i < 6; ++i) ok = ok && player_stat_get(kStats[i], &b.stat[i]);
    return ok;
}

// The game's own apply, which the level-up's YES runs: the attributes and level
// into the record, then hit points, stamina and the character refreshed. It
// reads them from a functor's captures: +8 the level, +0xc.. the six.
void apply(const Build& b) {
    alignas(16) static std::uint32_t captures[10];
    std::memset(captures, 0, sizeof(captures));
    captures[2] = static_cast<std::uint32_t>(b.level);
    for (int i = 0; i < 6; ++i) captures[3 + i] = static_cast<std::uint32_t>(b.stat[i]);
    hle_call_guest<std::int64_t>(code(slot(kApplyStats)), reinterpret_cast<std::uint64_t>(captures));
}

// Lowering vitality, the apply takes the drop in maximum hit points off the
// current ones, which can leave the hunter at 0 - dead at the altar. Rebirth
// leaves them whole instead: the character's hit points and the record's.
void heal() {
    std::uint64_t man = 0, chr = 0, mods = 0, hp = 0;
    std::int32_t max = 0;
    if (!rd(slot(kWorldChrMan), &man) || !man || !rd(man + 0x60, &chr) || !chr || !rd(chr + 0x3b0, &mods) || !mods ||
        !rd(mods + 0x20, &hp) || !hp || !rd(hp + 0xfc, &max) || max <= 0)
        return;
    std::memcpy(code(hp + 0xf8), &max, sizeof max);
    player_stat_set("hp", max);
}

// The echoes for one level, as the level-up screen prices it (sub_1f2f5e0):
// CalcCorrectGraph row 200's init_inclination_soul, adjustment_value,
// boundry_inclination_soul and boundry_value at +0x3c..+0x48.
std::int64_t level_cost(std::int64_t level, const float g[4]) {
    const float x = static_cast<float>(level + 81);
    float k = x - g[3];
    if (k < 0.f) k = 0.f;
    k = k * g[2] + g[0];
    return static_cast<std::int64_t>(x * x * k + g[1] + 0.5f);
}

bool reset() {
    Build cur;
    int origin = -1;
    if (!read_build(cur) || !player_origin(&origin)) {
        host_log("rebirth: no character to read");
        return false;
    }
    std::size_t n = 0, gn = 0;
    const auto* row = static_cast<const std::uint8_t*>(params_row("CharaInitParam", 3000u + static_cast<std::uint32_t>(origin), &n));
    const auto* graph = static_cast<const std::uint8_t*>(params_row("CalcCorrectGraph", 200, &gn));
    if (!row || n < 0xc9 || !graph || gn < 0x4c) {
        host_log("rebirth: no origin row (origin %d) or level-cost row", origin);
        return false;
    }
    Build base;
    std::int16_t lv = 0;
    std::memcpy(&lv, row + 0xc0, 2);
    base.level = lv;
    const std::size_t at[6] = {0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc8};  // baseVit, baseWil, baseStr, baseDex, baseMag, baseFai
    for (int i = 0; i < 6; ++i) base.stat[i] = row[at[i]];
    float g[4];
    std::memcpy(g, graph + 0x3c, sizeof g);
    // Every attribute point above the origin's is a level bought: on a played
    // character that is the level above the origin's, and a character whose
    // level was edited away from its attributes comes back consistent.
    std::int64_t gained = 0;
    for (int i = 0; i < 6; ++i) gained += cur.stat[i] > base.stat[i] ? cur.stat[i] - base.stat[i] : 0;
    if (gained != cur.level - base.level)
        host_log("rebirth: level %lld but %lld attribute points above the origin's: refunding the points", static_cast<long long>(cur.level),
                 static_cast<long long>(gained));
    std::int64_t refund = 0;
    for (std::int64_t l = base.level; l < base.level + gained; ++l) refund += level_cost(l, g);
    if (cur.echoes + refund > kEchoCap) {
        host_log("rebirth: %lld echoes held and %lld to give back pass the limit", static_cast<long long>(cur.echoes),
                 static_cast<long long>(refund));
        return false;
    }
    g_snapshot = cur;
    g_have_snapshot = true;
    base.echoes = cur.echoes + refund;
    apply(base);
    heal();
    player_stat_set("echoes", base.echoes);
    host_log("rebirth: level %lld to %lld (origin %d), %lld blood echoes given back, %lld held", static_cast<long long>(cur.level),
             static_cast<long long>(base.level), origin, static_cast<long long>(refund), static_cast<long long>(base.echoes));
    return true;
}

void undo() {
    if (!g_have_snapshot) {
        host_log("rebirth: nothing to undo");
        return;
    }
    apply(g_snapshot);
    heal();
    player_stat_set("echoes", g_snapshot.echoes);
    g_have_snapshot = false;
    host_log("rebirth: undone - level %lld, %lld echoes", static_cast<long long>(g_snapshot.level), static_cast<long long>(g_snapshot.echoes));
}

void set_flag(std::uint32_t id, bool v) { event_flag_set(id, v); }
bool flag(std::uint32_t id) {
    bool v = false;
    return event_flag_get(id, &v) && v;
}

}  // namespace

void rebirth_tick() {
    if (!g_on) return;
    const std::uint64_t made = menu_steps_take(g_watch);  // every frame: a doll's level-up is not ours
    std::uint32_t block = 0;
    if (!world_player_block(&block)) {
        g_loading = true;
        return;
    }
    using namespace rebirth;
    if (g_loading) {  // a world came (back): a request a save carried over is not one
        g_loading = false;
        for (const std::uint32_t id : {kReqReset, kReqUndo, kFail, kMenu}) set_flag(id, false);
        g_have_snapshot = false;
        if (g_menu) menu_step_release(g_menu), g_menu = 0;
        return;
    }
    if (flag(kReqReset)) {
        if (block != kAltarBlock || !reset()) set_flag(kFail, true);
        set_flag(kReqReset, false);
    }
    if (flag(kReqUndo)) {
        undo();
        set_flag(kReqUndo, false);
    }
    if (flag(kMenu)) {
        if (!g_menu && made && *menu_step_count(made) >= 1) {
            menu_step_hold(made);  // ours: when it is the last, the menu has closed
            g_menu = made;
            g_menu_wait = 0;
        }
        if (g_menu && *menu_step_count(g_menu) == 1) {
            menu_step_release(g_menu);
            g_menu = 0;
            set_flag(kMenu, false);
        } else if (!g_menu && ++g_menu_wait > kMenuWaitFrames) {
            g_menu_wait = 0;
            set_flag(kMenu, false);
            if (g_said++ < 4) host_log("rebirth: the level-up menu did not open");
        }
    } else if (g_menu) {
        menu_step_release(g_menu);
        g_menu = 0;
    }
}

void rebirth_install(ElfImage* image) {
    if (!config().rebirth) {
        host_log("rebirth: off (PC enhancements: Rebirth at the Altar)");
        return;
    }
    if (!image || !eboot_is_109(image->sha256)) {
        host_log("rebirth: off - not the 1.09 eboot");
        return;
    }
    const char* app0 = hle_fs_app0_root();
    const char* data = hle_fs_data_root();
    if (!app0 || !*app0 || !data || !*data) {
        host_log("rebirth: off - no dump or data folder");
        return;
    }
    g_slide = image->mem.slide;
    g_watch = menu_steps_watch(7, u"LevelUp");
    if (g_watch < 0 || !menu_steps_install(image)) {
        host_log("rebirth: off - the level-up menu cannot be watched");
        return;
    }
    const fs::path out = fs::path(data) / "bbhost" / "rebirth-assets";
    const auto have = read_file(out / "stamp");
    const bool fresh = have && std::string(have->begin(), have->end()) == kStamp && fs::exists(out / kArchive);
    if (!fresh) {
        const auto src = read_file(fs::path(app0) / kArchive);
        if (!src) {
            host_log("rebirth: off - the dump has no %s", kArchive);
            return;
        }
        std::string why;
        const std::vector<std::uint8_t> raw = gcn::dcx_decompress(*src, &why);
        gcn::Bnd4Archive archive;
        if (raw.empty() || !gcn::bnd4_read(raw, archive, &why)) {
            host_log("rebirth: off - %s does not read (%s)", kArchive, why.c_str());
            return;
        }
        bool done = false;
        for (gcn::Bnd4File& f : archive.files) {
            if (!name_ends_with(f.name, kScript)) continue;
            if (!rebirth_script(f.data, &why)) {
                host_log("rebirth: off - %s", why.c_str());
                return;
            }
            done = true;
        }
        if (!done) {
            host_log("rebirth: off - %s has no %s", kArchive, kScript);
            return;
        }
        const std::vector<std::uint8_t> packed = gcn::dcx_compress(gcn::bnd4_pack(archive));
        const std::string stamp = kStamp;
        if (packed.empty() || !write_file(out / kArchive, packed) ||
            !write_file(out / "stamp", std::vector<std::uint8_t>(stamp.begin(), stamp.end()))) {
            host_log("rebirth: off - cannot write %s", out.string().c_str());
            return;
        }
        host_log("rebirth: made the Altar of Despair's talk script (%zu bytes) in %s", packed.size(), out.string().c_str());
    }
    hle_fs_add_generated_root(out.string().c_str());
    g_on = true;
    host_log("rebirth: the Altar of Despair offers Rebirth in the Nightmare (with the Yharnam Stone)");
}
