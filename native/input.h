#pragma once
#include <cstdint>
namespace ot2 {
// Host pointer lifecycle. Coordinates are pixels in the Android content view;
// the latest presented image aspect determines the letterboxed game viewport.
enum class TouchAction { down, move, up };
void touch_display_size(uint32_t width, uint32_t height);
void touch_event(TouchAction action, int id, float x, float y, int width, int height);
void touch_cancel();
}
