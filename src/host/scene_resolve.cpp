#include "host/scene_resolve.h"
#include "host/scene_motion_pass.h"
#include "host/gpu_internal.h"
#include "host/settings.h"
#include "engine/frame_rate.h"
#include "log.h"
#include <chrono>
#include <memory>
namespace gpu {
namespace {
struct Image {
    UpscaleImage image;ImageMemory memory;
    bool initialized=false;
    VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    void retire() {
        if(image.view) defer_destroy_private_view(image.view);
        if(image.image) defer_destroy_image(image.image,memory);
        image={};memory={};initialized=false;
    }
    bool acquire(VkFormat format,UpscaleExtent size,VkImageAspectFlags plane=VK_IMAGE_ASPECT_COLOR_BIT) {
        if(image.image && image.format==format && image.extent==size && aspect==plane) return true;
        retire();VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(g.phys,format,&props);
        const bool depth=plane==VK_IMAGE_ASPECT_DEPTH_BIT;
        const auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|(depth?0:VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if((props.optimalTilingFeatures&required)!=required) return false;
        VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};c.imageType=VK_IMAGE_TYPE_2D;c.format=format;
        c.extent={size.width,size.height,1};c.mipLevels=c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;
        c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=VK_IMAGE_USAGE_SAMPLED_BIT|(depth?0:VK_IMAGE_USAGE_STORAGE_BIT)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(vkCreateImage(g.device,&c,nullptr,&image.image)!=VK_SUCCESS) return false;
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(g.device,image.image,&req);
        if(!image_memory_alloc(req,memory) || vkBindImageMemory(g.device,image.image,memory.memory,memory.offset)!=VK_SUCCESS) {retire();return false;}
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;
        v.subresourceRange={plane,0,1,0,1};
        if(vkCreateImageView(g.device,&v,nullptr,&image.view)!=VK_SUCCESS) {retire();return false;}
        aspect=plane;image.format=format;image.extent=size;image.layout=VK_IMAGE_LAYOUT_GENERAL;return true;
    }
    void writable(VkCommandBuffer commands,VkAccessFlags access=VK_ACCESS_SHADER_WRITE_BIT,VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask=initialized ? VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT : 0;
        b.dstAccessMask=access;b.oldLayout=initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.image=image.image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={aspect,0,1,0,1};
        vkCmdPipelineBarrier(commands,initialized ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,stage,0,0,nullptr,0,nullptr,1,&b);
        initialized=true;
    }
};
Image motion,output,scene_input,scene_depth;std::shared_ptr<SceneMotionPass> pass;VkSampler sampler{};
VkImageView depth_view{};VkImage viewed_depth{};
SceneCameraHistory cameras;UpscaleHistory history;
std::chrono::steady_clock::time_point last_scene;
VkImage last_depth{},last_color{};
std::uint64_t resource_epoch=0;
std::uint64_t scene_evaluations=0,history_resets=0;
std::uint64_t resolved_source=0,resolved_frame=~0ull;
UpscaleImage image(const RtImage& r) {return {r.image,r.view,r.format,VK_IMAGE_LAYOUT_GENERAL,{r.width,r.height}};}
void dependency(VkCommandBuffer cmd,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;
    vkCmdPipelineBarrier(cmd,from,to,0,1,&b,0,nullptr,0,nullptr);
}
}
bool scene_resolve_locked(UpscalerProvider& provider,const SceneCamera& camera,std::uint64_t frame,const UpscaleConfig& requested,
    RtImage& depth,RtImage& color,UpscaleExtent color_picture,TemporalSample jitter) {
    const auto extent=requested.render;
    if(!provider.temporal() || !scene_camera_valid(camera) || !depth.initialised || !color.initialised ||
       depth.width<extent.width || depth.height<extent.height || !color_picture.width || !color_picture.height ||
       color.width<color_picture.width || color.height<color_picture.height) return false;
    render_end_pass_locked();begin_recording_locked();
    UpscaleConfig config=requested;config.provider=provider.id();
    if(last_depth!=depth.image || last_color!=color.image) {last_depth=depth.image;last_color=color.image;++resource_epoch;}
    config.resource_epoch=resource_epoch;
    if(!pass) pass=std::make_shared<SceneMotionPass>(g.device);
    if(!pass->ready() || !motion.acquire(VK_FORMAT_R16G16_SFLOAT,extent) || !output.acquire(color.format,config.output)) return false;
    auto color_input=image(color);
    if(!(color_picture==extent)) {
        // GX/YEBIS may composite the lower-resolution world into a native-size
        // post target. Preserve its alpha/effect channels through YEBIS first;
        // normalize the completed color to the provider's actual input extent.
        // Scene depth and camera motion already use the real GX render extent.
        VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(g.phys,color.format,&properties);
        const auto required=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&required)!=required || !scene_input.acquire(color.format,extent)) return false;
        scene_input.writable(g_cmd());
        dependency(g_cmd(),VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        b.srcOffsets[1]={static_cast<int>(color_picture.width),static_cast<int>(color_picture.height),1};
        b.dstOffsets[1]={static_cast<int>(extent.width),static_cast<int>(extent.height),1};
        vkCmdBlitImage(g_cmd(),color.image,VK_IMAGE_LAYOUT_GENERAL,scene_input.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
        dependency(g_cmd(),VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        color_input=scene_input.image;
    }
    if(!sampler) {
        VkSamplerCreateInfo c{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};c.magFilter=c.minFilter=VK_FILTER_NEAREST;
        c.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;c.addressModeU=c.addressModeV=c.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(g.device,&c,nullptr,&sampler)!=VK_SUCCESS) return false;
    }
    const auto set=alloc_set_locked(pass->descriptor_layout());if(!set) return false;
    auto depth_input=image(depth);
    if(depth.width!=extent.width || depth.height!=extent.height) {
        // The shared GX attachment can be native-size even though the world
        // occupies only its top-left render rectangle. Give camera motion and
        // NGX the same exact depth footprint as color/MVs; never advertise a
        // smaller extent for a larger Vulkan image, or interpolate depth.
        if(!scene_depth.acquire(depth.format,extent,VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
        scene_depth.writable(g_cmd(),VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        dependency(g_cmd(),VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        copy.extent={extent.width,extent.height,1};
        vkCmdCopyImage(g_cmd(),depth.image,VK_IMAGE_LAYOUT_GENERAL,scene_depth.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        dependency(g_cmd(),VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        depth_input=scene_depth.image;
    } else if(viewed_depth!=depth.image) {
        if(depth_view) defer_destroy_private_view(depth_view);depth_view={};viewed_depth={};
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=depth.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=depth.format;
        // An attachment view may contain both Z and stencil. A sampled depth
        // view must select Z alone; never substitute the color snapshot's R32.
        v.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        if(vkCreateImageView(g.device,&v,nullptr,&depth_view)!=VK_SUCCESS) return false;
        viewed_depth=depth.image;
    }
    const auto commands=g_cmd();motion.writable(commands);output.writable(commands);
    dependency(commands,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    SceneMatrix reprojection;const auto reset=cameras.prepare(camera,frame,config.resource_epoch,reprojection);
    if(depth_input.image==depth.image) depth_input.view=depth_view;
    if(!pass->record(commands,set,sampler,depth_input,motion.image,extent,reprojection,jitter)) return false;
    dependency(commands,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    UpscaleFrame inputs;inputs.commands=commands;inputs.color=color_input;inputs.depth=depth_input;inputs.motion=motion.image;inputs.output=output.image;
    inputs.input_area={{0,0},{extent.width,extent.height}};
    inputs.output_area={{0,0},{config.output.width,config.output.height}};inputs.stage=UpscaleStage::SceneBeforeUI;
    inputs.jitter=jitter;inputs.engine_jitter_applied=true;
    const auto now=std::chrono::steady_clock::now();
    inputs.delta_seconds=1.0f/std::max(frame_rate_game_fps(),1);
    if(reset==HistoryReset::None && last_scene.time_since_epoch().count()) {
        const auto elapsed=std::chrono::duration<float>(now-last_scene).count();
        if(elapsed>0 && elapsed<=1) inputs.delta_seconds=elapsed;
    }
    if(reset!=HistoryReset::None) history.invalidate(reset);
    inputs.reset=history.begin(config);
    if(inputs.reset!=HistoryReset::None)
        host_log("DLSS: scene after effects picture=%ux%u input=%ux%u output=%ux%u color-format=%d depth-source=%ux%u depth-input=%ux%u",
            color_picture.width,color_picture.height,extent.width,extent.height,config.output.width,config.output.height,
            color.format,depth.width,depth.height,depth_input.extent.width,depth_input.extent.height);
    if(!provider.supports(config,inputs) || !provider.record(config,inputs)) {history.invalidate(HistoryReset::BackendFailure);cameras.clear();return false;}
    dependency(commands,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={config.output.width,config.output.height,1};
    if(color.width>=config.output.width && color.height>=config.output.height) {
        vkCmdCopyImage(commands,output.image.image,VK_IMAGE_LAYOUT_GENERAL,color.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        resolved_source=0;
    } else {resolved_source=color.base;resolved_frame=frame;}
    dependency(commands,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    color.fill_last=false;history.commit();cameras.commit(camera,frame,config.resource_epoch);last_scene=now;
    ++scene_evaluations;if(inputs.reset!=HistoryReset::None) ++history_resets;
    return true;
}
VkImageView scene_resolve_view_locked(std::uint64_t source,std::uint64_t frame) {
    return source && source==resolved_source && frame==resolved_frame ? output.image.view : VK_NULL_HANDLE;
}
void scene_resolve_shutdown_locked() {
    if(!pass && !sampler && !motion.image.image && !output.image.image) return;
    host_log("DLSS: scene evaluations=%llu; history resets=%llu",static_cast<unsigned long long>(scene_evaluations),static_cast<unsigned long long>(history_resets));
    render_end_pass_locked();begin_recording_locked();motion.retire();output.retire();scene_input.retire();scene_depth.retire();
    if(depth_view) defer_destroy_private_view(depth_view);depth_view={};viewed_depth={};
    auto keep=std::move(pass);const auto old=sampler;sampler={};const auto device=g.device;
    g.slots[g.slot].retire_functions.push_back([keep=std::move(keep),old,device]{if(old) vkDestroySampler(device,old,nullptr);});
    cameras.clear();history.invalidate(HistoryReset::Load);
    last_scene={};last_depth={};last_color={};
    scene_evaluations=history_resets=0;
    resolved_source=0;resolved_frame=~0ull;
}
}
