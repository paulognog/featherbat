#pragma once

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WM_APP_TRAY_CB (WM_APP + 10)

void alert_check(HWND ctrl_hwnd, HINSTANCE inst, Device* devs, int count);
void alert_cleanup(HWND ctrl_hwnd);
void alert_on_tray_msg(HWND ctrl_hwnd, LPARAM lp);

#ifdef __cplusplus
}
#endif
