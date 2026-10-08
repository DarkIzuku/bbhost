// GPU readback validation of camera motion. Synthetic depths/cameras only;
// does not certify object motion or enable Bloodborne's temporal dispatch.
#include "host/scene_motion_pass.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace gpu;
namespace {
void check(VkResult result,const char* label) {if(result!=VK_SUCCESS) throw std::runtime_error(label);}
struct Device {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkCommandPool pool{}; VkCommandBuffer commands{}; VkFence fence{};
    VkPhysicalDeviceMemoryProperties memory{};
    Device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo=&app;
        check(vkCreateInstance(&create,nullptr,&instance),"instance");
        unsigned n=0; check(vkEnumeratePhysicalDevices(instance,&n,nullptr),"devices");
        std::vector<VkPhysicalDevice> devices(n); check(vkEnumeratePhysicalDevices(instance,&n,devices.data()),"devices");
        if(!n) throw std::runtime_error("No Vulkan device"); physical=devices[0];
        for(auto candidate:devices) {VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate,&p); if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {physical=candidate; break;}}
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props); std::printf("Motion probe GPU: %s\n",props.deviceName);
        vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr); std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
        unsigned family=0; while(family<n && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;
        if(family==n) throw std::runtime_error("No compute queue");
        const float priority=1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex=family; q.queueCount=1; q.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(physical,&features);
        if(!features.shaderStorageImageExtendedFormats) throw std::runtime_error("Missing rg16f storage support");
        VkPhysicalDeviceFeatures enabled{}; enabled.shaderStorageImageExtendedFormats=VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pEnabledFeatures=&enabled; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&q;
        check(vkCreateDevice(physical,&dc,nullptr,&device),"device"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=family; check(vkCreateCommandPool(device,&cp,nullptr,&pool),"pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&commands),"commands"); VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fc,nullptr,&fence),"fence");
    }
    unsigned type(unsigned bits,VkMemoryPropertyFlags flags) {for(unsigned k=0;k<memory.memoryTypeCount;++k) if((bits&(1u<<k)) && (memory.memoryTypes[k].propertyFlags&flags)==flags) return k; throw std::runtime_error("memory type");}
    void begin() {check(vkResetCommandPool(device,pool,0),"reset pool"); VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(commands,&b),"begin");}
    void submit() {check(vkEndCommandBuffer(commands),"end"); VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&commands; check(vkQueueSubmit(queue,1,&s,fence),"submit"); check(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ull),"wait"); check(vkResetFences(device,1,&fence),"reset fence");}
    ~Device() {if(device) {vkDeviceWaitIdle(device); if(fence) vkDestroyFence(device,fence,nullptr); if(pool) vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr);} if(instance) vkDestroyInstance(instance,nullptr);}
};
struct Image {
    Device& d; UpscaleImage value; VkDeviceMemory memory{}; VkImageAspectFlags aspect;
    Image(Device& device,VkFormat format,bool depth):d(device),aspect(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT) {
        value.format=format; value.extent={80,48};
        VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; c.imageType=VK_IMAGE_TYPE_2D; c.format=format; c.extent={80,48,1}; c.mipLevels=c.arrayLayers=1; c.samples=VK_SAMPLE_COUNT_1_BIT; c.tiling=VK_IMAGE_TILING_OPTIMAL;
        c.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; if(!depth) c.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
        check(vkCreateImage(d.device,&c,nullptr,&value.image),"image"); VkMemoryRequirements req{}; vkGetImageMemoryRequirements(d.device,value.image,&req);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=req.size; a.memoryTypeIndex=d.type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(d.device,&a,nullptr,&memory),"image memory"); check(vkBindImageMemory(d.device,value.image,memory,0),"bind image");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=value.image; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=format; v.subresourceRange={aspect,0,1,0,1}; check(vkCreateImageView(d.device,&v,nullptr,&value.view),"view");
    }
    void barrier(VkPipelineStageFlags src,VkAccessFlags access,VkPipelineStageFlags dst,VkAccessFlags next) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=access; b.dstAccessMask=next; b.oldLayout=value.layout; b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.image=value.image;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.subresourceRange={aspect,0,1,0,1}; vkCmdPipelineBarrier(d.commands,src,dst,0,0,nullptr,0,nullptr,1,&b); value.layout=VK_IMAGE_LAYOUT_GENERAL;
    }
    void clear(float depth) {
        const bool fresh=value.layout==VK_IMAGE_LAYOUT_UNDEFINED;
        barrier(fresh?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,fresh?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{aspect,0,1,0,1};
        if(aspect==VK_IMAGE_ASPECT_DEPTH_BIT) {VkClearDepthStencilValue c{depth,0}; vkCmdClearDepthStencilImage(d.commands,value.image,value.layout,&c,1,&range);}
        else {VkClearColorValue c{{9,-11,0,0}}; vkCmdClearColorImage(d.commands,value.image,value.layout,&c,1,&range);}
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    }
    ~Image() {if(value.view) vkDestroyImageView(d.device,value.view,nullptr); if(value.image) vkDestroyImage(d.device,value.image,nullptr); if(memory) vkFreeMemory(d.device,memory,nullptr);}
};
float half(unsigned short h) {unsigned e=(h>>10)&31,m=h&1023; const float v=e==31?(m?NAN:INFINITY):std::ldexp(e?1+m/1024.f:m/1024.f,e?static_cast<int>(e)-15:-14); return h&0x8000?-v:v;}
void run(Device& d,SceneMotionPass& pass,VkDescriptorSet set,VkSampler sampler,Image& depth,Image& motion,const SceneMatrix& matrix,float z,TemporalSample jitter,const char* label) {
    constexpr UpscaleExtent render{65,39}; constexpr VkDeviceSize bytes=80*48*4;
    VkBuffer buffer{}; VkDeviceMemory memory{};
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; b.size=bytes; b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; check(vkCreateBuffer(d.device,&b,nullptr,&buffer),"readback");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(d.device,buffer,&req); VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=req.size; a.memoryTypeIndex=d.type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(d.device,&a,nullptr,&memory),"readback memory"); check(vkBindBufferMemory(d.device,buffer,memory,0),"bind readback");
    d.begin(); depth.clear(z); motion.clear(0);
    if(!pass.record(d.commands,set,sampler,depth.value,motion.value,render,matrix,jitter)) throw std::runtime_error("motion contract rejected");
    motion.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={80,48,1}; vkCmdCopyImageToBuffer(d.commands,motion.value.image,motion.value.layout,buffer,1,&copy);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT; vkCmdPipelineBarrier(d.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr); d.submit();
    void* mapped{}; check(vkMapMemory(d.device,memory,0,bytes,0,&mapped),"map"); const auto* values=static_cast<const unsigned short*>(mapped); unsigned failures=0; float error=0;
    for(unsigned y=0;y<48;++y) for(unsigned x=0;x<80;++x) {
        TemporalSample wanted{9,-11};
        if(x<render.width && y<render.height) {wanted={}; scene_camera_motion(matrix,(x+.5f-jitter.x)/render.width,(y+.5f-jitter.y)/render.height,z,render,wanted);}
        for(unsigned c=0;c<2;++c) {const float target=c?wanted.y:wanted.x,actual=half(values[(y*80+x)*2+c]);
            if(!std::isfinite(actual) || std::fabs(actual-target)>.003f+std::fabs(target)*.001f) ++failures;
            else error=std::max(error,std::fabs(actual-target));}
    }
    vkUnmapMemory(d.device,memory); vkDestroyBuffer(d.device,buffer,nullptr); vkFreeMemory(d.device,memory,nullptr);
    std::printf("Motion %s: failures=%u, maximum error=%g pixels; padded region checked\n",label,failures,error);
    if(failures) throw std::runtime_error("motion GPU readback disagrees with CPU reprojection");
}
}
int main() {
    try {
        Device d; SceneMotionPass pass(d.device); if(!pass.ready()) throw std::runtime_error("pipeline");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
        VkDescriptorPoolCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; p.maxSets=1; p.poolSizeCount=3; p.pPoolSizes=sizes; VkDescriptorPool pool{}; check(vkCreateDescriptorPool(d.device,&p,nullptr,&pool),"descriptor pool");
        auto layout=pass.descriptor_layout(); VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; a.descriptorPool=pool; a.descriptorSetCount=1; a.pSetLayouts=&layout; VkDescriptorSet set{}; check(vkAllocateDescriptorSets(d.device,&a,&set),"set");
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; s.magFilter=s.minFilter=VK_FILTER_NEAREST; s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; VkSampler sampler{}; check(vkCreateSampler(d.device,&s,nullptr,&sampler),"sampler");
        Image depth(d,VK_FORMAT_D32_SFLOAT,true), motion(d,VK_FORMAT_R16G16_SFLOAT,false);
        SceneCamera current{{1.5f,0,0,0,0,2.0f,0,0,0,0,1.001f,-.1001f,0,0,1,0},{264,-51,65}},previous=current;
        SceneMatrix matrix{}; if(!scene_reprojection(current,previous,matrix)) throw std::runtime_error("static camera");
        run(d,pass,set,sampler,depth,motion,matrix,.99f,{},"static");
        previous.world_origin[0]-=.02f; previous.world_origin[1]+=.03f; if(!scene_reprojection(current,previous,matrix)) throw std::runtime_error("translation");
        run(d,pass,set,sampler,depth,motion,matrix,.5f,{},"near translation"); run(d,pass,set,sampler,depth,motion,matrix,.99f,{.25f,-.375f},"far translation with jitter removal");
        previous=current; const float angle=.04f,co=std::cos(angle),si=std::sin(angle);
        previous.clip_from_relative_world={1.5f*co,0,1.5f*si,0,0,2,0,0,-1.001f*si,0,1.001f*co,-.1001f,-si,0,co,0};
        if(!scene_reprojection(current,previous,matrix)) throw std::runtime_error("rotation"); run(d,pass,set,sampler,depth,motion,matrix,.99f,{},"camera yaw");
        matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,-1}; run(d,pass,set,sampler,depth,motion,matrix,.5f,{},"behind previous camera");
        vkDestroySampler(d.device,sampler,nullptr); vkDestroyDescriptorPool(d.device,pool,nullptr);
        std::puts("Camera motion GPU checks passed. Object/pose motion and in-game DLSS remain separate requirements."); return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"Motion probe failed: %s\n",e.what()); return 1;}
}
