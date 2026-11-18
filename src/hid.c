#include "device.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <devpkey.h>

// DEVPKEY_Device_ContainerId = {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
static const DEVPROPKEY kPropContainerId = {
    { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } },
    2
};

void scan_ctx_init(ScanCtx* ctx, const HidList* hids) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cap = 16;
    ctx->obs = (Obs*)xalloc((size_t)ctx->cap * sizeof(Obs));
    ctx->hids = hids;
}

void scan_ctx_free(ScanCtx* ctx) {
    xfree(ctx->obs);
    memset(ctx, 0, sizeof(*ctx));
}

void scan_ctx_claim(ScanCtx* ctx, uint16_t vid, uint16_t pid) {
    uint32_t k = ((uint32_t)vid << 16) | pid;
    if (scan_ctx_is_claimed(ctx, vid, pid)) return;
    if (ctx->claimed_count < 64) ctx->claimed[ctx->claimed_count++] = k;
}

bool scan_ctx_is_claimed(const ScanCtx* ctx, uint16_t vid, uint16_t pid) {
    uint32_t k = ((uint32_t)vid << 16) | pid;
    for (int i = 0; i < ctx->claimed_count; i++) {
        if (ctx->claimed[i] == k) return true;
    }
    return false;
}

void scan_ctx_add(ScanCtx* ctx, const Obs* o) {
    if (ctx->count >= ctx->cap) {
        int ncap = ctx->cap ? ctx->cap * 2 : 16;
        Obs* n = (Obs*)xrealloc(ctx->obs, (size_t)ncap * sizeof(Obs));
        if (!n) return;
        ctx->obs = n;
        ctx->cap = ncap;
    }
    ctx->obs[ctx->count++] = *o;
}

bool scan_ctx_has_bt_container(const ScanCtx* ctx, const wchar_t* cid) {
    if (!cid || !*cid) return false;
    for (int i = 0; i < ctx->bt_container_count; i++) {
        if (weqi(ctx->bt_containers[i], cid)) return true;
    }
    return false;
}

void scan_ctx_add_bt_container(ScanCtx* ctx, const wchar_t* cid) {
    if (!cid || !*cid || scan_ctx_has_bt_container(ctx, cid)) return;
    if (ctx->bt_container_count < 16) {
        wcpy(ctx->bt_containers[ctx->bt_container_count++], 40, cid);
    }
}

static int parse_mi_iface(const wchar_t* path) {
    const wchar_t* p = wfind_i(path, L"&mi_");
    if (!p) return -1;
    p += 4;
    int val = 0;
    for (int i = 0; i < 2 && p[i]; i++) {
        wchar_t c = p[i];
        int d = -1;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
        else break;
        val = (val << 4) | d;
    }
    return val;
}

void hid_enum(HidList* list) {
    memset(list, 0, sizeof(*list));
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO di = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    uint8_t* detail_buf = (uint8_t*)xalloc(1024);
    if (!detail_buf) {
        SetupDiDestroyDeviceInfoList(di);
        return;
    }

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &hid_guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        if (!need || need > 1024) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detail_buf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, NULL)) continue;

        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        HidColl c;
        memset(&c, 0, sizeof(c));
        wcpy(c.path, 260, det->DevicePath);
        c.iface = parse_mi_iface(c.path);

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            c.vid = attr.VendorID;
            c.pid = attr.ProductID;
        }
        PHIDP_PREPARSED_DATA pp = NULL;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                c.usage_page = caps.UsagePage;
                c.usage = caps.Usage;
                c.in_len = caps.InputReportByteLength;
                c.out_len = caps.OutputReportByteLength;
                c.feat_len = caps.FeatureReportByteLength;
                c.num_feat_val_caps = caps.NumberFeatureValueCaps;
                c.num_in_val_caps = caps.NumberInputValueCaps;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);

        if (list->count >= list->cap) {
            int ncap = list->cap ? list->cap * 2 : 32;
            HidColl* n = (HidColl*)xrealloc(list->items, (size_t)ncap * sizeof(HidColl));
            if (!n) break;
            list->items = n;
            list->cap = ncap;
        }
        list->items[list->count++] = c;
    }
    xfree(detail_buf);
    SetupDiDestroyDeviceInfoList(di);
}

void hid_list_free(HidList* list) {
    xfree(list->items);
    memset(list, 0, sizeof(*list));
}

bool hid_open(HidDev* d, const HidColl* c, DWORD access, bool overlapped) {
    memset(d, 0, sizeof(*d));
    d->h = INVALID_HANDLE_VALUE;
    if (!c) return false;
    DWORD flags = overlapped ? FILE_FLAG_OVERLAPPED : 0;
    d->h = CreateFileW(c->path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, flags, NULL);
    if (d->h == INVALID_HANDLE_VALUE) return false;
    if (overlapped) {
        d->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!d->ov.hEvent) {
            CloseHandle(d->h);
            d->h = INVALID_HANDLE_VALUE;
            return false;
        }
    }
    d->in_len = c->in_len;
    d->out_len = c->out_len;
    d->feat_len = c->feat_len;
    d->overlapped = overlapped;
    d->alive = true;
    return true;
}

void hid_close(HidDev* d) {
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE && d->h != NULL) {
        if (d->read_pending && d->overlapped) {
            CancelIoEx(d->h, &d->ov);
            DWORD dummy = 0;
            GetOverlappedResult(d->h, &d->ov, &dummy, TRUE);
        }
        CloseHandle(d->h);
    }
    if (d->ov.hEvent) CloseHandle(d->ov.hEvent);
    memset(d, 0, sizeof(*d));
    d->h = INVALID_HANDLE_VALUE;
}

bool hid_write(HidDev* d, const uint8_t* data, int len, int timeout_ms) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->overlapped || !d->out_len || !data || len <= 0) return false;
    int buf_len = d->out_len;
    uint8_t* buf = (uint8_t*)xalloc((size_t)buf_len);
    if (!buf) return false;
    int copy_n = len < buf_len ? len : buf_len;
    memcpy(buf, data, (size_t)copy_n);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) { xfree(buf); return false; }

    DWORD written = 0;
    bool done = false;
    if (WriteFile(d->h, buf, (DWORD)buf_len, &written, &ov)) {
        done = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, (DWORD)timeout_ms) == WAIT_OBJECT_0) {
            done = GetOverlappedResult(d->h, &ov, &written, FALSE) != FALSE;
        } else {
            CancelIoEx(d->h, &ov);
            done = GetOverlappedResult(d->h, &ov, &written, TRUE) != FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    xfree(buf);
    return done;
}

int hid_read_sync(HidDev* d, uint8_t* out, int cap, int timeout_ms) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->overlapped || !d->in_len || d->read_pending) return -1;
    int rlen = d->in_len < (int)sizeof(d->rx_buf) ? d->in_len : (int)sizeof(d->rx_buf);
    memset(d->rx_buf, 0, (size_t)rlen);
    ResetEvent(d->ov.hEvent);
    d->ov.Offset = 0;
    d->ov.OffsetHigh = 0;

    DWORD got = 0;
    bool done = false;
    if (ReadFile(d->h, d->rx_buf, (DWORD)rlen, &got, &d->ov)) {
        done = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (timeout_ms > 0 && WaitForSingleObject(d->ov.hEvent, (DWORD)timeout_ms) == WAIT_OBJECT_0) {
            done = GetOverlappedResult(d->h, &d->ov, &got, FALSE) != FALSE;
        } else {
            CancelIoEx(d->h, &d->ov);
            done = GetOverlappedResult(d->h, &d->ov, &got, TRUE) != FALSE;
        }
    }
    if (!done || got == 0) return -1;
    size_t off = (d->rx_buf[0] == 0) ? 1 : 0;
    int payload = (int)got - (int)off;
    if (payload <= 0) return 0;
    int copy_n = payload < cap ? payload : cap;
    memcpy(out, d->rx_buf + off, (size_t)copy_n);
    return copy_n;
}

bool hid_start_async_read(HidDev* d) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->overlapped || !d->in_len || d->read_pending) return false;
    int rlen = d->in_len < (int)sizeof(d->rx_buf) ? d->in_len : (int)sizeof(d->rx_buf);
    memset(d->rx_buf, 0, (size_t)rlen);
    ResetEvent(d->ov.hEvent);
    d->ov.Offset = 0;
    d->ov.OffsetHigh = 0;
    DWORD got = 0;
    if (ReadFile(d->h, d->rx_buf, (DWORD)rlen, &got, &d->ov)) {
        // Completed synchronously; signal event so caller processes via hid_poll_async_read uniformly
        SetEvent(d->ov.hEvent);
        d->read_pending = true;
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        d->read_pending = true;
        return true;
    }
    d->alive = false;
    return false;
}

int hid_poll_async_read(HidDev* d, uint8_t* out, int cap, bool* io_error) {
    if (io_error) *io_error = false;
    if (!d || !d->read_pending) return 0;
    DWORD got = 0;
    if (!GetOverlappedResult(d->h, &d->ov, &got, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) return 0;
        d->read_pending = false;
        d->alive = false;
        if (io_error) *io_error = true;
        return -1;
    }
    d->read_pending = false;
    if (got == 0) return 0;
    size_t off = (d->rx_buf[0] == 0) ? 1 : 0;
    int payload = (int)got - (int)off;
    if (payload <= 0) return 0;
    int copy_n = payload < cap ? payload : cap;
    memcpy(out, d->rx_buf + off, (size_t)copy_n);
    return copy_n;
}

void hid_flush(HidDev* d) {
    if (d && d->h != INVALID_HANDLE_VALUE) HidD_FlushQueue(d->h);
}

bool hid_set_feature(HidDev* d, const uint8_t* data, int len) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->feat_len || !data || len <= 0) return false;
    uint8_t* buf = (uint8_t*)xalloc(d->feat_len);
    if (!buf) return false;
    int copy_n = len < d->feat_len ? len : d->feat_len;
    memcpy(buf, data, (size_t)copy_n);
    BOOLEAN ok = HidD_SetFeature(d->h, buf, d->feat_len);
    xfree(buf);
    return ok != FALSE;
}

bool hid_get_feature(HidDev* d, uint8_t report_id, uint8_t* out, int cap, int* out_len) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->feat_len || !out || cap <= 0) return false;
    uint8_t* buf = (uint8_t*)xalloc(d->feat_len);
    if (!buf) return false;
    buf[0] = report_id;
    BOOLEAN ok = HidD_GetFeature(d->h, buf, d->feat_len);
    if (ok) {
        int copy_n = d->feat_len < cap ? d->feat_len : cap;
        memcpy(out, buf, (size_t)copy_n);
        if (out_len) *out_len = copy_n;
    }
    xfree(buf);
    return ok != FALSE;
}

bool hid_get_input_report(HidDev* d, uint8_t report_id, uint8_t* out, int cap, int* out_len) {
    if (!d || d->h == INVALID_HANDLE_VALUE || !d->in_len || !out || cap <= 0) return false;
    uint8_t* buf = (uint8_t*)xalloc(d->in_len);
    if (!buf) return false;
    buf[0] = report_id;
    BOOLEAN ok = HidD_GetInputReport(d->h, buf, d->in_len);
    if (ok) {
        int copy_n = d->in_len < cap ? d->in_len : cap;
        memcpy(out, buf, (size_t)copy_n);
        if (out_len) *out_len = copy_n;
    }
    xfree(buf);
    return ok != FALSE;
}

void hid_product_name(const wchar_t* path, wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!path || !*path) return;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t tmp[128];
    memset(tmp, 0, sizeof(tmp));
    if (HidD_GetProductString(h, tmp, sizeof(tmp) - sizeof(wchar_t))) {
        wtrim_end(tmp);
        wcpy(out, cap, tmp);
    }
    CloseHandle(h);
}

bool hid_container_id(const wchar_t* path, wchar_t* out, size_t cap) {
    if (!out || cap < 39) return false;
    out[0] = 0;
    HDEVINFO di = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (di == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof(dd);
    if (SetupDiOpenDeviceInterfaceW(di, path, 0, &ifd)) {
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, NULL, &dd);
        if (dd.DevInst) {
            GUID g;
            DEVPROPTYPE t = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kPropContainerId, &t, (PBYTE)&g, sizeof(g), NULL, 0) &&
                t == DEVPROP_TYPE_GUID) {
                guid_to_wstr(&g, out, cap);
                ok = true;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return ok;
}

DeviceKind kind_from_name(const wchar_t* name) {
    if (!name || !*name) return KIND_OTHER;
    if (wcontains_i(name, L"airpods max") || wcontains_i(name, L"headset") ||
        wcontains_i(name, L"headphone") || wcontains_i(name, L"arctis") ||
        wcontains_i(name, L"cloud") || wcontains_i(name, L"wh-") ||
        wcontains_i(name, L"virtuoso") || wcontains_i(name, L"void")) {
        return KIND_HEADSET;
    }
    if (wcontains_i(name, L"earbud") || wcontains_i(name, L"buds") ||
        wcontains_i(name, L"airpods") || wcontains_i(name, L"earphone") ||
        wcontains_i(name, L"in-ear") || wcontains_i(name, L"wf-")) {
        return KIND_EARBUDS;
    }
    if (wcontains_i(name, L"mouse") || wcontains_i(name, L"mice") || wcontains_i(name, L"trackball")) {
        return KIND_MOUSE;
    }
    if (wcontains_i(name, L"keyboard") || wcontains_i(name, L"keys")) {
        return KIND_KEYBOARD;
    }
    if (wcontains_i(name, L"controller") || wcontains_i(name, L"gamepad") ||
        wcontains_i(name, L"xbox") || wcontains_i(name, L"dualsense") ||
        wcontains_i(name, L"dualshock") || wcontains_i(name, L"joy-con")) {
        return KIND_GAMEPAD;
    }
    return KIND_OTHER;
}

DeviceKind kind_from_hid(const HidList* list, uint16_t vid, uint16_t pid) {
    if (!list) return KIND_OTHER;
    bool mouse = false, pad = false, kbd = false;
    for (int i = 0; i < list->count; i++) {
        const HidColl* c = &list->items[i];
        if (c->vid != vid || c->pid != pid || c->usage_page != 0x01) continue;
        if (c->usage == 0x02) mouse = true;
        if (c->usage == 0x04 || c->usage == 0x05) pad = true;
        if (c->usage == 0x06) kbd = true;
    }
    if (mouse) return KIND_MOUSE;
    if (pad) return KIND_GAMEPAD;
    if (kbd) return KIND_KEYBOARD;
    return KIND_OTHER;
}
