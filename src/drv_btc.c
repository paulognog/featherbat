#include "drivers.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>

#define MAX_BT_NODES 24

static const DEVPROPKEY kPropBtBattery = {
    { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2
};
static const DEVPROPKEY kPropIsConnected = {
    { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15
};
static const DEVPROPKEY kPropCod10 = {
    { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 10
};
static const DEVPROPKEY kPropCod4 = {
    { 0x2BD67D8B, 0x8BEB, 0x48D5, { 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A } }, 4
};
static const DEVPROPKEY kPropContainerId = {
    { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2
};
static const DEVPROPKEY kPropFriendlyName = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14
};
static const DEVPROPKEY kPropDeviceDesc = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 2
};
static const DEVPROPKEY kPropCategory = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 90
};

typedef struct {
    wchar_t cid[40];
    wchar_t main_name[96];
    wchar_t svc_name[96];
    bool has_main_conn;
    bool main_conn;
    bool has_cod;
    uint32_t cod;
    wchar_t category[48];
    bool has_bat;
    bool bat_conn;
    int bat_pct;
} BtContainer;

static void strip_service_suffix(wchar_t* s) {
    static const wchar_t* suffixes[] = {
        L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport", L" Stereo", L" Audio Gateway"
    };
    size_t sl = wlen(s);
    for (size_t i = 0; i < ARRAYSIZE(suffixes); i++) {
        size_t fl = wlen(suffixes[i]);
        if (sl > fl && weqi(s + sl - fl, suffixes[i])) {
            s[sl - fl] = 0;
            return;
        }
    }
}

static DeviceKind kind_from_cod(uint32_t cod, const wchar_t* name) {
    uint32_t major = (cod >> 8) & 0x1F;
    uint32_t minor = (cod >> 2) & 0x3F;
    if (major == 0x04) {
        return (kind_from_name(name) == KIND_EARBUDS) ? KIND_EARBUDS : KIND_HEADSET;
    }
    if (major == 0x05) {
        if ((minor & 0x0F) == 0x01 || (minor & 0x0F) == 0x02) return KIND_GAMEPAD;
        if ((minor >> 4) == 1) return KIND_KEYBOARD;
        if ((minor >> 4) == 2) return KIND_MOUSE;
        return KIND_GAMEPAD;
    }
    return KIND_OTHER;
}

static DeviceKind kind_from_cat(const wchar_t* c) {
    if (!c || !*c) return KIND_OTHER;
    if (weq(c, L"Input.Keyboard")) return KIND_KEYBOARD;
    if (weq(c, L"Input.Mouse")) return KIND_MOUSE;
    if (weq(c, L"Input.Gaming")) return KIND_GAMEPAD;
    if (wstarts(c, L"Communication.Headset") || weq(c, L"Audio.Headphones")) return KIND_HEADSET;
    return KIND_OTHER;
}

static int find_or_add_container(BtContainer* arr, int* count, const wchar_t* cid) {
    for (int i = 0; i < *count; i++) {
        if (weqi(arr[i].cid, cid)) return i;
    }
    if (*count >= MAX_BT_NODES) return -1;
    int idx = (*count)++;
    memset(&arr[idx], 0, sizeof(arr[idx]));
    wcpy(arr[idx].cid, 40, cid);
    arr[idx].bat_pct = -1;
    return idx;
}

void drv_btc_scan(ScanCtx* ctx) {
    HDEVINFO di = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;

    BtContainer* conts = (BtContainer*)xalloc(MAX_BT_NODES * sizeof(BtContainer));
    wchar_t* buf = (wchar_t*)xalloc(512 * sizeof(wchar_t));
    if (!conts || !buf) {
        xfree(conts); xfree(buf);
        SetupDiDestroyDeviceInfoList(di);
        return;
    }
    int nconts = 0;

    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof(dd);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, buf, 512, NULL)) continue;
        if (!wstartsi(buf, L"BTHENUM\\") && !wstartsi(buf, L"BTHLE\\") && !wstartsi(buf, L"BTHLEDEVICE\\")) continue;

        DEVPROPTYPE t = 0;
        GUID g;
        wchar_t cid[40];
        if (SetupDiGetDevicePropertyW(di, &dd, &kPropContainerId, &t, (PBYTE)&g, sizeof(g), NULL, 0) && t == DEVPROP_TYPE_GUID) {
            guid_to_wstr(&g, cid, 40);
        } else {
            wfmt(cid, 40, L"%08X", fnv1a_w(buf));
        }

        int idx = find_or_add_container(conts, &nconts, cid);
        if (idx < 0) continue;
        BtContainer* bc = &conts[idx];

        wchar_t name[96];
        name[0] = 0;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kPropFriendlyName, &t, (PBYTE)name, sizeof(name) - 2, NULL, 0) || t != DEVPROP_TYPE_STRING || !name[0]) {
            if (!SetupDiGetDevicePropertyW(di, &dd, &kPropDeviceDesc, &t, (PBYTE)name, sizeof(name) - 2, NULL, 0) || t != DEVPROP_TYPE_STRING) {
                name[0] = 0;
            }
        }

        if (wstartsi(buf, L"BTHENUM\\DEV_") || wstartsi(buf, L"BTHLE\\DEV_")) {
            if (name[0]) wcpy(bc->main_name, 96, name);
            uint32_t cod = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kPropCod10, &t, (PBYTE)&cod, sizeof(cod), NULL, 0) && t == DEVPROP_TYPE_UINT32) {
                bc->has_cod = true; bc->cod = cod;
            } else if (SetupDiGetDevicePropertyW(di, &dd, &kPropCod4, &t, (PBYTE)&cod, sizeof(cod), NULL, 0) && t == DEVPROP_TYPE_UINT32) {
                bc->has_cod = true; bc->cod = cod;
            }
            wchar_t cat[64];
            memset(cat, 0, sizeof(cat));
            if (SetupDiGetDevicePropertyW(di, &dd, &kPropCategory, &t, (PBYTE)cat, sizeof(cat) - 2, NULL, 0) &&
                (t == DEVPROP_TYPE_STRING_LIST || t == DEVPROP_TYPE_STRING)) {
                wcpy(bc->category, 48, cat);
            }
            uint8_t conn = 0;
            if (SetupDiGetDevicePropertyW(di, &dd, &kPropIsConnected, &t, &conn, sizeof(conn), NULL, 0) && t == DEVPROP_TYPE_BOOLEAN) {
                bc->has_main_conn = true;
                bc->main_conn = (conn != 0);
            }
        }

        uint8_t pct = 0;
        if (!SetupDiGetDevicePropertyW(di, &dd, &kPropBtBattery, &t, &pct, sizeof(pct), NULL, 0) || t != DEVPROP_TYPE_BYTE) continue;
        if (pct > 100) continue;

        ULONG st = 0, prob = 0;
        if (CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0) == CR_SUCCESS && (st & DN_HAS_PROBLEM)) continue;

        uint8_t svc_conn_b = 1;
        bool svc_conn = true;
        if (SetupDiGetDevicePropertyW(di, &dd, &kPropIsConnected, &t, &svc_conn_b, sizeof(svc_conn_b), NULL, 0) && t == DEVPROP_TYPE_BOOLEAN) {
            svc_conn = (svc_conn_b != 0);
        }
        if (bc->has_bat && bc->bat_conn && !svc_conn) continue;

        bc->has_bat = true;
        bc->bat_conn = svc_conn;
        bc->bat_pct = (int)pct;
        if (name[0]) {
            wcpy(bc->svc_name, 96, name);
            strip_service_suffix(bc->svc_name);
        }
    }
    xfree(buf);
    SetupDiDestroyDeviceInfoList(di);

    // Build map of HID containers already claimed by vendor drivers (e.g. Logitech over BLE)
    for (int i = 0; i < nconts; i++) {
        const BtContainer* bc = &conts[i];
        if (!bc->has_bat) continue;
        bool connected = bc->has_main_conn ? bc->main_conn : bc->bat_conn;
        if (!connected) continue;
        if (scan_ctx_has_bt_container(ctx, bc->cid)) continue;

        // Skip if a vendor driver already reported this container or exact name
        bool dup = false;
        const wchar_t* dname = bc->main_name[0] ? bc->main_name : (bc->svc_name[0] ? bc->svc_name : L"Bluetooth device");
        for (int j = 0; j < ctx->count; j++) {
            if ((ctx->obs[j].container_id[0] && weqi(ctx->obs[j].container_id, bc->cid)) ||
                weq(ctx->obs[j].name, dname)) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        DeviceKind kind = bc->has_cod ? kind_from_cod(bc->cod, dname) : KIND_OTHER;
        if (kind == KIND_OTHER && bc->category[0]) kind = kind_from_cat(bc->category);

        // If still KIND_OTHER, check HID collections sharing this container
        if (kind == KIND_OTHER && ctx->hids) {
            for (int h = 0; h < ctx->hids->count; h++) {
                wchar_t hcid[40];
                if (hid_container_id(ctx->hids->items[h].path, hcid, 40) && weqi(hcid, bc->cid)) {
                    // Attack Shark K86 over Bluetooth (3151:4013) exposes a mouse collection too
                    if (ctx->hids->items[h].vid == 0x3151 && ctx->hids->items[h].pid == 0x4013) {
                        kind = KIND_KEYBOARD;
                    } else {
                        kind = kind_from_hid(ctx->hids, ctx->hids->items[h].vid, ctx->hids->items[h].pid);
                    }
                    if (kind != KIND_OTHER) break;
                }
            }
        }
        if (kind == KIND_OTHER) kind = kind_from_name(dname);

        Obs o;
        memset(&o, 0, sizeof(o));
        wfmt(o.id, 64, L"bt:%08X", fnv1a_w(bc->cid));
        wcpy(o.name, 96, dname);
        wcpy(o.via, 32, L"Bluetooth");
        wcpy(o.container_id, 40, bc->cid);
        o.kind = kind;
        o.has_kind = true;
        o.state = STATE_BATTERY;
        o.percent = bc->bat_pct;
        o.is_bt = true;
        scan_ctx_add(ctx, &o);
        scan_ctx_add_bt_container(ctx, bc->cid);
    }

    xfree(conts);
}
