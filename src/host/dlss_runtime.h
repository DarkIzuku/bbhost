// DLSS uses bbhost's device and submission fences. Scene inputs remain an
// independent gate: initializing NGX does not enable temporal presentation.
#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "host/upscale_policy.h"

namespace gpu {
class UpscalerProvider;
void dlss_runtime_instance_locked(std::vector<std::string>& extensions);
void dlss_runtime_device_locked(VkInstance, VkPhysicalDevice, std::vector<const char*>& extensions);
void dlss_runtime_initialize_locked(VkInstance, VkPhysicalDevice, VkDevice);
bool dlss_runtime_shutdown_locked();
UpscalerProvider* dlss_runtime_scene_locked();
bool dlss_runtime_scene_size_locked(UpscaleExtent output, UpscaleExtent& render, UpscalePreset& preset);
void dlss_runtime_request_probe_locked();
bool dlss_runtime_probe_locked(std::string& reason);
}
