#pragma once

// The host overlay: a 2D layer drawn over the frame the game presented, by the
// code that already owns presentation. It is how the PC port adds what the PS4
// build has no room for - the pointer, the text box that stands in for the
// system on-screen keyboard, and the options screen - without touching a menu
// asset or the dump.
//
// Coordinates are the game's display-buffer pixels, the same space
// host_mouse_state() reports its position in, so a hit test and a drawn
// highlight agree without either knowing the window size.

#include <vulkan/vulkan.h>

#include <cstdint>

// Colours are 0xRRGGBBAA.
void host_overlay_reset();
void host_overlay_rect(float x, float y, float w, float h, std::uint32_t rgba);
void host_overlay_tri(float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t rgba);
// A pointer with its tip at (x, y): a filled arrow with a dark outline, so it
// reads against both a bright menu and a dark one.
void host_overlay_cursor(float x, float y);
bool host_overlay_empty();

// Presenter side. `init` is safe to call repeatedly; `draw` records into a
// command buffer that is already in dynamic rendering's expected layout and
// leaves the frame beneath it untouched (it loads rather than clears).
// Draws `text` with its top-left at (x, y); `scale` multiplies the atlas cell,
// so 1.0 is 12x24 display pixels a character. Returns the width drawn.
float host_overlay_text(float x, float y, float scale, std::uint32_t rgba, const char* text);
float host_overlay_text_width(float scale, const char* text);

// `queue` and `family` are needed once, to upload the font atlas.
bool host_overlay_init(VkDevice device, VkPhysicalDevice phys, VkFormat colour_format, VkQueue queue,
                       std::uint32_t family);
void host_overlay_shutdown(VkDevice device);
// `area`: where the game's picture is in the image (the overlay's display
// coordinates map onto it, as the picture's pixels do).
bool host_overlay_draw(VkDevice device, VkCommandBuffer cmd, VkImage image, VkImageView view, VkExtent2D extent, VkRect2D area,
                       float display_w, float display_h);
