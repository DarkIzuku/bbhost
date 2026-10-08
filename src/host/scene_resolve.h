#pragma once
#include "host/scene_motion.h"
#include "host/upscaler.h"
namespace gpu {
struct RtImage;
// Resolve a verified scene before UI, using existing image heap, descriptor
// pools, command buffers and queue-fence retirement. No present interception.
bool scene_resolve_locked(UpscalerProvider&, const SceneCamera&, std::uint64_t frame,
    UpscaleExtent, RtImage& depth, RtImage& color, TemporalSample jitter);
void scene_resolve_shutdown_locked();
}
