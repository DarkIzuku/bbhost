// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in hardware integration test of bbhost's real DLSS provider. This uses
// synthetic uniform scenes; no game, save, online account or copyrighted data.
#include "host/dlss_ngx.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace gpu;
namespace {
void vk_check(VkResult r, const char* what) { if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": Vulkan " + std::to_string(r)); }
struct Device {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; unsigned family = 0;
    VkCommandPool pool{}; VkCommandBuffer commands{}; VkFence fence{};
    std::vector<std::function<void()>> deferred;
    unsigned retirement_count = 0;
    bool gpu_pending = false;
    VkPhysicalDeviceMemoryProperties memory{};
    unsigned memory_type(unsigned bits, VkMemoryPropertyFlags required) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i) if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & required) == required) return i;
        throw std::runtime_error("No suitable Vulkan memory type");
    }
    void retire() { for (auto& f : deferred) { f(); ++retirement_count; } deferred.clear(); }
    void begin() {
        vk_check(vkResetCommandPool(device, pool, 0), "reset command pool");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(commands, &b), "begin commands");
    }
    void submit() {
        vk_check(vkEndCommandBuffer(commands), "end commands");
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &commands;
        vk_check(vkQueueSubmit(queue, 1, &s, fence), "submit DLSS commands");
        gpu_pending = true;
        vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull), "wait DLSS test fence");
        gpu_pending = false;
        vk_check(vkResetFences(device, 1, &fence), "reset fence"); retire();
    }
    ~Device() {
        // Diagnostic teardown only; no per-frame idle wait exists in provider.
        if (device) { vkDeviceWaitIdle(device); retire(); if (pool) vkDestroyCommandPool(device, pool, nullptr); if (fence) vkDestroyFence(device, fence, nullptr); vkDestroyDevice(device, nullptr); }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
struct Image {
    Device& d; UpscaleImage image; VkDeviceMemory memory{}; VkImageAspectFlags aspect;
    Image(Device& device, VkFormat format, UpscaleExtent size, bool depth = false) : d(device), aspect(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT) {
        image.format = format; image.extent = size;
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType = VK_IMAGE_TYPE_2D; create.format = format;
        create.extent = {size.width, size.height, 1}; create.mipLevels = create.arrayLayers = 1; create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!depth) create.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        vk_check(vkCreateImage(d.device, &create, nullptr, &image.image), "create test image");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(d.device, image.image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize = req.size; alloc.memoryTypeIndex = d.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(d.device, &alloc, nullptr, &memory), "allocate test image"); vk_check(vkBindImageMemory(d.device, image.image, memory, 0), "bind test image");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image = image.image; v.viewType = VK_IMAGE_VIEW_TYPE_2D; v.format = format;
        v.subresourceRange = {aspect, 0, 1, 0, 1}; vk_check(vkCreateImageView(d.device, &v, nullptr, &image.view), "create test view");
    }
    void barrier(VkPipelineStageFlags src, VkAccessFlags access, VkPipelineStageFlags dst, VkAccessFlags next) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = access; b.dstAccessMask = next;
        b.oldLayout = image.layout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.image = image.image;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.subresourceRange = {aspect | (image.format==VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u), 0, 1, 0, 1};
        vkCmdPipelineBarrier(d.commands, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b); image.layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    void clear(std::array<float, 4> rgba) {
        barrier(image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) { VkClearDepthStencilValue v{rgba[0], 0}; vkCmdClearDepthStencilImage(d.commands, image.image, image.layout, &v, 1, &range); }
        else { VkClearColorValue v{{rgba[0], rgba[1], rgba[2], rgba[3]}}; vkCmdClearColorImage(d.commands, image.image, image.layout, &v, 1, &range); }
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    ~Image() {
        if (d.gpu_pending) { vkDeviceWaitIdle(d.device); d.gpu_pending = false; }
        if (image.view) vkDestroyImageView(d.device, image.view, nullptr); if (image.image) vkDestroyImage(d.device, image.image, nullptr); if (memory) vkFreeMemory(d.device, memory, nullptr);
    }
};
float half(unsigned short h) {
    const unsigned e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 31 ? (m ? NAN : INFINITY) : std::ldexp(e ? 1 + m / 1024.0f : m / 1024.0f, e ? static_cast<int>(e) - 15 : -14);
    return h & 0x8000 ? -v : v;
}
void read_output(Device& d, Image& output) {
    const bool bgra=output.image.format==VK_FORMAT_B8G8R8A8_UNORM;
    const VkDeviceSize pixel_bytes=bgra ? 4 : 8;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(output.image.extent.width) * output.image.extent.height * pixel_bytes;
    VkBuffer buffer{}; VkDeviceMemory memory{};
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; b.size = bytes; b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vk_check(vkCreateBuffer(d.device, &b, nullptr, &buffer), "create readback");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(d.device, buffer, &req);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize = req.size; a.memoryTypeIndex = d.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(d.device, &a, nullptr, &memory), "allocate readback"); vk_check(vkBindBufferMemory(d.device, buffer, memory, 0), "bind readback");
    d.begin(); output.barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {output.image.extent.width, output.image.extent.height, 1};
    vkCmdCopyImageToBuffer(d.commands, output.image.image, output.image.layout, buffer, 1, &copy);
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(d.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m, 0, nullptr, 0, nullptr); d.submit();
    void* data = nullptr; vk_check(vkMapMemory(d.device, memory, 0, bytes, 0, &data), "map readback");
    auto values = static_cast<const unsigned short*>(data); unsigned bad = 0; float largest_error = 0;
    const float wanted[3] = {0.25f, 0.5f, 0.75f};
    for (VkDeviceSize i = 0; i < bytes / pixel_bytes; ++i) for (unsigned c = 0; c < 3; ++c) {
        const auto v = bgra ? static_cast<const unsigned char*>(data)[i*4+2-c]/255.0f : half(values[i * 4 + c]); if (!std::isfinite(v)) ++bad; else largest_error = std::max(largest_error, std::abs(v - wanted[c]));
    }
    vkUnmapMemory(d.device, memory); vkDestroyBuffer(d.device, buffer, nullptr); vkFreeMemory(d.device, memory, nullptr);
    std::printf("DLSS output: %ux%u, nonfinite=%u, maximum color error=%g\n", output.image.extent.width, output.image.extent.height, bad, largest_error);
    if (bad || largest_error > 0.08f) throw std::runtime_error("DLSS synthetic output failed readback validation");
}
void run_mode(Device& d, DlssProvider& provider, UpscaleExtent render, UpscaleExtent out, UpscalePreset preset,bool native_formats=false) {
    DlssOptimalSettings optimal;
    if (!provider.optimal_settings(out, preset, optimal)) throw std::runtime_error(provider.problem());
    render = optimal.render;
    std::printf("DLSS optimal: %ux%u -> %ux%u, range %ux%u .. %ux%u\n", render.width, render.height, out.width, out.height,
                optimal.minimum.width, optimal.minimum.height, optimal.maximum.width, optimal.maximum.height);
    const auto format=native_formats ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
    Image color(d,format,render),depth(d,native_formats ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT,render,true),motion(d,VK_FORMAT_R16G16_SFLOAT,render),output(d,format,out);
    UpscaleConfig config; config.provider = UpscalerId::Dlss; config.preset = preset; config.render = render; config.output = out;
    UpscaleHistory history;
    for (unsigned frame = 0; frame < 16; ++frame) {
        d.begin(); color.clear({0.25f, 0.5f, 0.75f, 1}); depth.clear({0.5f, 0, 0, 0}); motion.clear({0, 0, 0, 0});
        output.barrier(output.image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       output.image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        UpscaleFrame f; f.commands = d.commands; f.color = color.image; f.depth = depth.image; f.motion = motion.image; f.output = output.image;
        f.stage = UpscaleStage::SceneBeforeUI; f.engine_jitter_applied = true; f.jitter = temporal_jitter(frame, 16); f.delta_seconds = 1.0f / 60;
        f.input_area.extent = {render.width, render.height}; f.output_area.extent = {out.width, out.height};
        // Uniform static infinite plane: zero object/camera motion and a jitter
        // invariant scene are exact synthetic inputs, not game reconstruction.
        if (frame == 8) history.invalidate(HistoryReset::CameraCut);
        f.reset = history.begin(config);
        if (!provider.record(config, f)) throw std::runtime_error(provider.problem().empty() ? "DLSS frame contract rejected" : provider.problem());
        d.submit(); history.commit();
    }
    read_output(d, output);
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool lifecycle_only = argc == 4 && std::strcmp(argv[3], "--lifecycle-only") == 0;
    if (argc != 3 && !lifecycle_only) { std::fprintf(stderr, "Usage: dlss_probe MODEL_DIRECTORY WRITABLE_CACHE_DIRECTORY [--lifecycle-only]\nUses synthetic frames only. Does not enable in-game DLSS.\n"); return 2; }
    try {
        Device d;
        {
            DlssProvider provider(argv[2], {argv[1]}, [&](auto f) { d.deferred.push_back(std::move(f)); });
            std::vector<std::string> iext, dext;
            if (!provider.instance_extensions(iext)) throw std::runtime_error(provider.problem());
            std::vector<const char*> pointers; for (auto& s : iext) pointers.push_back(s.c_str());
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "bbhost DLSS integration test"; app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app; create.enabledExtensionCount = static_cast<unsigned>(pointers.size()); create.ppEnabledExtensionNames = pointers.data();
            vk_check(vkCreateInstance(&create, nullptr, &d.instance), "create instance");
            unsigned count = 0; vk_check(vkEnumeratePhysicalDevices(d.instance, &count, nullptr), "enumerate devices");
            std::vector<VkPhysicalDevice> devices(count); vk_check(vkEnumeratePhysicalDevices(d.instance, &count, devices.data()), "enumerate devices");
            for (auto pd : devices) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p); if (p.vendorID == 0x10de && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { d.physical = pd; break; } }
            if (!d.physical) throw std::runtime_error("No NVIDIA discrete Vulkan device");
            if (!provider.device_extensions(d.instance, d.physical, dext)) throw std::runtime_error(provider.problem());
            unsigned num = 0; vk_check(vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &num, nullptr), "enumerate extensions");
            std::vector<VkExtensionProperties> have(num); vk_check(vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &num, have.data()), "enumerate extensions");
            pointers.clear(); for (auto& s : dext) {
                bool found = false; for (auto& e : have) if (s == e.extensionName) found = true;
                if (!found) throw std::runtime_error("Missing NGX extension: " + s); pointers.push_back(s.c_str()); std::printf("DLSS extension: %s\n", s.c_str());
            }
            vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &num, nullptr); std::vector<VkQueueFamilyProperties> families(num); vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &num, families.data());
            bool queue_found = false; for (unsigned i = 0; i < num; ++i) if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { d.family = i; queue_found = true; break; }
            if (!queue_found) throw std::runtime_error("No graphics/compute queue");
            float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = d.family; q.queueCount = 1; q.pQueuePriorities = &priority;
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f12; vkGetPhysicalDeviceFeatures2(d.physical, &f2);
            if (!f12.bufferDeviceAddress) throw std::runtime_error("Missing bufferDeviceAddress");
            f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.bufferDeviceAddress = VK_TRUE;
            VkDeviceCreateInfo c{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; c.pNext = &f12; c.queueCreateInfoCount = 1; c.pQueueCreateInfos = &q; c.enabledExtensionCount = static_cast<unsigned>(pointers.size()); c.ppEnabledExtensionNames = pointers.data();
            vk_check(vkCreateDevice(d.physical, &c, nullptr, &d.device), "create device"); vkGetDeviceQueue(d.device, d.family, 0, &d.queue); vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory);
            VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex = d.family; vk_check(vkCreateCommandPool(d.device, &cp, nullptr, &d.pool), "create pool");
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = d.pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1; vk_check(vkAllocateCommandBuffers(d.device, &ca, &d.commands), "allocate command buffer");
            VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk_check(vkCreateFence(d.device, &fc, nullptr, &d.fence), "create fence");
            if (!provider.initialize(d.instance, d.physical, d.device)) throw std::runtime_error(provider.problem());
            if (lifecycle_only) {
                DlssOptimalSettings optimal;
                if (!provider.optimal_settings({1920, 1080}, UpscalePreset::Quality, optimal)) throw std::runtime_error(provider.problem());
            } else {
            run_mode(d, provider, {1280, 720}, {1920, 1080}, UpscalePreset::Quality);
            run_mode(d, provider, {960, 540}, {1920, 1080}, UpscalePreset::Performance);
            run_mode(d, provider, {1114, 626}, {1920, 1080}, UpscalePreset::Balanced);
            run_mode(d, provider, {1920, 1080}, {1920, 1080}, UpscalePreset::NativeAA);
            run_mode(d, provider, {1706, 960}, {2560, 1440}, UpscalePreset::Quality);
            run_mode(d, provider, {1280, 720}, {3840, 2160}, UpscalePreset::UltraPerformance);
            run_mode(d, provider, {1706, 720}, {2560, 1080}, UpscalePreset::Quality);
            run_mode(d, provider, {1920, 1080}, {1920, 1080}, UpscalePreset::NativeAA,true);
            }
        }
        if (lifecycle_only) {
            // Runtime shutdown may retire on a different host thread. Exercise
            // the initialized-but-never-dispatched case independently of frames.
            std::thread([&] { d.retire(); }).join();
            std::printf("DLSS lifecycle: initialized, optimal size queried, retired on another thread (%u retirements). No scene or synthetic frames dispatched.\n", d.retirement_count);
        } else {
            d.retire(); std::printf("DLSS integration: 128 synthetic frames passed, all quality modes/native BGRA+D32S8/reset/retirement checked (%u retirements). Synthetic tests do not certify game dispatch.\n", d.retirement_count);
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "DLSS integration failed: %s\n", e.what()); return 1; }
}
