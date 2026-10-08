#include "host/shader_patch.h"
#include "host/settings.h"
#include <cstdio>

namespace { HostSettings settings; std::uint64_t serial=1; }
HostSettings host_settings() { return settings; }
std::uint64_t host_opt_serial() { return serial; }
int main() {
    int failures=0;
    const auto check=[&](bool ok,const char* why) {if(!ok){++failures;std::printf("FAIL %s\n",why);}};
    // The conditional blur branch and its centre-sample else path have the
    // same entry in both verified programs. Retain all following instructions.
    const std::vector<std::uint32_t> original={0x7c061420,0xbea0246a,0xbf880001,0x7e080305};
    for(const char* name:{"e0305cef","29e06868"}) {
        check(shader_patch_touches(name),"both world blur variants follow the setting");
        auto words=original;
        shader_patch_apply(name,words);
        check(words==original,"On preserves the native motion-blur program");
        settings.motion_blur=false;++serial;
        shader_patch_apply(name,words);
        check(words[0]==0x7c001420 && words[1]==original[1] && words[2]==original[2] && words[3]==original[3],
              "Off bypasses blur while retaining the centre-sample path");
        auto ambiguous=original;ambiguous.insert(ambiguous.end(),original.begin(),original.end());
        const auto before=ambiguous;shader_patch_apply(name,ambiguous);
        check(ambiguous==before,"a changed/ambiguous program is never patched");
        settings.motion_blur=true;++serial;
    }
    settings.motion_blur=false;++serial;auto unknown=original;
    shader_patch_apply("unknown",unknown);
    check(unknown==original,"an unrelated shader with matching words remains untouched");
    std::printf("shader patch: %s\n",failures?"FAILED":"passed");return failures?1:0;
}
