// SPDX-License-Identifier: GPL-2.0-or-later
#include "host/dlss_ngx.h"
#include <cstdio>
#include <limits>
using namespace gpu;
int main() {
    int failures = 0;
    auto check = [&](bool v, const char* why) { if (!v) { std::printf("FAIL %s\n", why); ++failures; } };
    UpscaleConfig c; c.provider = UpscalerId::Dlss; c.preset = UpscalePreset::Quality; c.render = {1280, 720}; c.output = {1920, 1080};
    UpscaleFrame f; f.stage = UpscaleStage::SceneBeforeUI; f.commands = reinterpret_cast<VkCommandBuffer>(1); f.delta_seconds = 1.0f / 60; f.engine_jitter_applied = true;
    auto image = [](std::uintptr_t value, VkFormat format, UpscaleExtent extent) { return UpscaleImage{reinterpret_cast<VkImage>(value), reinterpret_cast<VkImageView>(value), format, VK_IMAGE_LAYOUT_GENERAL, extent}; };
    f.color = image(2, VK_FORMAT_R16G16B16A16_SFLOAT, c.render); f.depth = image(3, VK_FORMAT_D32_SFLOAT, c.render);
    f.motion = image(4, VK_FORMAT_R16G16_SFLOAT, c.render); f.output = image(5, VK_FORMAT_R16G16B16A16_SFLOAT, c.output);
    f.input_area.extent = {c.render.width, c.render.height}; f.output_area.extent = {c.output.width, c.output.height};
    check(dlss_frame_contract(c, f), "valid scene contract");
    auto bad = f; bad.stage = UpscaleStage::CompositePresentation; check(!dlss_frame_contract(c, bad), "reject HUD/Scaleform composite");
    bad = f; bad.engine_jitter_applied = false; check(!dlss_frame_contract(c, bad), "reject unverified engine jitter");
    bad = f; bad.motion.image = VK_NULL_HANDLE; check(!dlss_frame_contract(c, bad), "reject absent engine motion");
    bad = f; bad.output.image = f.color.image; check(!dlss_frame_contract(c, bad), "reject aliasing scene output");
    bad = f; bad.output.layout = VK_IMAGE_LAYOUT_UNDEFINED; check(!dlss_frame_contract(c, bad), "reject undefined output layout");
    bad = f; bad.color.view = VK_NULL_HANDLE; check(!dlss_frame_contract(c, bad), "reject absent image view");
    bad = f; bad.input_area.offset.x = 10; check(!dlss_frame_contract(c, bad), "reject unhandled input offsets");
    bad = f; bad.depth.format = VK_FORMAT_R32_SFLOAT; check(!dlss_frame_contract(c, bad), "reject color-as-depth aspect mismatch");
    bad = f; bad.motion.extent = {1, 1}; check(!dlss_frame_contract(c, bad), "reject short motion image");
    bad = f; bad.delta_seconds = std::numeric_limits<float>::quiet_NaN(); check(!dlss_frame_contract(c, bad), "reject invalid frame interval");
    bad = f; bad.pre_exposure = 0; check(!dlss_frame_contract(c, bad), "reject invalid pre-exposure");
    bad = f; bad.jitter.x = 100; check(!dlss_frame_contract(c, bad), "reject invalid jitter");
    auto native = c; native.preset = UpscalePreset::NativeAA; check(!dlss_frame_contract(native, f), "DLAA requires native rendering");
    native.render = native.output; bad = f; bad.color.extent = bad.depth.extent = bad.motion.extent = native.render; bad.input_area = bad.output_area;
    check(dlss_frame_contract(native, bad), "DLAA native frame");
    std::printf("DLSS frame contract: %s\n", failures ? "FAILED" : "passed"); return failures ? 1 : 0;
}
