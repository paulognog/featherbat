#pragma once

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WM_APP_UPDATE (WM_APP + 1)

// Starts background I/O thread and PnP notifications.
void io_start(HWND ctrl_hwnd);
// Stops background I/O thread cleanly.
void io_stop(void);
// Notifies I/O thread of PnP / power resume change (debounced).
void io_notify_pnp(void);
// Triggers an immediate full refresh on the I/O thread.
void io_refresh_now(void);
// Copies latest tracked devices into `out` (returns count <= max_count). Thread-safe.
int  io_snapshot(Device* out, int max_count);

// CLI helpers
int  io_run_once(void);
int  io_run_dump(void);

#ifdef __cplusplus
}
#endif
