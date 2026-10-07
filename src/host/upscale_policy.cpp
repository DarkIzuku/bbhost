#include "host/upscale_policy.h"
#include <algorithm>
#include <cmath>

namespace gpu {
namespace {
float halton(std::uint32_t index, std::uint32_t base) {
    float result = 0, scale = 1;
    while (index) { scale /= static_cast<float>(base); result += scale * static_cast<float>(index % base); index /= base; }
    return result;
}
}
TemporalSample temporal_jitter(std::uint64_t frame, std::uint32_t phases) {
    phases = std::clamp(phases, 1u, 1024u);
    const auto index = static_cast<std::uint32_t>(frame % phases) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}
TemporalSample motion_to_render_pixels(TemporalSample m, MotionUnits units, UpscaleExtent size) {
    if (!std::isfinite(m.x) || !std::isfinite(m.y) || !size.width || !size.height) return {};
    switch (units) {
    case MotionUnits::RenderPixels: return m;
    case MotionUnits::NormalizedUV: return {m.x * size.width, m.y * size.height};
    case MotionUnits::NdcYUp: return {m.x * size.width * 0.5f, -m.y * size.height * 0.5f};
    }
    return {};
}
HistoryReset UpscaleHistory::begin(const UpscaleConfig& c) {
    if (have_previous_) {
        if (!(previous_.render == c.render)) invalidate(HistoryReset::RenderSize);
        if (!(previous_.output == c.output)) invalidate(HistoryReset::OutputSize);
        if (previous_.provider != c.provider) invalidate(HistoryReset::Provider);
        if (previous_.preset != c.preset) invalidate(HistoryReset::Preset);
        if (previous_.resource_epoch != c.resource_epoch) invalidate(HistoryReset::ResourceEpoch);
        if (previous_.fullscreen != c.fullscreen) invalidate(HistoryReset::WindowMode);
    }
    previous_ = c; have_previous_ = true;
    return pending_;
}
void UpscaleHistory::commit() { pending_ = HistoryReset::None; ++samples_; }
bool temporal_inputs_valid(UpscaleStage stage, bool depth, bool motion, bool engine_jitter, float exposure) {
    return stage == UpscaleStage::SceneBeforeUI && depth && motion && engine_jitter && std::isfinite(exposure) && exposure > 0;
}
}  // namespace gpu
