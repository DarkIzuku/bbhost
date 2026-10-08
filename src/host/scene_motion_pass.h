#pragma once
#include "host/scene_motion.h"
#include "host/upscaler.h"

namespace gpu {
// Caller owns images, queue, barriers and retirement. Destroy the pass only
// after its last command buffer has retired; no allocator or queue is added.
class SceneMotionPass {
    VkDevice device_{};
    VkDescriptorSetLayout set_layout_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
public:
    explicit SceneMotionPass(VkDevice, VkPipelineCache = VK_NULL_HANDLE);
    ~SceneMotionPass();
    SceneMotionPass(const SceneMotionPass&) = delete;
    SceneMotionPass& operator=(const SceneMotionPass&) = delete;
    bool ready() const {return pipeline_ != VK_NULL_HANDLE;}
    VkDescriptorSetLayout descriptor_layout() const {return set_layout_;}
    // Set allocated by the caller's existing per-submission descriptor pool.
    // Both images are GENERAL, outside a render pass, with write->read depth
    // and write->write output dependencies already established by the owner.
    bool record(VkCommandBuffer, VkDescriptorSet, VkSampler, const UpscaleImage& depth,
                const UpscaleImage& output, UpscaleExtent render, const SceneMatrix&, TemporalSample jitter);
};
}
