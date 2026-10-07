#pragma once
#include "host/upscale_policy.h"
#include <vulkan/vulkan.h>

namespace gpu {
struct UpscaleImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    UpscaleExtent extent;
};
struct UpscaleFrame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    UpscaleImage color, depth, motion, exposure_image, output;
    VkRect2D input_area{}, output_area{};
    UpscaleStage stage = UpscaleStage::CompositePresentation;
    TemporalSample jitter;
    float exposure = 1, pre_exposure = 1, delta_seconds = 0;
    bool depth_inverted = false;
    bool engine_jitter_applied = false;
    HistoryReset reset = HistoryReset::None;
};
class UpscalerProvider {
public:
    virtual ~UpscalerProvider() = default;
    virtual UpscalerId id() const = 0;
    virtual bool temporal() const = 0;
    virtual bool supports(const UpscaleConfig&, const UpscaleFrame&) const = 0;
    // Guarded by Gpu::mu, after the caller's existing resource fence, outside
    // an active render pass. Images stay caller-owned; no queue submission,
    // device-idle wait or independent Vulkan resource allocator is introduced.
    // On false, the frame must remain valid for the caller's native blit.
    virtual bool record(const UpscaleConfig&, const UpscaleFrame&) = 0;
};
// One selection/dispatch point. Only registered implementations can run;
// future temporal IDs fall back safely and cannot consume a composited HUD.
bool upscale_record_locked(const UpscaleConfig&, UpscaleFrame frame);
UpscalerId upscale_selected();
void upscale_invalidate_locked(HistoryReset why);
// Engine scene integration owns the provider and its queue-fence retirement.
// Install/remove only under Gpu::mu; removal precedes provider destruction.
// Temporal dispatch remains prohibited at the composited presentation stage.
void upscale_set_temporal_provider_locked(UpscalerProvider* provider);
}  // namespace gpu
