#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>

namespace gpu {
enum class ScenePassKind { Engine, Yebis, Scaleform };
// Per-frame provenance over GX render targets, including offscreen Scaleform.
// A token's position alone cannot distinguish UI prepared before the scene
// from UI composited over it. Keys are the resource identities given by GX.
class ScenePassGraph {
    enum Content { Scene=1, Ui=2 };
    std::unordered_map<std::uint64_t,unsigned> content_;
    std::uint64_t frame_=~0ull;
    bool boundary_=false;
public:
    void begin(std::uint64_t frame);
    void seed(std::span<const std::uint64_t> scene_targets);
    bool scene_owned(std::uint64_t target) const;
    // Returns the scene image to resolve BEFORE this draw. For Scaleform it
    // is its destination; for a native UI composite it is the unique scene
    // input. Downscaled UI preparations and ambiguous composites are declined.
    std::uint64_t observe(ScenePassKind,bool full_viewport,
        std::span<const std::uint64_t> reads,std::span<const std::uint64_t> writes);
};
}
