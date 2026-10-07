#include "host/upscale_policy.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>
using namespace gpu;
namespace {
int fails = 0;
void check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); ++fails; } }
bool has(HistoryReset reasons, HistoryReset reason) { return (static_cast<unsigned>(reasons) & static_cast<unsigned>(reason)) != 0; }
bool near(float a, float b) { return std::abs(a - b) < 0.0001f; }
}
int main() {
    UpscaleHistory history;
    UpscaleConfig config; config.render = {1280, 720}; config.output = {1920, 1080}; config.provider = UpscalerId::Fsr31;
    check(has(history.begin(config), HistoryReset::FirstFrame), "first dispatch resets");
    check(has(history.begin(config), HistoryReset::FirstFrame), "failed dispatch does not consume reset");
    history.commit(); check(history.begin(config) == HistoryReset::None && history.sample_index() == 1, "stable frame retains history");
    config.render = {1920, 1080}; check(has(history.begin(config), HistoryReset::RenderSize), "live resolution resets");
    history.commit(); config.output = {3840, 2160}; check(has(history.begin(config), HistoryReset::OutputSize), "output resize resets");
    history.commit(); config.provider = UpscalerId::Dlss; config.preset = UpscalePreset::NativeAA;
    const auto both = history.begin(config); check(has(both, HistoryReset::Provider) && has(both, HistoryReset::Preset), "DLAA/provider switch resets");
    history.commit(); config.fullscreen = true; ++config.resource_epoch;
    const auto resize = history.begin(config); check(has(resize, HistoryReset::WindowMode) && has(resize, HistoryReset::ResourceEpoch), "swapchain/resource replacement resets");
    history.commit(); config.sharpness = 0.8f; check(history.begin(config) == HistoryReset::None, "sharpening does not discard temporal history");
    for (auto event : {HistoryReset::Load, HistoryReset::Teleport, HistoryReset::CameraCut, HistoryReset::Area, HistoryReset::NewGame, HistoryReset::BackendFailure}) {
        history.invalidate(event); check(has(history.begin(config), event) && history.sample_index() == 0, "engine reset survives until successful dispatch"); history.commit();
    }
    for (unsigned i = 0; i < 64; ++i) {
        const auto j = temporal_jitter(i, 16), repeat = temporal_jitter(i + 16, 16);
        check(j.x >= -0.5f && j.x <= 0.5f && j.y >= -0.5f && j.y <= 0.5f && near(j.x, repeat.x) && near(j.y, repeat.y), "bounded reproducible jitter");
    }
    const auto first = temporal_jitter(0, 0); check(near(first.x, 0) && near(first.y, -1.0f / 6), "zero phase count is safe");
    const auto uv = motion_to_render_pixels({0.01f, -0.02f}, MotionUnits::NormalizedUV, {1920, 1080});
    check(near(uv.x, 19.2f) && near(uv.y, -21.6f), "motion normalization uses render size, not output size");
    const auto ndc = motion_to_render_pixels({0.02f, 0.04f}, MotionUnits::NdcYUp, {1920, 1080});
    check(near(ndc.x, uv.x) && near(ndc.y, uv.y), "NDC top-left conversion");
    const auto invalid = motion_to_render_pixels({std::numeric_limits<float>::quiet_NaN(), 1}, MotionUnits::RenderPixels, {1920, 1080});
    check(invalid.x == 0 && invalid.y == 0, "invalid motion cannot poison history");
    check(!temporal_inputs_valid(UpscaleStage::CompositePresentation, true, true, true, 1), "HUD/Scaleform composite cannot enter temporal provider");
    check(!temporal_inputs_valid(UpscaleStage::SceneBeforeUI, true, false, true, 1) && !temporal_inputs_valid(UpscaleStage::SceneBeforeUI, true, true, false, 1), "missing object motion or engine jitter rejected");
    check(!temporal_inputs_valid(UpscaleStage::SceneBeforeUI, true, true, true, 0), "invalid exposure rejected");
    check(temporal_inputs_valid(UpscaleStage::SceneBeforeUI, true, true, true, 1), "scene input with verified temporal signals accepted");
    std::printf("upscale policy: %s\n", fails ? "FAILED" : "passed"); return fails ? 1 : 0;
}
