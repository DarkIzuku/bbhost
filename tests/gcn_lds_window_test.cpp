// gcn/translate.cpp: in the game's own hull draws every stage keeps the LDS
// in a buffer, a window a patch (TranslateOptions::tess_window). An access
// past the window reached memory the GPU had not mapped and lost the device
// (an RX 9070 XT, 2026-10-08), so each LDS address is clamped into the
// window: the LS pass, the hull shader and the domain shader each translate
// to valid SPIR-V with that clamp - a UMin against the window's last dword -
// and a stage without a window has none.
#include "gcn/isa.h"
#include "gcn/translate.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

constexpr std::uint32_t kWindow = 848;  // the Forbidden Woods' hull: 3 control points, 112/96-byte strides, 14 constants

// ds_<op> in the DS encoding: op, offsets; then vdst, data1, data0, addr.
void ds(std::vector<std::uint32_t>& w, std::uint32_t op, std::uint32_t offset, std::uint32_t dst, std::uint32_t data0,
        std::uint32_t addr) {
    w.push_back(0xd8000000u | (op << 18) | (offset & 0xffff));
    w.push_back((dst << 24) | (data0 << 8) | addr);
}

// The word a GLSL.std.450 UMin with the constant `bound` as one operand
// names, if the module has one.
bool has_umin_with(const std::vector<std::uint32_t>& spv, std::uint32_t bound) {
    std::uint32_t glsl = 0;
    std::vector<std::uint32_t> consts;  // ids of 32-bit constants equal to `bound`
    for (std::size_t i = 5; i < spv.size();) {
        const std::uint32_t op = spv[i] & 0xffff, len = spv[i] >> 16;
        if (!len || i + len > spv.size()) break;
        if (op == 11 && len >= 3) glsl = spv[i + 1];                                   // OpExtInstImport
        if (op == 43 && len == 4 && spv[i + 3] == bound) consts.push_back(spv[i + 2]); // OpConstant
        if (op == 12 && len >= 7 && spv[i + 3] == glsl && spv[i + 4] == 38) {          // OpExtInst UMin
            for (const std::uint32_t c : consts) {
                if (spv[i + 5] == c || spv[i + 6] == c) return true;
            }
        }
        i += len;
    }
    return false;
}

}  // namespace

int main() {
    // ds_read_b32 v1, v0; ds_write_b32 v0, v1 offset:4; s_endpgm
    std::vector<std::uint32_t> words;
    ds(words, 54, 0, 1, 0, 0);
    ds(words, 13, 4, 0, 1, 0);
    words.push_back(0xbf810000u);
    const gcn::Program prog = gcn::decode(words.data(), words.size());
    CHECK(prog.errors.empty() && prog.insts.size() == 3);

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string msg;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) {
        if (msg.empty()) msg = m;
    });
    struct Role {
        const char* name;
        gcn::Stage stage;
        gcn::TranslateOptions::TessRole role;
        std::uint32_t window;
    };
    const Role roles[] = {
        {"LS pass", gcn::Stage::Compute, gcn::TranslateOptions::TessRole::LsCompute, kWindow},
        {"hull shader", gcn::Stage::TessControl, gcn::TranslateOptions::TessRole::HullTcs, kWindow},
        {"domain shader", gcn::Stage::TessEval, gcn::TranslateOptions::TessRole::DomainTes, kWindow},
        {"LS pass without a window", gcn::Stage::Compute, gcn::TranslateOptions::TessRole::LsCompute, 0},
    };
    for (const Role& r : roles) {
        gcn::TranslateOptions o;
        o.stage = r.stage;
        o.tess_role = r.role;
        o.tess_window = r.window;
        o.tess_patch_control_points = 3;
        o.tess_quads = false;
        o.cs_threads[0] = 64;
        if (r.role == gcn::TranslateOptions::TessRole::HullTcs) o.descriptor_set = 0;
        const gcn::TranslateResult t = gcn::translate(prog, o);
        msg.clear();
        const bool valid = t.ok() && tools.Validate(t.spirv);
        const bool clamped = has_umin_with(t.spirv, kWindow - 4);
        std::printf("gcn_lds_window_test: %s: %s, %zu words%s%s\n", r.name, t.ok() ? "translated" : t.errors[0].c_str(), t.spirv.size(),
                    valid ? ", valid" : (", invalid: " + msg).c_str(), clamped ? ", LDS clamped to the window" : "");
        CHECK(t.ok());
        CHECK(valid);
        CHECK(clamped == (r.window != 0));
    }
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("gcn_lds_window_test: ok\n");
    return 0;
}
