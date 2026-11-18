#pragma once

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Push/Persistent Drivers ----

// Logitech HID++ + G435 (046D:0ADF)
void drv_logi_sync(const HidList* hids, bool once_mode);
int  drv_logi_wait_handles(HANDLE* out, int max_count);
bool drv_logi_on_io_ready(void);   // returns true if any reading changed
bool drv_logi_refresh_awake(void); // slow refresh for non-broadcasting awake devices
void drv_logi_emit(ScanCtx* ctx);
void drv_logi_close_all(void);

// Bluetooth LE GATT Battery Service (0x180F / 0x2A19) via BluetoothApis.dll
void drv_ble_sync(HANDLE wake_evt);
void drv_ble_emit(ScanCtx* ctx);
void drv_ble_close_all(void);

// Xenta 2.4 GHz receivers (1D57:FA60/FA65/FA55/2155 - Attack Shark, Delux)
void drv_xenta_sync(const HidList* hids, bool once_mode);
int  drv_xenta_wait_handles(HANDLE* out, int max_count);
bool drv_xenta_on_io_ready(void);
bool drv_xenta_drain_all(void);
void drv_xenta_emit(ScanCtx* ctx);
void drv_xenta_close_all(void);
void drv_xenta_dump(const HidList* hids, WBuf* out);

// ---- Pull / On-Demand Drivers ----
void drv_rongyuan_scan(ScanCtx* ctx);
void drv_kysona_scan(ScanCtx* ctx);
void drv_hyperx_scan(ScanCtx* ctx);
void drv_ps_scan(ScanCtx* ctx);
void drv_btc_scan(ScanCtx* ctx);
void drv_genhid_scan(ScanCtx* ctx);
void drv_xbox_scan(ScanCtx* ctx);
void drv_hsc_scan(ScanCtx* ctx);

// Presence checks (so io.c skips pull timers when hardware isn't attached)
bool drv_rongyuan_present(const HidList* hids);
bool drv_kysona_present(const HidList* hids);
bool drv_hyperx_present(const HidList* hids);
bool drv_ps_present(const HidList* hids);
bool drv_xbox_present(const HidList* hids);
bool drv_genhid_present(const HidList* hids);
bool drv_hsc_present(void);
bool drv_logi_g435_present(const HidList* hids);

// Pure testable parsing helpers (also called by unit tests)
int  logi_liion_percent(int mv);
int  logi_headset_percent(uint16_t pid, int mv);
void ps_parse_ds4(uint8_t status, int* pct, bool* charging);
void ps_parse_dualsense(uint8_t status, int* pct, bool* charging);
uint8_t kysona_checksum(const uint8_t* b16);

#ifdef __cplusplus
}
#endif
