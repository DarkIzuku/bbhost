// DLSS uses bbhost's device and submission fences. Scene inputs remain an
// independent gate: initializing NGX does not enable temporal presentation.
#pragma once
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace gpu {
void dlss_runtime_instance_locked(std::vector<std::string>& extensions);
void dlss_runtime_device_locked(VkInstance, VkPhysicalDevice, std::vector<const char*>& extensions);
void dlss_runtime_initialize_locked(VkInstance, VkPhysicalDevice, VkDevice);
bool dlss_runtime_shutdown_locked();
}
