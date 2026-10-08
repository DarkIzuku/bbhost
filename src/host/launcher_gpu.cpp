#include "host/launcher_gpu.h"
#include "host/gpu.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(BBHOST_HAVE_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace {
std::string quote(const char* s) {
    std::string r = "\"";
    for (; *s; ++s) { if (*s == '"' || *s == '\\') r += '\\'; if (static_cast<unsigned char>(*s) >= 32) r += *s; }
    return r + '"';
}
}
std::string launcher_gpu_json() {
#if defined(BBHOST_HAVE_VULKAN)
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Bloodborne PC GPU probe"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) return "{\"ok\":false,\"error\":\"A Vulkan 1.3 driver is required\"}";
    std::uint32_t count = 0; vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count); vkEnumeratePhysicalDevices(instance, &count, devices.data());
    std::string result = "{\"ok\":false,\"error\":\"No Vulkan GPU is available\"}";
    int selected = -1;
    std::vector<VkPhysicalDeviceProperties> props(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        vkGetPhysicalDeviceProperties(devices[i], &props[i]);
        if (selected < 0 || (props[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && props[selected].deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) selected = static_cast<int>(i);
    }
    if (const char* wanted = std::getenv("BBHOST_GPU_DEVICE"); wanted && *wanted) {
        char* end = nullptr; const long index = std::strtol(wanted, &end, 10);
        if (end && *end == 0 && index >= 0 && index < count) selected = static_cast<int>(index);
        else for (std::uint32_t i = 0; i < count; ++i) if (std::strstr(props[i].deviceName, wanted)) selected = static_cast<int>(i);
    }
    if (selected >= 0) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p.pNext = &driver;
        vkGetPhysicalDeviceProperties2(devices[selected], &p);
        result = "{\"ok\":true,\"name\":" + quote(p.properties.deviceName) + ",\"driver\":" + quote(driver.driverInfo) +
                 ",\"vendor\":" + std::to_string(p.properties.vendorID) + ",\"device\":" + std::to_string(p.properties.deviceID) +
                 ",\"api_version\":" + std::to_string(p.properties.apiVersion) + ",\"driver_version\":" + std::to_string(p.properties.driverVersion) + '}';
    }
    vkDestroyInstance(instance, nullptr);
    if(selected>=0) {
        std::string reason;
        const bool dlss=host_gpu_dlss_probe(reason);
        result.pop_back();result+=",\"dlss_available\":"+std::string(dlss?"true":"false")+",\"dlss_reason\":"+quote(reason.c_str())+'}';
    }
    return result;
#else
    return "{\"ok\":false,\"error\":\"This build has no Vulkan support\"}";
#endif
}
