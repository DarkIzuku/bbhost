// Debug Menu: an official bbhost plugin (docs/plugins.md) - the developers'
// debug menu, which the game still carries but leaves off.
//
//   [plugins]
//   debug_menu = true
//
// With it on, bbhost restores the menu at the next start (engine/debug_menu.h)
// and the Debug Menu key (`) opens and closes it. The menu draws with a debug
// font the game does not ship; this plugin makes it (debug_font.hpp) from the
// public-domain X11 fonts k14 and 7x14 and hands it to the game through its
// file overlay, so nothing has to be downloaded. A DbgFont14h.ccm and .tpf in
// the player's mods folder still win.
//
// It is a gameplay plugin: the menu can hand out items and change the rules,
// so its sessions announce a ruleset of their own and the server keeps their
// messages, bloodstains and statistics apart from other players'.
#include "bbhost/sdk.hpp"

#include "debug_font.hpp"

#include <string>

namespace {

// font14.bin (tools/make_debug_font.py), built in (CMakeLists.txt).
const std::uint8_t kFont14[] = {
#include "debug_font14.inc"
};

bb::Plugin g;

}  // namespace

extern "C" BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION,
    BB_PLUGIN_OPT_IN | BB_PLUGIN_GAMEPLAY,
    "debug_menu",
    "Debug Menu",
    "1.0.0",
    "bbhost",
    "The developers' debug menu, still in the game: ` opens and closes it (from the next start). Cheating against "
    "other players with it gets an account banned; trying it out online with friends is fine.",
};

extern "C" BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_HEADING, nullptr, "Playing online",
     "Using the debug menu to cheat against other players - in their worlds, in invasions or on the leaderboards - "
     "gets your account banned. Trying it out online with friends is fine."},
    {BB_OPT_HEADING, nullptr, "Using it",
     "` (the key left of 1) opens and closes it, and Key Bindings can move it. The arrows move, Enter opens an entry and "
     "Backspace goes back. Its text is the developers' Japanese. Sessions with it on keep their messages, bloodstains "
     "and statistics apart from other players'."},
    {BB_OPT_END}};

extern "C" BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* api) {
    if (!g.attach(api, 9)) return 1;
    if (!api->eboot_is_109()) return 1;
    debug_font::Files font;
    std::string err;
    if (!debug_font::build(kFont14, sizeof(kFont14), &font, &err)) {
        g.log("debug_menu: %s", err.c_str());
        return 1;
    }
    if (api->overlay_file("debug_menu", "/dvdroot_ps4/adhoc/font/DbgFont14h.ccm", font.ccm.data(), font.ccm.size()) != 0 ||
        api->overlay_file("debug_menu", "/dvdroot_ps4/adhoc/font/DbgFont14h.tpf", font.tpf.data(), font.tpf.size()) != 0) {
        g.log("debug_menu: the debug font could not be written to the overlay");
        return 1;
    }
    g.log("debug_menu: the debug font made (%d glyphs on %d pages); bbhost restores the menu", font.glyphs, font.pages);
    return 0;
}
