// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime ABI and parameter bindings adapted from DarkIzuku/bloodborne_pc
// upstream-0.3-core-port, vk_dlss.cpp. Resource and lifetime policy is bbhost's.
#include "host/dlss_ngx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#else
#include <dlfcn.h>
#endif

namespace gpu {
namespace {
using Result = unsigned;
constexpr Result success = 1;
constexpr unsigned api_version = 0x15;
constexpr int super_sampling = 1;
constexpr const char* project_id = "71bb0db3-6622-5293-afdd-8c9c4073076b";
struct Parameter;
struct Handle;
struct Paths { const wchar_t* const* paths; unsigned count; };
struct Logging { void (*callback)(const char*, int, int); int level; bool disabled; };
struct Common { Paths paths; void* internal; Logging logging; };
struct Project { const char* id; int engine; const char* version; };
struct Identifier { int kind; union { Project project; unsigned long long app; } value; };
struct Discovery { int version, feature; Identifier id; const wchar_t* cache; const Common* common; };
struct Requirement { unsigned support, min_arch; char min_os[255]; };
struct ImageInfo { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned width, height; };
struct Resource { union { ImageInfo image; struct { VkBuffer buffer; unsigned bytes; } buffer; } value; int type; bool rw; };
static_assert(sizeof(Paths) == 16 && sizeof(Common) == 40 && sizeof(Discovery) == 56);
static_assert(sizeof(Resource) == 56);

// Driver parameter interface uses MSVC overload ordering on Windows even when
// bbhost is built with Clang/MinGW. No C++ virtual call crosses that ABI.
#ifdef _WIN32
enum { set_void = 0, set_int = 3, set_uint = 4, set_float = 6, get_void = 8, get_int = 11, get_uint = 12, get_float = 14 };
#else
enum { set_float = 1, set_uint = 3, set_int = 4, set_void = 7, get_float = 9, get_uint = 11, get_int = 12, get_void = 15 };
#endif
template<class F> F slot(Parameter* p, int n) { return reinterpret_cast<F>((*reinterpret_cast<void***>(p))[n]); }
void si(Parameter* p, const char* key, int v) { slot<void(*)(Parameter*, const char*, int)>(p, set_int)(p, key, v); }
void su(Parameter* p, const char* key, unsigned v) { slot<void(*)(Parameter*, const char*, unsigned)>(p, set_uint)(p, key, v); }
void sf(Parameter* p, const char* key, float v) { slot<void(*)(Parameter*, const char*, float)>(p, set_float)(p, key, v); }
void sp(Parameter* p, const char* key, void* v) { slot<void(*)(Parameter*, const char*, void*)>(p, set_void)(p, key, v); }
Result gi(Parameter* p, const char* key, int* v) { return slot<Result(*)(Parameter*, const char*, int*)>(p, get_int)(p, key, v); }
Result gu(Parameter* p, const char* key, unsigned* v) { return slot<Result(*)(Parameter*, const char*, unsigned*)>(p, get_uint)(p, key, v); }
Result gf(Parameter* p, const char* key, float* v) { return slot<Result(*)(Parameter*, const char*, float*)>(p, get_float)(p, key, v); }
Result gp(Parameter* p, const char* key, void** v) { return slot<Result(*)(Parameter*, const char*, void**)>(p, get_void)(p, key, v); }
bool abi_ok(Parameter* p) {
    int i = 0; unsigned u = 0; float f = 0; void* pointer = nullptr;
    si(p, "bbhost.Probe.Int", 0x5a5a); su(p, "bbhost.Probe.Uint", 77); sf(p, "bbhost.Probe.Float", 1.25f); sp(p, "bbhost.Probe.Pointer", p);
    return gi(p, "bbhost.Probe.Int", &i) == success && i == 0x5a5a && gu(p, "bbhost.Probe.Uint", &u) == success && u == 77 &&
           gf(p, "bbhost.Probe.Float", &f) == success && f == 1.25f && gp(p, "bbhost.Probe.Pointer", &pointer) == success && pointer == p;
}
void ngx_log(const char* s, int, int) { if (s) std::fprintf(stderr, "DLSS: NGX %s\n", s); }
struct Core {
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void* module = nullptr;
#endif
    Result (*instance_ext)(const Discovery*, unsigned*, VkExtensionProperties**) = nullptr;
    Result (*device_ext)(VkInstance, VkPhysicalDevice, const Discovery*, unsigned*, VkExtensionProperties**) = nullptr;
    Result (*legacy_extensions)(unsigned*, const char***, unsigned*, const char***) = nullptr;
    Result (*requirements)(VkInstance, VkPhysicalDevice, const Discovery*, Requirement*) = nullptr;
    // Driver's ProjectID_Ext ABI (SDK's Init_with_ProjectID has a different
    // argument order). Verified against the driver and public proxy sources.
    Result (*init)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                   PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, unsigned, const Common*) = nullptr;
    Result (*capabilities)(Parameter**) = nullptr;
    Result (*allocate)(Parameter**) = nullptr;
    Result (*destroy)(Parameter*) = nullptr;
    Result (*create)(VkDevice, VkCommandBuffer, int, Parameter*, Handle**) = nullptr;
    Result (*evaluate)(VkCommandBuffer, const Handle*, const Parameter*, void*) = nullptr;
    Result (*release)(Handle*) = nullptr;
#ifdef _WIN32
    // This is the driver's export, not the one-argument SDK wrapper. The
    // wrapper supplies a second output containing the remaining init count.
    // Omitting it writes through an arbitrary RDX on Windows x64.
    Result (*shutdown)(VkDevice, unsigned*) = nullptr;
#else
    Result (*shutdown)(VkDevice) = nullptr;
#endif
    Result shutdown_device(VkDevice device) const {
#ifdef _WIN32
        unsigned remaining = 0;
        const auto result = shutdown(device, &remaining);
        if (std::getenv("BBHOST_DLSS_LOG")) std::fprintf(stderr, "DLSS: NGX shutdown status=0x%08x; remaining initializations=%u\n", result, remaining);
        return result;
#else
        return shutdown(device);
#endif
    }
    template<class F> bool load(F& f, const char* name) {
#ifdef _WIN32
        f = reinterpret_cast<F>(GetProcAddress(module, name));
#else
        f = reinterpret_cast<F>(dlsym(module, name));
#endif
        return f != nullptr;
    }
    bool open() {
#ifdef _WIN32
        wchar_t path[32768]{}; DWORD size = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                         RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return false;
        const auto file = std::filesystem::path(path) / L"_nvngx.dll";
        if (!file.is_absolute()) return false;
        // Load only the driver's absolute registry path, never a game/dump DLL.
        module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        module = dlopen("libnvidia-ngx.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
#endif
        if (!module) return false;
        load(instance_ext, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
        load(device_ext, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");
        load(legacy_extensions, "NVSDK_NGX_VULKAN_RequiredExtensions");
        const bool ready = ((instance_ext && device_ext) || legacy_extensions) &&
               load(requirements, "NVSDK_NGX_VULKAN_GetFeatureRequirements") && load(init, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext") &&
               load(capabilities, "NVSDK_NGX_VULKAN_GetCapabilityParameters") && load(allocate, "NVSDK_NGX_VULKAN_AllocateParameters") &&
               load(destroy, "NVSDK_NGX_VULKAN_DestroyParameters") && load(create, "NVSDK_NGX_VULKAN_CreateFeature1") &&
               load(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature") && load(release, "NVSDK_NGX_VULKAN_ReleaseFeature") &&
               load(shutdown, "NVSDK_NGX_VULKAN_Shutdown1");
#ifdef _WIN32
        // Keep the driver core's code resident through Vulkan device teardown,
        // which may still reference callbacks installed by NGX. Feature,
        // parameter and GPU memory lifetimes are retired normally; pinning
        // the module does not keep those resources alive. The old binding
        // also left its driver module loaded for the process lifetime.
        HMODULE pinned = nullptr;
        return ready && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                          reinterpret_cast<LPCWSTR>(init), &pinned);
#else
        return ready;
#endif
    }
    ~Core() {
#ifdef _WIN32
        if (module) FreeLibrary(module);
#else
        if (module) dlclose(module);
#endif
    }
};
int quality(UpscalePreset p) {
    switch (p) { case UpscalePreset::NativeAA: return 5; case UpscalePreset::Quality: return 2; case UpscalePreset::Balanced: return 1;
    case UpscalePreset::Performance: return 0; case UpscalePreset::UltraPerformance: return 3; case UpscalePreset::Custom: return -1; }
    return -1;
}
Resource resource(const UpscaleImage& i, bool depth, bool rw) {
    Resource r{}; r.value.image = {i.view, i.image, {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, i.format, i.extent.width, i.extent.height};
    r.type = 0; r.rw = rw; return r;
}
bool sampled_layout(VkImageLayout l) { return l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; }
bool image_valid(const UpscaleImage& i, UpscaleExtent minimum) { return i.image && i.view && i.extent.width >= minimum.width && i.extent.height >= minimum.height; }
bool color_format(VkFormat f) { return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_R8G8B8A8_UNORM; }
bool hdr_format(VkFormat f) { return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT; }
std::string loaded_model_version() {
#ifdef _WIN32
    for (const auto* name : {L"nvngx_dlss.dll", L"_nvngx_dlss.dll"}) {
        const auto module = GetModuleHandleW(name);
        if (!module) continue;
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(module, path, 32768);
        if (!length || length >= 32768) continue;
        const DWORD size = GetFileVersionInfoSizeW(path, nullptr);
        if (!size || size > (1u << 20)) continue;
        std::vector<unsigned char> data(size);
        if (!GetFileVersionInfoW(path, 0, size, data.data())) continue;
        VS_FIXEDFILEINFO* info = nullptr; UINT bytes = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &bytes) ||
            !info || bytes < sizeof(*info) || info->dwSignature != 0xfeef04bd) continue;
        char version[64];
        std::snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                      LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        return version;
    }
#endif
    return {};
}
}
bool dlss_frame_contract(const UpscaleConfig& c, const UpscaleFrame& f) {
    if (c.provider != UpscalerId::Dlss || quality(c.preset) < 0 || !f.commands || !c.render.width || !c.render.height || !c.output.width || !c.output.height ||
        c.render.width > c.output.width || c.render.height > c.output.height ||
        !temporal_inputs_valid(f.stage, f.depth.image != VK_NULL_HANDLE, f.motion.image != VK_NULL_HANDLE, f.engine_jitter_applied, f.exposure) ||
        !std::isfinite(f.pre_exposure) || f.pre_exposure <= 0 || !std::isfinite(f.delta_seconds) || f.delta_seconds <= 0 ||
        !std::isfinite(f.jitter.x) || !std::isfinite(f.jitter.y) || std::abs(f.jitter.x) > 0.5f || std::abs(f.jitter.y) > 0.5f) return false;
    if (!image_valid(f.color, c.render) || !image_valid(f.depth, c.render) || !image_valid(f.motion, c.render) || !image_valid(f.output, c.output)) return false;
    if (!color_format(f.color.format) || !color_format(f.output.format) ||
        (f.depth.format != VK_FORMAT_D32_SFLOAT && f.depth.format != VK_FORMAT_D16_UNORM) ||
        (f.motion.format != VK_FORMAT_R16G16_SFLOAT && f.motion.format != VK_FORMAT_R32G32_SFLOAT)) return false;
    if (!sampled_layout(f.color.layout) || !sampled_layout(f.depth.layout) || !sampled_layout(f.motion.layout) || f.output.layout != VK_IMAGE_LAYOUT_GENERAL) return false;
    if (f.output.image == f.color.image || f.output.image == f.depth.image || f.output.image == f.motion.image) return false;
    if (f.input_area.offset.x || f.input_area.offset.y || f.output_area.offset.x || f.output_area.offset.y ||
        f.input_area.extent.width != c.render.width || f.input_area.extent.height != c.render.height ||
        f.output_area.extent.width != c.output.width || f.output_area.extent.height != c.output.height) return false;
    if (c.preset == UpscalePreset::NativeAA && !(c.render == c.output)) return false;
    return !f.exposure_image.image || (image_valid(f.exposure_image, {1, 1}) && sampled_layout(f.exposure_image.layout) && f.exposure_image.format == VK_FORMAT_R32_SFLOAT);
}

struct InitStorage {
    std::wstring cache;
    std::vector<std::wstring> paths;
    std::vector<const wchar_t*> path_pointers;
    Common common{};
    Discovery discovery{};
    InitStorage(std::string dir, std::vector<std::string> search) : cache(std::filesystem::absolute(dir).wstring()) {
        for (const auto& p : search) paths.push_back(std::filesystem::absolute(p).wstring());
        for (const auto& p : paths) path_pointers.push_back(p.c_str());
        common = {{path_pointers.data(), static_cast<unsigned>(path_pointers.size())}, nullptr, {ngx_log, std::getenv("BBHOST_DLSS_LOG") ? 2 : 0, true}};
        discovery = {api_version, super_sampling, {1, {.project = {project_id, 0, "bbhost-integration-v1"}}}, cache.c_str(), &common};
    }
};
struct DlssProvider::Impl {
    std::shared_ptr<Core> core = std::make_shared<Core>();
    std::shared_ptr<InitStorage> storage;
    Retire retire;
    VkDevice device = VK_NULL_HANDLE;
    Parameter* caps = nullptr;
    Parameter* params = nullptr;
    Handle* feature = nullptr;
    bool tried = false, initialized = false, available = false, failed = false;
    std::string problem, runtime_version;
    struct Key { UpscaleExtent render, output; int quality, flags; bool operator==(const Key&) const = default; } key{};
    Impl(std::string dir, std::vector<std::string> search, Retire r)
        : storage(std::make_shared<InitStorage>(std::move(dir), std::move(search))), retire(std::move(r)) {}
    bool fail(const char* why, Result r = 0) {
        char text[512]; if (r) std::snprintf(text, sizeof(text), "%s (0x%08x)", why, r); else std::snprintf(text, sizeof(text), "%s", why);
        if (problem != text) std::fprintf(stderr, "DLSS: %s\n", text);
        problem = text; return false;
    }
    bool load() {
        if (tried) return core->module && core->shutdown;
        tried = true;
        return core->open() || fail("NVIDIA driver NGX Vulkan entry points are unavailable");
    }
    bool extensions(bool instance, VkInstance vk, VkPhysicalDevice pd, std::vector<std::string>& out) {
        if (!load()) return false;
        unsigned count = 0; VkExtensionProperties* props = nullptr;
        const auto r = instance ? (core->instance_ext ? core->instance_ext(&storage->discovery, &count, &props) : 0xbad00012u)
                                : (core->device_ext ? core->device_ext(vk, pd, &storage->discovery, &count, &props) : 0xbad00012u);
        // Some current driver cores export the feature-specific APIs as
        // NotImplemented stubs. The documented legacy query still returns the
        // complete required extension lists; never replace them with two
        // hardcoded NVX names or ignore an unrelated discovery failure.
        if ((r == 0xbad00012u || r == 0xbad0000cu) && core->legacy_extensions) {
            unsigned ni = 0, nd = 0; const char** ie = nullptr; const char** de = nullptr;
            const auto legacy = core->legacy_extensions(&ni, &ie, &nd, &de);
            if (legacy != success || ni > 128 || nd > 128 || (ni && !ie) || (nd && !de)) return fail("NGX legacy extension discovery failed", legacy);
            const auto n = instance ? ni : nd; const auto names = instance ? ie : de; std::vector<std::string> found;
            for (unsigned k = 0; k < n; ++k) {
                if (!names[k] || !std::memchr(names[k], 0, VK_MAX_EXTENSION_NAME_SIZE)) return fail("NGX returned an invalid legacy extension name");
                found.emplace_back(names[k]);
            }
            out = std::move(found); std::fprintf(stderr, "DLSS: using driver's complete legacy %s extension requirements\n", instance ? "instance" : "device"); return true;
        }
        if (r != success || count > 128 || (count && !props)) return fail("NGX extension discovery failed", r);
        std::vector<std::string> found;
        for (unsigned i = 0; i < count; ++i) {
            if (!std::memchr(props[i].extensionName, 0, VK_MAX_EXTENSION_NAME_SIZE)) return fail("NGX returned an invalid extension name");
            found.emplace_back(props[i].extensionName);
        }
        out = std::move(found); return true;
    }
    void retire_feature() {
        if (!feature && !params) return;
        auto api = core; auto h = std::exchange(feature, nullptr); auto p = std::exchange(params, nullptr);
        retire([api, h, p] {
            if (std::getenv("BBHOST_DLSS_LOG")) std::fprintf(stderr, "DLSS: retiring feature %p and parameters %p\n", static_cast<void*>(h), static_cast<void*>(p));
            if (h) api->release(h); if (p) api->destroy(p);
        });
    }
    ~Impl() {
        if (!retire) return;
        retire_feature();
        if (initialized) {
            auto api = core; auto p = caps; auto d = device;
            // NGX init arguments and search-path strings must outlive deferred
            // device shutdown, not only the frontend/provider object.
            retire([api, p, d, keep = storage] {
                (void)keep; // Own init data until this deferred callback finishes.
                if (std::getenv("BBHOST_DLSS_LOG")) std::fprintf(stderr, "DLSS: retiring capabilities %p\n", static_cast<void*>(p));
                if (p) api->destroy(p);
                if (std::getenv("BBHOST_DLSS_LOG")) std::fprintf(stderr, "DLSS: shutting down NGX device\n");
                const auto result = api->shutdown_device(d);
                if (result != success) std::fprintf(stderr, "DLSS: NGX device shutdown failed (0x%08x)\n", result);
                else if (std::getenv("BBHOST_DLSS_LOG")) std::fprintf(stderr, "DLSS: NGX device shutdown complete\n");
            });
        }
    }
};
DlssProvider::DlssProvider(std::string cache, std::vector<std::string> paths, Retire retire) : impl_(std::make_unique<Impl>(std::move(cache), std::move(paths), std::move(retire))) {}
DlssProvider::~DlssProvider() = default;
bool DlssProvider::instance_extensions(std::vector<std::string>& out) { return impl_->extensions(true, VK_NULL_HANDLE, VK_NULL_HANDLE, out); }
bool DlssProvider::device_extensions(VkInstance vk, VkPhysicalDevice pd, std::vector<std::string>& out) { return impl_->extensions(false, vk, pd, out); }
bool DlssProvider::initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
    auto& i = *impl_;
    if (i.initialized) return i.device == device && i.available;
    if (!instance || !physical || !device || !i.retire || !i.load()) return i.fail("NGX needs a Vulkan device and fence retirement callback");
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical, &props);
    if (props.vendorID != 0x10de) return i.fail("DLSS requires a supported NVIDIA device");
    Requirement support{};
    auto r = i.core->requirements(instance, physical, &i.storage->discovery, &support);
    if (r != success || support.support != 0) return i.fail("NGX reports this GPU/driver/model unsupported", r);
    std::error_code error; std::filesystem::create_directories(i.storage->cache, error);
    if (error) return i.fail("Cannot create the NGX cache directory");
    r = i.core->init(project_id, 0, "bbhost-integration-v1", i.storage->cache.c_str(), instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, api_version, &i.storage->common);
    if (r != success) return i.fail("NGX project initialization failed", r);
    i.device = device; i.initialized = true;
    r = i.core->capabilities(&i.caps);
    if (r != success || !i.caps) return i.fail("NGX capability allocation failed", r);
    if (!abi_ok(i.caps)) return i.fail("NGX parameter ABI does not match the driver");
    int available = 0, needs_driver = 0; gi(i.caps, "SuperSampling.NeedsUpdatedDriver", &needs_driver);
    if (gi(i.caps, "SuperSampling.Available", &available) != success || !available) {
        int code = 0; gi(i.caps, "SuperSampling.FeatureInitResult", &code);
        return i.fail(needs_driver ? "DLSS requires a newer NVIDIA driver" : "DLSS model is unavailable in the configured local search paths", static_cast<unsigned>(code));
    }
    i.available = true; i.problem.clear(); std::fprintf(stderr, "DLSS: NGX Vulkan initialized; parameter ABI verified on %s\n", props.deviceName); return true;
}
bool DlssProvider::available() const { return impl_->available && !impl_->failed; }
bool DlssProvider::optimal_settings(UpscaleExtent output, UpscalePreset preset, DlssOptimalSettings& out) {
    auto& i = *impl_;
    if (!available() || !output.width || !output.height || quality(preset) < 0) return false;
    void* callback = nullptr;
    if (gp(i.caps, "DLSSOptimalSettingsCallback", &callback) != success || !callback) return i.fail("DLSS optimal settings callback is unavailable");
    su(i.caps, "Width", output.width); su(i.caps, "Height", output.height); si(i.caps, "PerfQualityValue", quality(preset)); si(i.caps, "RTXValue", 0);
    const auto r = reinterpret_cast<Result(*)(Parameter*)>(callback)(i.caps);
    if (r != success) return i.fail("DLSS optimal settings query failed", r);
    DlssOptimalSettings found;
    if (gu(i.caps, "OutWidth", &found.render.width) != success || gu(i.caps, "OutHeight", &found.render.height) != success) return i.fail("DLSS optimal render dimensions are absent");
    found.minimum = found.maximum = found.render;
    gu(i.caps, "DLSS.Get.Dynamic.Min.Render.Width", &found.minimum.width); gu(i.caps, "DLSS.Get.Dynamic.Min.Render.Height", &found.minimum.height);
    gu(i.caps, "DLSS.Get.Dynamic.Max.Render.Width", &found.maximum.width); gu(i.caps, "DLSS.Get.Dynamic.Max.Render.Height", &found.maximum.height);
    if (!found.minimum.width || !found.minimum.height || found.minimum.width > found.render.width || found.minimum.height > found.render.height ||
        found.maximum.width < found.render.width || found.maximum.height < found.render.height || found.maximum.width > output.width || found.maximum.height > output.height)
        return i.fail("DLSS returned inconsistent render size limits");
    if (preset == UpscalePreset::NativeAA && !(found.render == output)) return i.fail("DLAA is unsupported by this model");
    out = found; return true;
}
const std::string& DlssProvider::problem() const { return impl_->problem; }
const std::string& DlssProvider::runtime_version() const { return impl_->runtime_version; }
bool DlssProvider::supports(const UpscaleConfig& c, const UpscaleFrame& f) const { return available() && dlss_frame_contract(c, f); }
bool DlssProvider::record(const UpscaleConfig& c, const UpscaleFrame& f) {
    if (!supports(c, f)) return false;
    auto& i = *impl_;
    const int flags = 2 | (hdr_format(f.color.format) ? 1 : 0) | (f.depth_inverted ? 8 : 0) | (!f.exposure_image.image ? 64 : 0);
    const Impl::Key key{c.render, c.output, quality(c.preset), flags};
    bool created = false;
    if (!i.feature || !(key == i.key)) {
        i.retire_feature();
        auto r = i.core->allocate(&i.params);
        if (r != success || !i.params) { i.failed = true; return i.fail("NGX feature parameters could not be allocated", r); }
        su(i.params, "CreationNodeMask", 1); su(i.params, "VisibilityNodeMask", 1);
        su(i.params, "Width", c.render.width); su(i.params, "Height", c.render.height); su(i.params, "OutWidth", c.output.width); su(i.params, "OutHeight", c.output.height);
        si(i.params, "PerfQualityValue", key.quality); si(i.params, "DLSS.Feature.Create.Flags", flags); si(i.params, "DLSS.Enable.Output.Subrects", 0);
        r = i.core->create(i.device, f.commands, super_sampling, i.params, &i.feature);
        if (r != success || !i.feature) { i.failed = true; i.retire_feature(); return i.fail("DLSS feature creation failed", r); }
        i.key = key; created = true;
        i.runtime_version = loaded_model_version();
        std::fprintf(stderr, "DLSS: loaded model file version=%s; requested preset=%d (NGX default model selection)\n",
                     i.runtime_version.empty() ? "unavailable" : i.runtime_version.c_str(), key.quality);
        std::fprintf(stderr, "DLSS: feature %ux%u -> %ux%u; quality=%d; flags=%d\n", c.render.width, c.render.height, c.output.width, c.output.height, key.quality, flags);
    }
    auto color = resource(f.color, false, false), depth = resource(f.depth, true, false), motion = resource(f.motion, false, false), output = resource(f.output, false, true);
    auto exposure = resource(f.exposure_image, false, false);
    sp(i.params, "Color", &color); sp(i.params, "Depth", &depth); sp(i.params, "MotionVectors", &motion); sp(i.params, "Output", &output);
    sp(i.params, "ExposureTexture", f.exposure_image.image ? &exposure : nullptr);
    sf(i.params, "Jitter.Offset.X", f.jitter.x); sf(i.params, "Jitter.Offset.Y", f.jitter.y);
    sf(i.params, "Sharpness", 0); // Current models do not implement legacy NGX sharpening; use a separate native pass.
    si(i.params, "Reset", created || f.reset != HistoryReset::None ? 1 : 0);
    sf(i.params, "MV.Scale.X", 1); sf(i.params, "MV.Scale.Y", 1); // canonical unjittered previous-current render pixels
    sf(i.params, "DLSS.Pre.Exposure", f.pre_exposure); sf(i.params, "DLSS.Exposure.Scale", f.exposure);
    su(i.params, "DLSS.Render.Subrect.Dimensions.Width", c.render.width); su(i.params, "DLSS.Render.Subrect.Dimensions.Height", c.render.height);
    sf(i.params, "FrameTimeDeltaInMsec", f.delta_seconds * 1000);
    const auto r = i.core->evaluate(f.commands, i.feature, i.params, nullptr);
    // Resource descriptors are CPU call arguments; NGX copies their contents
    // during Evaluate. Images themselves remain caller-owned through its fence.
    sp(i.params, "Color", nullptr); sp(i.params, "Depth", nullptr); sp(i.params, "MotionVectors", nullptr); sp(i.params, "Output", nullptr); sp(i.params, "ExposureTexture", nullptr);
    if (r != success) { i.failed = true; i.retire_feature(); return i.fail("DLSS evaluation failed; backend disabled for this session", r); }
    i.problem.clear(); return true;
}
}  // namespace gpu
