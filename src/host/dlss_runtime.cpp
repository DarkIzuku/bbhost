#include "host/dlss_runtime.h"
#include "host/dlss_ngx.h"
#include "host/scene_resolve.h"
#include "host/gpu_internal.h"
#include "host/settings.h"
#include "core/config.h"
#include "log.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace gpu {
namespace {
// Explicit diagnostics until the actual scene/motion/jitter integration is
// verified. Native/FSR1 runs neither load NGX nor alter their extension list.
std::unique_ptr<DlssProvider> provider;
std::vector<std::string> device_extensions;

bool supported(const std::vector<std::string>& needed, const std::vector<VkExtensionProperties>& available) {
    for (const auto& name : needed) {
        if (std::none_of(available.begin(), available.end(), [&](const auto& e) {return name == e.extensionName;})) {
            host_log("DLSS: required Vulkan extension %s unavailable; native rendering retained", name.c_str());
            return false;
        }
    }
    return true;
}
void discard() { provider.reset(); device_extensions.clear(); }
}
void dlss_runtime_instance_locked(std::vector<std::string>& extensions) {
    const char* env = std::getenv("BBHOST_DLSS_INIT");
    const char* scene = std::getenv("BBHOST_DLSS_SCENE");
    if (!(env && std::string(env) == "1") && !(scene && std::string(scene) == "1") && config_value("upscaling.dlss_diagnostics") != "true") return;
    std::vector<std::string> paths{config_exe_dir(), (std::filesystem::path(config_exe_dir()) / "runtime/dlss").string()};
    if (const auto path = config_value("upscaling.dlss_model_dir"); !path.empty()) paths.push_back(path);
    if (const char* path = std::getenv("BBHOST_DLSS_MODEL_PATH"); path && *path) paths.emplace_back(path);
    const auto cache = std::filesystem::path(config().data.empty() ? config_default_data_dir() : config().data) / "bbhost/dlss";
    provider = std::make_unique<DlssProvider>(cache.string(), std::move(paths), [](std::function<void()> release) {
        // Even a context with no dispatch needs an ordered retirement. Start
        // the normal slot recording; its fence also orders all earlier uses.
        begin_recording_locked();
        g.slots[g.slot].retire_functions.push_back(std::move(release));
    });
    std::vector<std::string> needed;
    std::uint32_t n = 0;
    if (!provider->instance_extensions(needed) || vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS) {
        host_log("DLSS: extension discovery failed: %s; native rendering retained", provider->problem().c_str()); discard(); return;
    }
    std::vector<VkExtensionProperties> available(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data()) != VK_SUCCESS || !supported(needed, available)) { discard(); return; }
    for (const auto& name : needed) if (std::find(extensions.begin(), extensions.end(), name) == extensions.end()) extensions.push_back(name);
}
void dlss_runtime_device_locked(VkInstance instance, VkPhysicalDevice physical, std::vector<const char*>& extensions) {
    if (!provider) return;
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical, &props);
    if (props.vendorID != 0x10de) {host_log("DLSS: selected device is not NVIDIA; native rendering retained"); discard(); return;}
    std::vector<std::string> needed;
    std::uint32_t n = 0;
    if (!provider->device_extensions(instance, physical, needed) || vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr) != VK_SUCCESS) {
        host_log("DLSS: device extension discovery failed: %s; native rendering retained", provider->problem().c_str()); discard(); return;
    }
    std::vector<VkExtensionProperties> available(n);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data()) != VK_SUCCESS || !supported(needed, available)) {discard(); return;}
    device_extensions = std::move(needed); // Strings outlive vkCreateDevice.
    for (const auto& name : device_extensions) {
        if (std::none_of(extensions.begin(), extensions.end(), [&](const char* e) {return name == e;})) extensions.push_back(name.c_str());
        host_log("DLSS: runtime device requirement %s", name.c_str());
    }
}
void dlss_runtime_initialize_locked(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
    if (!provider || !device) return;
    if (!provider->initialize(instance, physical, device)) {
        host_log("DLSS: runtime initialization unavailable: %s; native rendering retained", provider->problem().c_str());
        provider.reset(); // Retire NGX resources through the existing slot.
        return;
    }
    DlssOptimalSettings optimal;
    const auto settings = host_settings();
    if (provider->optimal_settings({static_cast<std::uint32_t>(settings.res_width), static_cast<std::uint32_t>(settings.res_height)}, UpscalePreset::Quality, optimal)) {
        host_log("DLSS: real bbhost Vulkan device ready; Quality render size %ux%u. Scene dispatch is still gated on verified motion and jitter.", optimal.render.width, optimal.render.height);
    }
    // No provider registration at presentation. The scene owner will install
    // it only once its engine inputs and pre-Scaleform boundary are ready.
}
bool dlss_runtime_shutdown_locked() {
    scene_resolve_shutdown_locked();
    if (!provider) return false;
    upscale_set_temporal_provider_locked(nullptr);
    provider.reset();
    return true;
}
UpscalerProvider* dlss_runtime_scene_locked() {
    const char* scene=std::getenv("BBHOST_DLSS_SCENE");
    return scene && std::string(scene)=="1" && provider && provider->available() ? provider.get() : nullptr;
}
}
