// Renderer-independent temporal state. This never reads PM4/register state.
#pragma once
#include <cstdint>

namespace gpu {
enum class UpscalerId { Native, Fsr1, Taa, Fsr31, Fsr4, Fsr411, Dlss };
enum class UpscalePreset { NativeAA, Quality, Balanced, Performance, UltraPerformance, Custom };
enum class UpscaleStage { CompositePresentation, SceneBeforeUI };
struct UpscaleExtent {
    std::uint32_t width = 0, height = 0;
    bool operator==(const UpscaleExtent&) const = default;
};
struct UpscaleConfig {
    UpscalerId provider = UpscalerId::Native;
    UpscalePreset preset = UpscalePreset::Custom;
    UpscaleExtent render, output;
    std::uint64_t resource_epoch = 0;
    bool fullscreen = false;
    float sharpness = 0.2f;
    bool sharpening = true;
};
enum class HistoryReset : std::uint32_t {
    None = 0, FirstFrame = 1, RenderSize = 2, OutputSize = 4, Provider = 8,
    Preset = 16, ResourceEpoch = 32, WindowMode = 64, Load = 128,
    Teleport = 256, CameraCut = 512, Area = 1024, NewGame = 2048,
    BackendFailure = 4096
};
constexpr HistoryReset operator|(HistoryReset a, HistoryReset b) {
    return static_cast<HistoryReset>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
struct TemporalSample { float x = 0, y = 0; };
// Canonical jitter: render pixels, zero-centred; projection conversion belongs
// at the verified engine camera hook, never at presentation.
TemporalSample temporal_jitter(std::uint64_t frame, std::uint32_t phases);
enum class MotionUnits { RenderPixels, NormalizedUV, NdcYUp };
// Canonical motion is unjittered current-to-previous, top-left origin, render
// pixels. Backends may require an additional convention-specific conversion.
TemporalSample motion_to_render_pixels(TemporalSample motion, MotionUnits units, UpscaleExtent render);

class UpscaleHistory {
    UpscaleConfig previous_{};
    bool have_previous_ = false;
    std::uint64_t samples_ = 0;
    HistoryReset pending_ = HistoryReset::FirstFrame;
public:
    HistoryReset begin(const UpscaleConfig& config);
    void invalidate(HistoryReset why) { pending_ = pending_ | why; samples_ = 0; }
    // Only after a successful dispatch. A failed/fallback frame must not
    // consume a reset that the next temporal backend still needs.
    void commit();
    std::uint64_t sample_index() const { return samples_; }
};
bool temporal_inputs_valid(UpscaleStage stage, bool depth, bool motion, bool engine_jitter, float exposure);
}  // namespace gpu
