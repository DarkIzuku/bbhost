// Execute synthetic GCN on Vulkan: check sampled values, not just SPIR-V validity.
// No game files are needed. Exit 77 if no suitable Vulkan device is available.
#include "gcn/translate.h"
#include <spirv-tools/libspirv.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

static void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(r));
}

struct Fixture {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkBuffer buffer{};
    VkDeviceMemory buffer_mem{};
    void* mapped{};
    VkImage source{}, dest{};
    VkDeviceMemory source_mem{}, dest_mem{};
    VkImageView source_view{}, dest_view{};
    VkSampler sampler{};
    VkDescriptorSetLayout layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkImage cube_image{};
    VkDeviceMemory cube_mem{};
    VkImageView cube_view{};

    // Twelve 4x4 layers (two cubes of six faces), texel (x, y, layer, 1): what a
    // Sea Islands cube T# is bound as.
    void init_cube() {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = {4, 4, 1}; ci.mipLevels = 1; ci.arrayLayers = 12;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device, &ci, nullptr, &cube_image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, cube_image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
        check(vkAllocateMemory(device, &ai, nullptr, &cube_mem));
        check(vkBindImageMemory(device, cube_image, cube_mem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = cube_image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 12};
        check(vkCreateImageView(device, &vi, nullptr, &cube_view));
        VkBuffer staging{}; VkDeviceMemory staging_mem{};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 12 * 256; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &staging));
        vkGetBufferMemoryRequirements(device, staging, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ai, nullptr, &staging_mem));
        check(vkBindBufferMemory(device, staging, staging_mem, 0));
        void* map = nullptr;
        check(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &map));
        auto* px = static_cast<float*>(map);
        for (int l = 0; l < 12; ++l) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            const int i = ((l * 4 + y) * 4 + x) * 4;
            px[i] = float(x); px[i+1] = float(y); px[i+2] = float(l); px[i+3] = 1;
        }
        begin();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cube_image; b.subresourceRange = vi.subresourceRange;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        std::vector<VkBufferImageCopy> regions(12);
        for (std::uint32_t l = 0; l < 12; ++l) {
            regions[l].bufferOffset = l * 256; regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
            regions[l].imageExtent = {4, 4, 1};
        }
        vkCmdCopyBufferToImage(cmd, staging, cube_image, VK_IMAGE_LAYOUT_GENERAL, 12, regions.data());
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT); submit();
        vkDestroyBuffer(device, staging, nullptr); vkFreeMemory(device, staging_mem, nullptr);
    }

    void bind_sampled(VkImageView view) {
        VkDescriptorImageInfo info{{}, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = 16; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w.pImageInfo = &info;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }

    // A Sea Islands cube sample: (s, t) in [1, 2] on the face and the face id,
    // plus 8 per cube in an array, must read that face's texel as an array layer.
    bool run_cube(float s, float t, float face, int ex, int ey, int elayer) {
        gcn::Program p;
        auto add = [&](gcn::Inst in) { in.offset = uint32_t(p.insts.size() * 8); in.size = 2; p.insts.push_back(in); };
        auto mov = [&](int reg, float value) {
            gcn::Inst in; in.enc = gcn::Enc::VOP1; in.op = 1; in.dst = reg;
            in.src0 = gcn::kLiteral; in.has_literal = true; in.literal = std::bit_cast<uint32_t>(value); add(in);
        };
        mov(0, s); mov(1, t); mov(2, face);
        gcn::Inst sample; sample.enc = gcn::Enc::MIMG; sample.op = 39;  // image_sample_lz
        sample.srsrc = 0; sample.ssamp = 8; sample.vaddr = 0; sample.vdata = 16; sample.dmask = 15; add(sample);
        mov(24, 0); mov(25, 0);
        gcn::Inst store = sample; store.op = 8; store.vaddr = 24; add(store);
        gcn::Inst end; end.enc = gcn::Enc::SOPP; end.op = 1; add(end);
        gcn::TranslateOptions o; o.stage = gcn::Stage::Compute; o.rsrc2 = 12 << 1; o.cs_threads[0] = 1;
        o.sampler_force_unnormalized = {false};
        o.image_dims = {{3 /* DimCube */, false}};
        const auto translated = gcn::translate(p, o);
        if (!translated.ok()) throw std::runtime_error(translated.errors[0]);
        if (translated.images.empty() || !translated.images[0].cube || translated.images[0].dim != 1 || !translated.images[0].arrayed)
            throw std::runtime_error("a cube binding must be a 2D array");
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        validator.SetMessageConsumer([](auto, auto, const auto&, const char* msg) { std::fprintf(stderr,"%s\n",msg); });
        if (!validator.Validate(translated.spirv)) throw std::runtime_error("invalid SPIR-V");
        static_cast<gcn::StageParams*>(mapped)->user_sgpr[8] = 0;
        bind_sampled(cube_view);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = translated.spirv.size()*4; mi.pCode = translated.spirv.data(); VkShaderModule module{};
        check(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout = pipeline_layout;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main"; VkPipeline pipeline{};
        check(vkCreateComputePipelines(device, {}, 1, &ci, nullptr, &pipeline));
        begin(); barrier(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = 2048; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {1,1,1};
        vkCmdCopyImageToBuffer(cmd, dest, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT); submit();
        bind_sampled(source_view);
        const float* r = reinterpret_cast<float*>(static_cast<char*>(mapped) + 2048);
        const bool ok = std::abs(r[0]-ex)<0.01f && std::abs(r[1]-ey)<0.01f && std::abs(r[2]-elayer)<0.01f && std::abs(r[3]-1)<0.01f;
        std::printf("%s cube (s,t,face)=(%g,%g,%g): got (%g,%g,layer %g), expected (%d,%d,layer %d)\n",
                    ok ? "PASS" : "FAIL", s, t, face, r[0], r[1], r[2], ex, ey, elayer);
        vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
        return ok;
    }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no matching memory type");
    }
    void make_image(uint32_t size, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = {size, size, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device, &ci, nullptr, &image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
        check(vkAllocateMemory(device, &ai, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view));
    }
    void begin() {
        check(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &bi));
    }
    void submit() {
        check(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
        check(vkCreateFence(device, &fi, nullptr, &fence));
        check(vkQueueSubmit(queue, 1, &si, fence));
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull));
        vkDestroyFence(device, fence, nullptr);
    }
    void barrier(VkAccessFlags src, VkAccessFlags dst) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = src; mb.dstAccessMask = dst;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    bool init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return false;
        uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = 0;
        for (auto candidate : devices) {
            VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f.pNext = &v12;
            vkGetPhysicalDeviceFeatures2(candidate, &f);
            VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &subgroup;
            vkGetPhysicalDeviceProperties2(candidate, &props);
            const auto ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
            if (!f.features.shaderInt64 || !f.features.shaderStorageImageWriteWithoutFormat || !v12.bufferDeviceAddress ||
                props.properties.apiVersion < VK_API_VERSION_1_2 || !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                (subgroup.supportedOperations & ops) != ops) continue;
            uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, queues.data());
            for (uint32_t i = 0; i < n; ++i) if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                physical = candidate; family = i; break;
            }
            if (physical) break;
        }
        if (!physical) return false;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{}; features.shaderInt64 = VK_TRUE;
        features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        // The translator declares both formatless storage capabilities.
        VkPhysicalDeviceFeatures supported{}; vkGetPhysicalDeviceFeatures(physical, &supported);
        features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
        if (!features.shaderStorageImageReadWithoutFormat) return false;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; v12.bufferDeviceAddress = VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext = &v12;
        di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pEnabledFeatures = &features;
        check(vkCreateDevice(physical, &di, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ca, &cmd));
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, &buffer));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &ai, nullptr, &buffer_mem)); check(vkBindBufferMemory(device, buffer, buffer_mem, 0));
        check(vkMapMemory(device, buffer_mem, 0, VK_WHOLE_SIZE, 0, &mapped));
        std::memset(mapped, 0, 4096);
        auto* pixels = reinterpret_cast<float*>(static_cast<char*>(mapped) + 256);
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const int i = (y * 8 + x) * 4; pixels[i] = float(x); pixels[i+1] = float(y); pixels[i+2] = 0.25f; pixels[i+3] = 1;
        }
        make_image(8, source, source_mem, source_view); make_image(1, dest, dest_mem, dest_view);
        begin();
        std::array<VkImageMemoryBarrier, 2> transitions{};
        for (int i = 0; i < 2; ++i) {
            auto& b = transitions[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = i ? dest : source; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 2, transitions.data());
        VkBufferImageCopy copy{}; copy.bufferOffset = 256; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {8,8,1};
        vkCmdCopyBufferToImage(cmd, buffer, source, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT); submit();
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        check(vkCreateSampler(device, &sci, nullptr, &sampler));
        const std::array<VkDescriptorSetLayoutBinding,4> bindings = {{{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {16,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {48,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {81,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 4; li.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
        check(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout));
        std::array<VkDescriptorPoolSize,4> sizes{};
        for (int i = 0; i < 4; ++i) sizes[i] = {bindings[i].descriptorType,1};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 4; dpi.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool = descriptors; da.descriptorSetCount = 1; da.pSetLayouts = &layout;
        check(vkAllocateDescriptorSets(device, &da, &set));
        VkDescriptorBufferInfo ub{buffer,0,sizeof(gcn::StageParams)};
        std::array<VkDescriptorImageInfo,3> images = {{{{},source_view,VK_IMAGE_LAYOUT_GENERAL},{sampler,{},VK_IMAGE_LAYOUT_UNDEFINED},{ {},dest_view,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,4> writes{};
        for (int i = 0; i < 4; ++i) {
            auto& w = writes[i]; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set;
            w.dstBinding = bindings[i].binding; w.descriptorType = bindings[i].descriptorType; w.descriptorCount = 1;
            if (i == 0) w.pBufferInfo = &ub; else w.pImageInfo = &images[i-1];
        }
        vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
        return true;
    }
    bool run(bool unorm, bool force, bool gradients, float x, float y, int expected_x, int expected_y,
             bool branch = false) {
        gcn::Program p;
        auto add = [&](gcn::Inst in) { in.offset = uint32_t(p.insts.size() * 8); in.size = 2; p.insts.push_back(in); };
        auto mov = [&](int reg, float value) {
            gcn::Inst in; in.enc = gcn::Enc::VOP1; in.op = 1; in.dst = reg;
            in.src0 = gcn::kLiteral; in.has_literal = true; in.literal = std::bit_cast<uint32_t>(value); add(in);
        };
        int v = 0;
        if (gradients) { mov(v++, 0.0625f); mov(v++, 0); mov(v++, 0); mov(v++, 0.0625f); }
        mov(v++, x); mov(v++, y);
        if (branch) {
            gcn::Inst jump; jump.enc = gcn::Enc::SOPP; jump.op = 2; jump.imm = 3; add(jump);
            mov(v - 2, 0);  // unreachable poison; exercises the dispatcher fallback
        }
        gcn::Inst sample; sample.enc = gcn::Enc::MIMG; sample.op = gradients ? 34 : 39;
        sample.srsrc = 0; sample.ssamp = 8; sample.vaddr = 0; sample.vdata = 16; sample.dmask = 15; sample.unorm = unorm; add(sample);
        mov(24, 0); mov(25, 0);
        gcn::Inst store = sample; store.op = 8; store.vaddr = 24; store.unorm = false; add(store);
        gcn::Inst end; end.enc = gcn::Enc::SOPP; end.op = 1; add(end);
        gcn::TranslateOptions o; o.stage = gcn::Stage::Compute; o.rsrc2 = 12 << 1; o.cs_threads[0] = 1;
        o.sampler_force_unnormalized = {force};
        const auto translated = gcn::translate(p, o);
        if (!translated.ok()) throw std::runtime_error(translated.errors[0]);
        spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_2);
        validator.SetMessageConsumer([](auto, auto, const auto&, const char* msg) { std::fprintf(stderr,"%s\n",msg); });
        if (!validator.Validate(translated.spirv)) throw std::runtime_error("invalid SPIR-V");
        auto* params = static_cast<gcn::StageParams*>(mapped); params->user_sgpr[8] = force ? 1u << 15 : 0;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = translated.spirv.size()*4; mi.pCode = translated.spirv.data(); VkShaderModule module{};
        check(vkCreateShaderModule(device, &mi, nullptr, &module));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout = pipeline_layout;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main"; VkPipeline pipeline{};
        check(vkCreateComputePipelines(device, {}, 1, &ci, nullptr, &pipeline));
        begin(); barrier(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.bufferOffset = 2048; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {1,1,1};
        vkCmdCopyImageToBuffer(cmd, dest, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT); submit();
        const float* result = reinterpret_cast<float*>(static_cast<char*>(mapped) + 2048);
        const bool ok = std::abs(result[0]-expected_x)<0.01f && std::abs(result[1]-expected_y)<0.01f &&
                        std::abs(result[2]-0.25f)<0.01f && std::abs(result[3]-1)<0.01f;
        std::printf("%s unorm=%d force=%d gradients=%d uv=(%g,%g): got (%g,%g), expected (%d,%d)\n",
                    ok ? "PASS" : "FAIL",unorm,force,gradients,x,y,result[0],result[1],expected_x,expected_y);
        vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
        return ok;
    }
    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyDescriptorPool(device,descriptors,nullptr); vkDestroyPipelineLayout(device,pipeline_layout,nullptr);
            vkDestroyDescriptorSetLayout(device,layout,nullptr); vkDestroySampler(device,sampler,nullptr);
            vkDestroyImageView(device,source_view,nullptr); vkDestroyImageView(device,dest_view,nullptr);
            vkDestroyImageView(device,cube_view,nullptr); vkDestroyImage(device,cube_image,nullptr); vkFreeMemory(device,cube_mem,nullptr);
            vkDestroyImage(device,source,nullptr); vkDestroyImage(device,dest,nullptr);
            vkFreeMemory(device,source_mem,nullptr); vkFreeMemory(device,dest_mem,nullptr);
            if (mapped) vkUnmapMemory(device,buffer_mem);
            vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,buffer_mem,nullptr);
            vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr);
        }
        if (instance) vkDestroyInstance(instance,nullptr);
    }
};

int main() try {
    Fixture f;
    if (!f.init()) { std::puts("SKIP: suitable Vulkan device unavailable"); return 77; }
    bool ok = true;
    ok &= f.run(false,false,false,0.3125f,0.6875f,2,5);
    ok &= f.run(false,false,false,1.3125f,-0.3125f,2,5);
    ok &= f.run(true,false,false,2.5f,5.5f,2,5);
    ok &= f.run(false,true,false,2.5f,5.5f,2,5);
    ok &= f.run(false,false,true,0.3125f,0.6875f,2,5);
    ok &= f.run(true,false,true,2.5f,5.5f,2,5);
    ok &= f.run(false,false,false,0.3125f,0.6875f,2,5,true);
    f.init_cube();
    ok &= f.run_cube(1.375f, 1.625f, 3.0f, 1, 2, 3);    // face 3 of the first cube
    ok &= f.run_cube(1.125f, 1.875f, 0.0f, 0, 3, 0);    // face 0, far corner row
    ok &= f.run_cube(1.875f, 1.125f, 11.0f, 3, 0, 9);   // second cube (8 + face 3) is layer 9
    return ok ? 0 : 1;
} catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
