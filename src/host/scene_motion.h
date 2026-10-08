// Temporal math over the native engine's scene constants. No GPU registers,
// guessed follow-camera projection or emulation state participates here.
#pragma once
#include "host/upscale_policy.h"
#include <array>
#include <cstddef>

namespace gpu {
using SceneMatrix = std::array<float, 16>; // row-major, column-vector multiply
struct SceneCamera {
    SceneMatrix clip_from_relative_world{};
    std::array<float, 3> world_origin{};
};
// Verified 1.09 GX Scene constant layout. Decline truncated, non-finite,
// non-camera or singular data; the caller must identify the primary scene.
bool scene_camera_decode(const void* constants, std::size_t bytes, SceneCamera& out);
bool scene_camera_valid(const SceneCamera&);
bool scene_reprojection(const SceneCamera& current, const SceneCamera& previous, SceneMatrix& previous_clip_from_current_clip);
// UV/depth belong to the current unjittered frame. Motion is previous minus
// current in render pixels with downward-positive Y (DLSS canonical input).
bool scene_camera_motion(const SceneMatrix&, float u, float v, float depth, UpscaleExtent, TemporalSample&);
// Apply jitter only to a scene-owned copy. Never modify shared game camera
// constants used by Scaleform, plates, culling or shadow passes.
bool scene_projection_jitter(const SceneCamera&, TemporalSample render_pixels, UpscaleExtent, SceneCamera& out);

class SceneCameraHistory {
    SceneCamera previous_{};
    std::uint64_t previous_frame_ = 0, resource_epoch_ = 0;
    bool have_previous_ = false;
public:
    HistoryReset prepare(const SceneCamera&, std::uint64_t frame, std::uint64_t resource_epoch, SceneMatrix& reprojection) const;
    // Commit once after scene resolve, never per draw or during loading UI.
    void commit(const SceneCamera&, std::uint64_t frame, std::uint64_t resource_epoch);
    void clear() {have_previous_ = false;}
};
}
