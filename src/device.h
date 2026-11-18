#pragma once

#include "mini.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DEVICES 24
#define LOW_BATTERY_PCT 15
#define REARM_BATTERY_PCT 20

typedef enum {
    KIND_HEADSET = 0,
    KIND_EARBUDS,
    KIND_MOUSE,
    KIND_KEYBOARD,
    KIND_GAMEPAD,
    KIND_OTHER
} DeviceKind;

typedef enum {
    STATE_OFF = 0,
    STATE_BATTERY,
    STATE_ASLEEP
} DeviceState;

typedef struct {
    wchar_t id[64];
    wchar_t name[96];
    wchar_t via[32];
    wchar_t level_text[16];
    wchar_t container_id[40]; // optional "{GUID}" for dedup across BT/HID
    DeviceKind kind;
    DeviceState state;
    int percent;      // 0..100, or -1 if unknown
    int millivolts;   // 0 if N/A
    bool charging;
    bool is_bt;
    bool low_alerted;
    uint64_t last_seen_ms;
    uint32_t grace_ms;
    uint16_t vid, pid;
} Device;

typedef struct {
    wchar_t path[260];
    uint16_t vid;
    uint16_t pid;
    int iface;
    uint16_t usage_page;
    uint16_t usage;
    uint16_t in_len;
    uint16_t out_len;
    uint16_t feat_len;
    uint16_t num_feat_val_caps;
    uint16_t num_in_val_caps;
} HidColl;

typedef struct {
    HidColl* items;
    int count;
    int cap;
} HidList;

typedef struct {
    HANDLE h;
    OVERLAPPED ov;
    uint8_t rx_buf[128];
    uint16_t in_len;
    uint16_t out_len;
    uint16_t feat_len;
    bool read_pending;
    bool overlapped;
    bool alive;
} HidDev;

// Observation list passed during scans / tracker merges
typedef struct {
    wchar_t id[64];
    wchar_t name[96];
    wchar_t via[32];
    wchar_t level_text[16];
    wchar_t container_id[40];
    DeviceKind kind;
    bool has_kind;
    DeviceState state;
    int percent;
    int millivolts;
    bool charging;
    bool is_bt;
    uint32_t grace_ms;
    uint16_t vid, pid;
} Obs;

typedef struct {
    Obs* obs;
    int count;
    int cap;
    uint32_t claimed[64]; // (vid << 16) | pid
    int claimed_count;
    wchar_t bt_containers[16][40];
    int bt_container_count;
    const HidList* hids;
} ScanCtx;

void scan_ctx_init(ScanCtx* ctx, const HidList* hids);
void scan_ctx_free(ScanCtx* ctx);
void scan_ctx_claim(ScanCtx* ctx, uint16_t vid, uint16_t pid);
bool scan_ctx_is_claimed(const ScanCtx* ctx, uint16_t vid, uint16_t pid);
void scan_ctx_add(ScanCtx* ctx, const Obs* o);
bool scan_ctx_has_bt_container(const ScanCtx* ctx, const wchar_t* cid);
void scan_ctx_add_bt_container(ScanCtx* ctx, const wchar_t* cid);

// HID enumeration & I/O (hid.c)
void hid_enum(HidList* list);
void hid_list_free(HidList* list);
bool hid_open(HidDev* d, const HidColl* c, DWORD access, bool overlapped);
void hid_close(HidDev* d);
bool hid_write(HidDev* d, const uint8_t* data, int len, int timeout_ms);
int  hid_read_sync(HidDev* d, uint8_t* out, int cap, int timeout_ms);
bool hid_start_async_read(HidDev* d);
int  hid_poll_async_read(HidDev* d, uint8_t* out, int cap, bool* io_error);
void hid_flush(HidDev* d);
bool hid_set_feature(HidDev* d, const uint8_t* data, int len);
bool hid_get_feature(HidDev* d, uint8_t report_id, uint8_t* out, int cap, int* out_len);
bool hid_get_input_report(HidDev* d, uint8_t report_id, uint8_t* out, int cap, int* out_len);
void hid_product_name(const wchar_t* path, wchar_t* out, size_t cap);
bool hid_container_id(const wchar_t* path, wchar_t* out, size_t cap);

// Kind classification helpers (hid.c)
DeviceKind kind_from_name(const wchar_t* name);
DeviceKind kind_from_hid(const HidList* list, uint16_t vid, uint16_t pid);

#ifdef __cplusplus
}
#endif
