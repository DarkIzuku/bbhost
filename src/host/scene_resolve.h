#pragma once
#include "host/scene_motion.h"
#include "host/upscaler.h"
namespace gpu {
struct RtImage;
// Resolve a verified scene before UI, using existing image heap, descriptor
// pools, command buffers and queue-fence retirement. No present interception.
bool scene_resolve_locked(UpscalerProvider&, const SceneCamera&, std::uint64_t frame,
    const UpscaleConfig&, RtImage& depth, RtImage& color, UpscaleExtent color_picture, TemporalSample jitter);
// Upscaled scene sampled at the verified UI boundary. Never substitutes a
// storage/depth binding or an image from another ordered frame.
VkImageView scene_resolve_view_locked(std::uint64_t source, std::uint64_t frame);
void scene_resolve_shutdown_locked();
}
