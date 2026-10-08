#include "host/scene_pass_graph.h"
namespace gpu {
void ScenePassGraph::begin(std::uint64_t frame) {
    if(frame_==frame) return;
    frame_=frame;content_.clear();boundary_=false;
}
void ScenePassGraph::seed(std::span<const std::uint64_t> targets) {
    for(auto t:targets) if(t) content_[t]|=Scene;
}
bool ScenePassGraph::scene_owned(std::uint64_t target) const {
    const auto found=content_.find(target);
    return found!=content_.end() && found->second==Scene;
}
std::uint64_t ScenePassGraph::observe(ScenePassKind kind,bool full,std::span<const std::uint64_t> reads,std::span<const std::uint64_t> writes) {
    unsigned inputs=0;std::uint64_t scene=0;bool ambiguous=false;
    for(auto r:reads) {
        const auto found=content_.find(r);if(found==content_.end()) continue;
        inputs|=found->second;
        if(found->second==Scene) {if(scene && scene!=r) ambiguous=true;scene=r;}
    }
    std::uint64_t resolve=0;
    if(!boundary_ && full) {
        if(kind==ScenePassKind::Scaleform) {
            for(auto w:writes) if(scene_owned(w)) {if(resolve && resolve!=w) {ambiguous=true;break;}resolve=w;}
        } else if((inputs&Ui) && scene && !ambiguous) resolve=scene;
        if(ambiguous) resolve=0;
    }
    if(resolve) boundary_=true;
    for(auto w:writes) if(w) {
        unsigned& contents=content_[w];
        // Attachment contents survive blending/incremental scene draws.
        if(kind==ScenePassKind::Scaleform) contents|=Ui;
        else contents|=inputs;
    }
    // A malformed/unsupported frame must not grow unbounded diagnostic state.
    if(content_.size()>1024) {content_.clear();boundary_=true;}
    return resolve;
}
}
