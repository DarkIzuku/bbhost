// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host/upscaler.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gpu {
struct DlssOptimalSettings {
    UpscaleExtent render, minimum, maximum;
};
// Runtime ABI binding adapted from bloodborne_pc/vk_dlss.cpp. No NVIDIA SDK
// headers, linked SDK libraries, model or driver binaries are distributed.
// The caller supplies its existing queue-fence retirement mechanism. A feature
// replacement, including its parameters, lives until the last recorded use
// retires. Backend code never submits a queue or waits for device idle.
class DlssProvider final : public UpscalerProvider {
public:
    using Retire = std::function<void(std::function<void()>)>;
    explicit DlssProvider(std::string cache, std::vector<std::string> model_paths, Retire retire);
    ~DlssProvider() override;
    DlssProvider(const DlssProvider&) = delete;
    DlssProvider& operator=(const DlssProvider&) = delete;
    bool instance_extensions(std::vector<std::string>& out);
    bool device_extensions(VkInstance, VkPhysicalDevice, std::vector<std::string>& out);
    bool initialize(VkInstance, VkPhysicalDevice, VkDevice);
    bool optimal_settings(UpscaleExtent output, UpscalePreset, DlssOptimalSettings& out);
    bool available() const;
    const std::string& problem() const;
    // File version of the model module actually loaded by NGX, after feature
    // creation. A DLL merely present in a search directory is not evidence.
    const std::string& runtime_version() const;
    UpscalerId id() const override { return UpscalerId::Dlss; }
    bool temporal() const override { return true; }
    bool supports(const UpscaleConfig&, const UpscaleFrame&) const override;
    bool record(const UpscaleConfig&, const UpscaleFrame&) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Pure validation, also exercised without an NVIDIA device. No HUD composite,
// synthetic motion substituted for missing engine data, aliasing, subrect
// mismatch, undefined layout or invalid temporal values may reach NGX.
bool dlss_frame_contract(const UpscaleConfig&, const UpscaleFrame&);
}  // namespace gpu
