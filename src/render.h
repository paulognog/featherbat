#pragma once

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Draws the widget onto layered HWND `hwnd` with per-pixel alpha.
// Returns width in physical pixels (0 if nothing to show).
int render_widget(HWND hwnd, const Device* devs, int count, UINT dpi, int height, bool light, bool hover);

#ifdef __cplusplus
}
#endif
