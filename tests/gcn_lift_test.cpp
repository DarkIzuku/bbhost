// The pilot pixel shader (a22c7f71) lifts from the shipped bundle, and the lifter
// refuses the constructs it cannot prove equal. Skips when the dump is absent.
#include "test_app0.h"
#include "gcn/container.h"
#include "gcn/isa.h"
#include "gcn/lift.h"
#include "gcn/translate.h"

#include <spirv-tools/libspirv.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

namespace {

gcn::LiftResult lift(const gcn::Program& p, gcn::TranslateOptions o) {
    // The renderer translates with dimensions from the resolved T#s; the pilot's
    // last two images are cube shadow maps. Find their count with a first pass.
    const gcn::TranslateResult paths = gcn::translate(p, o);
    o.image_dims.assign(paths.images.size(), {1 /* 2D */, false});
    if (o.image_dims.size() >= 2) {
        o.image_dims[o.image_dims.size() - 1].first = 3;
        o.image_dims[o.image_dims.size() - 2].first = 3;
    }
    const gcn::TranslateResult ref = gcn::translate(p, o);
    return gcn::lift_pixel_shader(p, o, ref);
}

bool rejected_with(const gcn::LiftResult& r, const char* text) {
    for (const std::string& why : r.rejections) {
        if (why.find(text) != std::string::npos) return true;
    }
    std::fprintf(stderr, "expected a rejection containing \"%s\"; got %zu rejection(s)%s%s\n", text, r.rejections.size(),
                 r.rejections.empty() ? "" : ": ", r.rejections.empty() ? "" : r.rejections[0].c_str());
    return false;
}

// A copy of the program with the instruction at `offset` (which must be `expect`) changed.
gcn::Program mutate(const gcn::Program& p, std::uint32_t offset, const char* expect, const std::function<void(gcn::Inst&)>& change) {
    gcn::Program q = p;
    for (gcn::Inst& in : q.insts) {
        if (in.offset != offset) continue;
        const char* name = gcn::mnemonic(in);
        CHECK(name && std::strcmp(name, expect) == 0);
        change(in);
        return q;
    }
    CHECK(!"instruction offset not found");
    return q;
}

void make_nop(gcn::Inst& in) {
    in.enc = gcn::Enc::SOPP;
    in.op = 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : test_app0_file("dvdroot_ps4/shader/gxrenderershader.shaderbnd.dcx");
    std::vector<std::uint8_t> raw;
    if (!gcn::read_file(path, raw)) {
        std::printf("gcn_lift_test skipped: %s not readable\n", path.c_str());
        return kTestSkip;
    }
    std::string err;
    const std::vector<std::uint8_t> b = gcn::dcx_decompress(raw, &err);
    std::vector<gcn::BundleEntry> entries;
    if (b.empty() || !gcn::bnd4_entries(b, entries, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const gcn::BundleEntry* pilot = nullptr;
    for (const gcn::BundleEntry& e : entries) {
        if (e.name.find("GXLightAcc_LegacyDPointLightShadowed2.ppo") != std::string::npos) pilot = &e;
    }
    gcn::ShaderCode code;
    if (!pilot || !gcn::shader_code(pilot->data, code)) {
        std::fprintf(stderr, "pilot shader not found in %s\n", path.c_str());
        return 1;
    }
    std::size_t shdr = 0;
    for (std::size_t i = 0; i + 4 <= pilot->data.size(); ++i) {
        if (std::memcmp(pilot->data.data() + i, "Shdr", 4) == 0) {
            shdr = i;
            break;
        }
    }
    std::uint32_t regs[16];
    std::memcpy(regs, pilot->data.data() + shdr + 16, 64);
    const gcn::Program p = gcn::decode(code.words.data(), code.words.size());
    gcn::TranslateOptions o;
    o.stage = gcn::Stage::Pixel;
    o.rsrc1 = regs[4];
    o.rsrc2 = regs[5];
    o.ps_input_ena = regs[8];
    o.descriptor_set = 1;
    o.cb_ssbo = true;
    o.cb_no_fallback = true;
    o.early_fragment_tests = true;

    const gcn::LiftResult ok = lift(p, o);
    for (const std::string& why : ok.rejections) std::fprintf(stderr, "pilot rejected: %s\n", why.c_str());
    CHECK(ok.ok());
    CHECK(ok.proof.size() >= 9);  // five facts plus one per region
    if (ok.ok()) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
        std::string msg;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* m) {
            if (msg.empty()) msg = m;
        });
        spvtools::ValidatorOptions vo;
        vo.SetAllowOffsetTextureOperand(true);
        const bool valid = tools.Validate(ok.spirv.data(), ok.spirv.size(), vo);
        if (!valid) std::fprintf(stderr, "lifted SPIR-V invalid: %s\n", msg.c_str());
        CHECK(valid);
    }

    // Image dimensions predicted from the program alone (per-shader compiles,
    // step 2): the pilot's last two images are its cube shadow maps, whose face
    // coordinate is a v_cubeid_f32 result; the others are 2D.
    {
        const gcn::TranslateResult paths = gcn::translate(p, o);
        const std::vector<gcn::PredictedImage> predicted = gcn::predict_image_dims(p, paths);
        CHECK(predicted.size() == paths.images.size() && predicted.size() >= 2);
        for (std::size_t k = 0; k < predicted.size(); ++k) {
            const bool cube = k + 2 >= predicted.size();
            CHECK(predicted[k].dim == (cube ? 3u : 1u) && !predicted[k].arrayed && !predicted[k].conflict);
        }
    }

    // The storage-buffer fallback reads the page table, which the lift does not model.
    gcn::TranslateOptions fallback = o;
    fallback.cb_no_fallback = false;
    CHECK(rejected_with(lift(p, fallback), "no-fallback"));

    // Without the restore after the first masked region, the sample at 0x4cc runs
    // where EXEC is only the else-mask.
    CHECK(rejected_with(lift(mutate(p, 0x45c, "s_mov_b64", make_nop), o), "EXEC is not known set"));

    // Without the load at 0x460, s4 written in the second region (0x3ec-0x45c)
    // is read at 0x46c: GCN runs that block for every lane when any lane needs it.
    CHECK(rejected_with(lift(mutate(p, 0x460, "s_buffer_load_dwordx4", make_nop), o), "s4 is written inside region"));

    // An s_branch that closes no if's then arm (here a jump over a region).
    CHECK(rejected_with(lift(mutate(p, 0x3ac, "s_cbranch_execz", [](gcn::Inst& in) { in.op = 2; }), o), "does not close an if"));

    // A VCC branch becomes an if only where every lane holds the same VCC bit;
    // the pilot's VCC comes from per-pixel values.
    CHECK(rejected_with(lift(mutate(p, 0x3ac, "s_cbranch_execz", [](gcn::Inst& in) { in.op = 6; }), o), "VCC that may differ between lanes"));

    // A sample inside a masked region, where EXEC is the region's mask (the
    // instruction at 0x3b0 becomes the sample from 0x068).
    gcn::Inst sample;
    for (const gcn::Inst& in : p.insts) {
        if (in.offset == 0x68) sample = in;
    }
    CHECK(rejected_with(lift(mutate(p, 0x3b0, "v_mul_f32",
                                    [&](gcn::Inst& in) {
                                        const std::uint32_t at = in.offset;
                                        const std::uint8_t size = in.size;
                                        in = sample;
                                        in.offset = at;
                                        in.size = size;
                                    }),
                             o),
                        "image sample where EXEC is not known set"));

    if (g_failures) {
        std::fprintf(stderr, "gcn_lift_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gcn_lift_test ok: pilot lifted (%zu words), six constructs rejected\n", ok.spirv.size());
    return 0;
}
