#include "host/upscaler.h"
#include "host/gpu_internal.h"
#include "host/settings.h"
#include "log.h"
#include <cstdlib>
#include <cstring>

namespace gpu {
namespace {
class NativeProvider final : public UpscalerProvider {
public:
    UpscalerId id() const override { return UpscalerId::Native; }
    bool temporal() const override { return false; }
    bool supports(const UpscaleConfig&, const UpscaleFrame&) const override { return true; }
    bool record(const UpscaleConfig&, const UpscaleFrame& f) override {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {f.input_area.offset.x, f.input_area.offset.y, 0};
        blit.srcOffsets[1] = {f.input_area.offset.x + static_cast<std::int32_t>(f.input_area.extent.width), f.input_area.offset.y + static_cast<std::int32_t>(f.input_area.extent.height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = {f.output_area.offset.x, f.output_area.offset.y, 0};
        blit.dstOffsets[1] = {f.output_area.offset.x + static_cast<std::int32_t>(f.output_area.extent.width), f.output_area.offset.y + static_cast<std::int32_t>(f.output_area.extent.height), 1};
        vkCmdBlitImage(f.commands, f.color.image, f.color.layout, f.output.image, f.output.layout, 1, &blit, VK_FILTER_LINEAR);
        return true;
    }
};
class Fsr1Provider final : public UpscalerProvider {
public:
    UpscalerId id() const override { return UpscalerId::Fsr1; }
    bool temporal() const override { return false; }
    bool supports(const UpscaleConfig& c, const UpscaleFrame& f) const override {
        return c.render.width <= c.output.width && c.render.height <= c.output.height &&
               (c.render.width < c.output.width || c.render.height < c.output.height) &&
               f.color.layout == VK_IMAGE_LAYOUT_GENERAL && f.output.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    bool record(const UpscaleConfig& c, const UpscaleFrame& f) override {
        if (f.color.layout != VK_IMAGE_LAYOUT_GENERAL || f.output.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return false;
        return fsr_upscale_locked(f.commands, f.color.image, f.color.format, f.color.extent.width, f.color.extent.height,
                                  f.input_area.offset.x, f.input_area.offset.y, f.input_area.extent.width, f.input_area.extent.height,
                                  f.output.image, f.output_area, f.output.view, c.sharpness, c.sharpening);
    }
};
NativeProvider native;
Fsr1Provider fsr1;
UpscaleHistory history;
UpscalerProvider* temporal_provider = nullptr;
bool warned_missing = false;
UpscalerProvider* provider(UpscalerId id) {
    if (temporal_provider && temporal_provider->id() == id) return temporal_provider;
    for (UpscalerProvider* p : {static_cast<UpscalerProvider*>(&native), static_cast<UpscalerProvider*>(&fsr1)}) if (p->id() == id) return p;
    return nullptr;
}
}
UpscalerId upscale_selected() {
    if (const char* e = std::getenv("BBHOST_UPSCALE"); e && *e) return std::strcmp(e, "linear") == 0 ? UpscalerId::Native : UpscalerId::Fsr1;
    return host_settings().spatial_upscale ? UpscalerId::Fsr1 : UpscalerId::Native;
}
bool upscale_record_locked(const UpscaleConfig& config, UpscaleFrame frame) {
    bool fallback = false;
    auto* p = provider(config.provider);
    if (!p) {
        if (!warned_missing) { host_log("upscaler: requested backend is not integrated; using native presentation"); warned_missing = true; }
        history.invalidate(HistoryReset::BackendFailure); p = &native; fallback = true;
    }
    if (!frame.commands || !frame.color.image || !frame.output.image || !config.render.width || !config.render.height || !config.output.width || !config.output.height) return false;
    if (frame.input_area.offset.x < 0 || frame.input_area.offset.y < 0) return false;
    if (frame.reset != HistoryReset::None) history.invalidate(frame.reset);
    frame.reset = history.begin(config);
    if (p->temporal() && !temporal_inputs_valid(frame.stage, frame.depth.image != VK_NULL_HANDLE, frame.motion.image != VK_NULL_HANDLE, frame.engine_jitter_applied, frame.exposure)) {
        history.invalidate(HistoryReset::BackendFailure); p = &native; fallback = true;
    }
    if (!p->supports(config, frame)) {
        // A presentable fallback must not consume the failed temporal frame's
        // reset. The next actual DLSS/FSR dispatch still needs that reset.
        if (p->temporal()) history.invalidate(HistoryReset::BackendFailure);
        p = &native; fallback = true;
    }
    if (!p->record(config, frame)) {
        history.invalidate(HistoryReset::BackendFailure);
        return native.record(config, frame);
    }
    if (!fallback) history.commit();
    return true;
}
void upscale_invalidate_locked(HistoryReset why) { history.invalidate(why); }
void upscale_set_temporal_provider_locked(UpscalerProvider* p) {
    temporal_provider = p && p->temporal() ? p : nullptr;
    history.invalidate(HistoryReset::Provider);
}
}  // namespace gpu
