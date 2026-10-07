#pragma once

#include <string>

// Other programs' code loaded into bbhost: the overlays and capture tools that
// hook a game's graphics (OBS game capture, RivaTuner, the Steam and Discord
// overlays...), most of them through an implicit Vulkan layer that loads into
// every Vulkan program. Their names, "OBS game capture (graphics-hook64.dll)",
// comma-separated; "" when there are none. Said at the start and when the GPU
// is lost, which such a hook can cause.
std::string host_foreign_hooks();

// True when OBS's game capture is among them: the device-loss message names
// its own switch (DISABLE_VULKAN_OBS_CAPTURE=1).
bool host_foreign_hooks_obs();
