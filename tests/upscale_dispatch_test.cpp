// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the real common dispatcher without submitting work to a GPU.
#include "host/upscaler.h"
#include "host/settings.h"
#include <cstdio>
#include <vector>
using namespace gpu;
namespace { unsigned blits = 0; }
HostSettings host_settings() { return {}; }
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, std::uint32_t, const VkImageBlit*, VkFilter) { ++blits; }
namespace gpu {
bool fsr_upscale_locked(VkCommandBuffer, VkImage, VkFormat, std::uint32_t, std::uint32_t,
                        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, VkImage, VkRect2D, VkImageView, float, bool) { return false; }
}
class SceneProvider final : public UpscalerProvider {
public:
    bool supported = false, succeeds = true;
    std::vector<HistoryReset> resets;
    UpscalerId id() const override { return UpscalerId::Dlss; }
    bool temporal() const override { return true; }
    bool supports(const UpscaleConfig&, const UpscaleFrame&) const override { return supported; }
    bool record(const UpscaleConfig&, const UpscaleFrame& f) override { resets.push_back(f.reset); return succeeds; }
};
bool has(HistoryReset value, HistoryReset flag) { return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0; }
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } };
    SceneProvider provider;
    upscale_set_temporal_provider_locked(&provider);
    UpscaleConfig c; c.provider = UpscalerId::Dlss; c.render = {1280, 720}; c.output = {1920, 1080};
    UpscaleFrame f; f.stage = UpscaleStage::SceneBeforeUI; f.commands = reinterpret_cast<VkCommandBuffer>(1);
    f.color.image = reinterpret_cast<VkImage>(2); f.depth.image = reinterpret_cast<VkImage>(3); f.motion.image = reinterpret_cast<VkImage>(4); f.output.image = reinterpret_cast<VkImage>(5);
    f.color.layout = f.output.layout = VK_IMAGE_LAYOUT_GENERAL; f.engine_jitter_applied = true;
    f.input_area.extent = {1280, 720}; f.output_area.extent = {1920, 1080};
    upscale_invalidate_locked(HistoryReset::Load);
    check(upscale_record_locked(c, f) && blits == 1 && provider.resets.empty(), "unsupported provider safely blits native");
    provider.supported = true;
    check(upscale_record_locked(c, f) && provider.resets.size() == 1 && has(provider.resets.back(), HistoryReset::Load) && has(provider.resets.back(), HistoryReset::BackendFailure), "fallback preserves load reset until actual temporal dispatch");
    check(upscale_record_locked(c, f) && provider.resets.back() == HistoryReset::None, "successful temporal dispatch consumes reset");
    provider.succeeds = false;
    check(upscale_record_locked(c, f) && blits == 2, "failed temporal dispatch falls back");
    provider.succeeds = true;
    check(upscale_record_locked(c, f) && has(provider.resets.back(), HistoryReset::BackendFailure), "failed dispatch resets next actual temporal frame");
    const auto count = provider.resets.size(); f.stage = UpscaleStage::CompositePresentation;
    check(upscale_record_locked(c, f) && blits == 3 && provider.resets.size() == count, "HUD composite never reaches temporal provider");
    f.stage = UpscaleStage::SceneBeforeUI; check(upscale_record_locked(c, f) && has(provider.resets.back(), HistoryReset::BackendFailure), "HUD fallback preserves reset");
    upscale_set_temporal_provider_locked(nullptr);
    std::printf("upscale dispatch: %s\n", failures ? "FAILED" : "passed"); return failures ? 1 : 0;
}
