#include "host/scene_motion_pass.h"
#include "host/shaders/scene_motion.spv.h"
#include <cmath>

namespace gpu {
namespace {
struct Push {SceneMatrix matrix; UpscaleExtent extent; TemporalSample jitter;};
static_assert(sizeof(Push)==80 && offsetof(Push,extent)==64 && offsetof(Push,jitter)==72);
}
SceneMotionPass::SceneMotionPass(VkDevice device, VkPipelineCache cache):device_(device) {
    if(!device) return;
    const VkDescriptorSetLayoutBinding bindings[3]={
        {0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount=3; set.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(device_,&set,nullptr,&set_layout_)!=VK_SUCCESS) return;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount=1; layout.pSetLayouts=&set_layout_; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(device_,&layout,nullptr,&layout_)!=VK_SUCCESS) return;
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.pCode=k_scene_motion_spv; shader.codeSize=sizeof(k_scene_motion_spv);
    VkShaderModule module{};
    if(vkCreateShaderModule(device_,&shader,nullptr,&module)!=VK_SUCCESS) return;
    VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    create.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; create.stage.module=module; create.stage.pName="main"; create.layout=layout_;
    if(vkCreateComputePipelines(device_,cache,1,&create,nullptr,&pipeline_)!=VK_SUCCESS) pipeline_=VK_NULL_HANDLE;
    vkDestroyShaderModule(device_,module,nullptr);
}
SceneMotionPass::~SceneMotionPass() {
    if(pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if(layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(set_layout_) vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
}
bool SceneMotionPass::record(VkCommandBuffer commands,VkDescriptorSet set,VkSampler sampler,
    const UpscaleImage& depth,const UpscaleImage& output,UpscaleExtent render,const SceneMatrix& matrix,TemporalSample jitter) {
    if(!ready() || !commands || !set || !sampler || !depth.image || !depth.view || !output.image || !output.view ||
       depth.image==output.image || depth.layout!=VK_IMAGE_LAYOUT_GENERAL || output.layout!=VK_IMAGE_LAYOUT_GENERAL ||
       output.format!=VK_FORMAT_R16G16_SFLOAT || !render.width || !render.height ||
       depth.extent.width<render.width || depth.extent.height<render.height ||
       output.extent.width<render.width || output.extent.height<render.height ||
       !std::isfinite(jitter.x) || !std::isfinite(jitter.y) || std::fabs(jitter.x)>0.5f || std::fabs(jitter.y)>0.5f) return false;
    for(float x:matrix) if(!std::isfinite(x)) return false;
    const VkDescriptorImageInfo images[3]={{VK_NULL_HANDLE,depth.view,VK_IMAGE_LAYOUT_GENERAL},
        {sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED},{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3]{};
    for(unsigned k=0;k<3;++k) {
        writes[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[k].dstSet=set; writes[k].dstBinding=k;
        writes[k].descriptorCount=1; writes[k].descriptorType=k==0?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:k==1?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[k].pImageInfo=images+k;
    }
    vkUpdateDescriptorSets(device_,3,writes,0,nullptr);
    const Push push{matrix,render,jitter};
    vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set,0,nullptr);
    vkCmdPushConstants(commands,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(commands,(render.width+7)/8,(render.height+7)/8,1);
    return true;
}
}
