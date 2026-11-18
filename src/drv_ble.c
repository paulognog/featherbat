#include "drivers.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <bluetoothleapis.h>

#define MAX_BLE_DEVS 12

static const GUID kGuidGattSvc = {
    0x6e3bb679, 0x4372, 0x40c8, { 0x9e, 0xaa, 0x45, 0x09, 0xdf, 0x26, 0x0c, 0xd8 }
};
static const DEVPROPKEY kPropIsConnected = {
    { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15
};
static const DEVPROPKEY kPropFriendlyName = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14
};
static const DEVPROPKEY kPropDeviceDesc = {
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 2
};
static const DEVPROPKEY kPropContainerId = {
    { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2
};
static const DEVPROPKEY kPropCategory = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 90
};

typedef HRESULT (WINAPI *FnGattGetChars)(HANDLE, PBTH_LE_GATT_SERVICE, USHORT, PBTH_LE_GATT_CHARACTERISTIC, USHORT*, ULONG);
typedef HRESULT (WINAPI *FnGattGetDescs)(HANDLE, PBTH_LE_GATT_CHARACTERISTIC, USHORT, PBTH_LE_GATT_DESCRIPTOR, USHORT*, ULONG);
typedef HRESULT (WINAPI *FnGattGetCharVal)(HANDLE, PBTH_LE_GATT_CHARACTERISTIC, ULONG, PBTH_LE_GATT_CHARACTERISTIC_VALUE, USHORT*, ULONG);
typedef HRESULT (WINAPI *FnGattGetDescVal)(HANDLE, PBTH_LE_GATT_DESCRIPTOR, ULONG, PBTH_LE_GATT_DESCRIPTOR_VALUE, USHORT*, ULONG);
typedef HRESULT (WINAPI *FnGattSetDescVal)(HANDLE, PBTH_LE_GATT_DESCRIPTOR, PBTH_LE_GATT_DESCRIPTOR_VALUE, ULONG);
typedef HRESULT (WINAPI *FnGattRegEvent)(HANDLE, BTH_LE_GATT_EVENT_TYPE, PVOID, PFNBLUETOOTH_GATT_EVENT_CALLBACK, PVOID, BLUETOOTH_GATT_EVENT_HANDLE*, ULONG);
typedef HRESULT (WINAPI *FnGattUnregEvent)(BLUETOOTH_GATT_EVENT_HANDLE, ULONG);

typedef struct {
    bool loaded;
    HMODULE mod;
    FnGattGetChars   get_chars;
    FnGattGetDescs   get_descs;
    FnGattGetCharVal get_char_val;
    FnGattGetDescVal get_desc_val;
    FnGattSetDescVal set_desc_val;
    FnGattRegEvent   reg_event;
    FnGattUnregEvent unreg_event;
} BtApi;

static BtApi g_bt;

static bool load_bt_api(void) {
    if (g_bt.loaded) return g_bt.mod != NULL;
    g_bt.loaded = true;
    HMODULE m = LoadLibraryExW(L"BluetoothApis.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
    g_bt.mod = m;
    g_bt.get_chars    = (FnGattGetChars)(void*)GetProcAddress(m, "BluetoothGATTGetCharacteristics");
    g_bt.get_descs    = (FnGattGetDescs)(void*)GetProcAddress(m, "BluetoothGATTGetDescriptors");
    g_bt.get_char_val = (FnGattGetCharVal)(void*)GetProcAddress(m, "BluetoothGATTGetCharacteristicValue");
    g_bt.get_desc_val = (FnGattGetDescVal)(void*)GetProcAddress(m, "BluetoothGATTGetDescriptorValue");
    g_bt.set_desc_val = (FnGattSetDescVal)(void*)GetProcAddress(m, "BluetoothGATTSetDescriptorValue");
    g_bt.reg_event    = (FnGattRegEvent)(void*)GetProcAddress(m, "BluetoothGATTRegisterEvent");
    g_bt.unreg_event  = (FnGattUnregEvent)(void*)GetProcAddress(m, "BluetoothGATTUnregisterEvent");
    if (!g_bt.get_chars || !g_bt.get_char_val) {
        FreeLibrary(m);
        g_bt.mod = NULL;
        return false;
    }
    return true;
}

typedef struct {
    bool active;
    wchar_t path[260];
    wchar_t id[64];
    wchar_t name[96];
    wchar_t container_id[40];
    DeviceKind kind;
    HANDLE h_svc;
    BLUETOOTH_GATT_EVENT_HANDLE h_ev;
    volatile LONG percent;
    uint32_t gen;
} BleEntry;

static BleEntry g_ble[MAX_BLE_DEVS];
static SRWLOCK  g_ble_lock = SRWLOCK_INIT;
static HANDLE   g_wake_evt = NULL;
static uint32_t g_ble_gen = 1;

static bool is_uuid16(const BTH_LE_UUID* u, uint16_t short_id) {
    if (u->IsShortUuid) return u->Value.ShortUuid == short_id;
    GUID g = { 0x00000000, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
    g.Data1 += short_id;
    return memcmp(&u->Value.LongUuid, &g, sizeof(GUID)) == 0;
}

static void CALLBACK gatt_value_cb(BTH_LE_GATT_EVENT_TYPE type, PVOID param, PVOID ctx) {
    if (type != CharacteristicValueChangedEvent || !param || !ctx) return;
    PBLUETOOTH_GATT_VALUE_CHANGED_EVENT ev = (PBLUETOOTH_GATT_VALUE_CHANGED_EVENT)param;
    if (!ev->CharacteristicValue || ev->CharacteristicValue->DataSize < 1) return;
    int pct = (int)ev->CharacteristicValue->Data[0];
    if (pct > 100) pct = 100;

    BleEntry* e = (BleEntry*)ctx;
    InterlockedExchange(&e->percent, (LONG)pct);
    if (g_wake_evt) SetEvent(g_wake_evt);
}

static bool cm_get_prop(DEVINST dn, const DEVPROPKEY* key, DEVPROPTYPE* ptype, void* buf, ULONG sz) {
    ULONG needed = sz;
    return CM_Get_DevNode_PropertyW(dn, key, ptype, (PBYTE)buf, &needed, 0) == CR_SUCCESS;
}

static DeviceKind kind_from_cat(const wchar_t* c) {
    if (!c || !*c) return KIND_OTHER;
    if (weq(c, L"Input.Keyboard")) return KIND_KEYBOARD;
    if (weq(c, L"Input.Mouse")) return KIND_MOUSE;
    if (weq(c, L"Input.Gaming")) return KIND_GAMEPAD;
    if (wstarts(c, L"Communication.Headset") || weq(c, L"Audio.Headphones")) return KIND_HEADSET;
    return KIND_OTHER;
}

static bool resolve_ble_devnode(DEVINST svc_dn, wchar_t* name, size_t ncap,
                                wchar_t* cid, size_t ccap, DeviceKind* kind, bool* connected) {
    *connected = false;
    *kind = KIND_OTHER;
    name[0] = 0;
    cid[0] = 0;

    DEVINST cur = svc_dn;
    for (int depth = 0; depth < 4 && cur; depth++) {
        wchar_t inst[256];
        memset(inst, 0, sizeof(inst));
        if (CM_Get_Device_IDW(cur, inst, 256, 0) == CR_SUCCESS) {
            if (wstartsi(inst, L"BTHLE\\DEV_") || wstartsi(inst, L"BTHLEDEVICE\\")) {
                DEVPROPTYPE t = 0;
                uint8_t b = 0;
                if (cm_get_prop(cur, &kPropIsConnected, &t, &b, sizeof(b)) && t == DEVPROP_TYPE_BOOLEAN) {
                    *connected = (b != 0);
                }
                GUID g;
                if (cm_get_prop(cur, &kPropContainerId, &t, &g, sizeof(g)) && t == DEVPROP_TYPE_GUID) {
                    guid_to_wstr(&g, cid, ccap);
                }
                wchar_t tmp[128];
                memset(tmp, 0, sizeof(tmp));
                if (cm_get_prop(cur, &kPropFriendlyName, &t, tmp, sizeof(tmp) - 2) && t == DEVPROP_TYPE_STRING && tmp[0]) {
                    wcpy(name, ncap, tmp);
                } else if (cm_get_prop(cur, &kPropDeviceDesc, &t, tmp, sizeof(tmp) - 2) && t == DEVPROP_TYPE_STRING && tmp[0]) {
                    wcpy(name, ncap, tmp);
                }
                memset(tmp, 0, sizeof(tmp));
                if (cm_get_prop(cur, &kPropCategory, &t, tmp, sizeof(tmp) - 2) &&
                    (t == DEVPROP_TYPE_STRING_LIST || t == DEVPROP_TYPE_STRING)) {
                    *kind = kind_from_cat(tmp);
                }
                if (*kind == KIND_OTHER && name[0]) *kind = kind_from_name(name);
                return true;
            }
        }
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
        cur = parent;
    }
    return false;
}

static bool open_and_subscribe_ble(BleEntry* e) {
    if (!load_bt_api()) return false;

    HANDLE h = CreateFileW(e->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    bool writable = (h != INVALID_HANDLE_VALUE);
    if (!writable) {
        h = CreateFileW(e->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) return false;

    USHORT nchars = 0;
    HRESULT hr = g_bt.get_chars(h, NULL, 0, NULL, &nchars, BLUETOOTH_GATT_FLAG_NONE);
    if (nchars == 0 || (hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) && FAILED(hr))) {
        CloseHandle(h);
        return false;
    }
    BTH_LE_GATT_CHARACTERISTIC* chars = (BTH_LE_GATT_CHARACTERISTIC*)xalloc(nchars * sizeof(BTH_LE_GATT_CHARACTERISTIC));
    if (!chars) { CloseHandle(h); return false; }

    USHORT actual = 0;
    hr = g_bt.get_chars(h, NULL, nchars, chars, &actual, BLUETOOTH_GATT_FLAG_NONE);
    if (FAILED(hr)) {
        xfree(chars);
        CloseHandle(h);
        return false;
    }

    int bat_idx = -1;
    for (USHORT i = 0; i < actual; i++) {
        if (is_uuid16(&chars[i].CharacteristicUuid, 0x2A19)) {
            bat_idx = (int)i;
            break;
        }
    }
    if (bat_idx < 0) {
        xfree(chars);
        CloseHandle(h);
        return false;
    }

    BTH_LE_GATT_CHARACTERISTIC bat_char = chars[bat_idx];
    xfree(chars);

    // Read initial value
    USHORT val_sz = 0;
    ULONG flags = BLUETOOTH_GATT_FLAG_NONE;
    hr = g_bt.get_char_val(h, &bat_char, 0, NULL, &val_sz, flags);
    if ((hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) && FAILED(hr)) || val_sz == 0) {
        flags = BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE;
        val_sz = 0;
        hr = g_bt.get_char_val(h, &bat_char, 0, NULL, &val_sz, flags);
    }
    int init_pct = -1;
    if (val_sz >= sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE)) {
        PBTH_LE_GATT_CHARACTERISTIC_VALUE val = (PBTH_LE_GATT_CHARACTERISTIC_VALUE)xalloc(val_sz);
        if (val) {
            if (SUCCEEDED(g_bt.get_char_val(h, &bat_char, val_sz, val, &val_sz, flags)) && val->DataSize >= 1) {
                init_pct = val->Data[0] <= 100 ? (int)val->Data[0] : 100;
            }
            xfree(val);
        }
    }
    if (init_pct < 0) {
        CloseHandle(h);
        return false;
    }

    e->h_svc = h;
    InterlockedExchange(&e->percent, (LONG)init_pct);

    // Register push notifications if supported
    if (bat_char.IsNotifiable && g_bt.reg_event) {
        BLUETOOTH_GATT_VALUE_CHANGED_EVENT_REGISTRATION reg;
        memset(&reg, 0, sizeof(reg));
        reg.NumCharacteristics = 1;
        reg.Characteristics[0] = bat_char;
        BLUETOOTH_GATT_EVENT_HANDLE ev_h = NULL;
        if (SUCCEEDED(g_bt.reg_event(h, CharacteristicValueChangedEvent, &reg, gatt_value_cb, e, &ev_h, BLUETOOTH_GATT_FLAG_NONE))) {
            e->h_ev = ev_h;
            // Enable CCCD notification bit on device if writable
            if (writable && g_bt.get_descs && g_bt.set_desc_val) {
                USHORT ndescs = 0;
                g_bt.get_descs(h, &bat_char, 0, NULL, &ndescs, BLUETOOTH_GATT_FLAG_NONE);
                if (ndescs > 0) {
                    BTH_LE_GATT_DESCRIPTOR* descs = (BTH_LE_GATT_DESCRIPTOR*)xalloc(ndescs * sizeof(BTH_LE_GATT_DESCRIPTOR));
                    if (descs) {
                        USHORT dact = 0;
                        if (SUCCEEDED(g_bt.get_descs(h, &bat_char, ndescs, descs, &dact, BLUETOOTH_GATT_FLAG_NONE))) {
                            for (USHORT j = 0; j < dact; j++) {
                                if (descs[j].DescriptorType == ClientCharacteristicConfiguration) {
                                    BTH_LE_GATT_DESCRIPTOR_VALUE dv;
                                    memset(&dv, 0, sizeof(dv));
                                    dv.DescriptorType = ClientCharacteristicConfiguration;
                                    dv.ClientCharacteristicConfiguration.IsSubscribeToNotification = TRUE;
                                    g_bt.set_desc_val(h, &descs[j], &dv, BLUETOOTH_GATT_FLAG_NONE);
                                    break;
                                }
                            }
                        }
                        xfree(descs);
                    }
                }
            }
        }
    }
    return true;
}

static void close_ble_entry(BleEntry* e) {
    HANDLE h_svc = e->h_svc;
    BLUETOOTH_GATT_EVENT_HANDLE h_ev = e->h_ev;
    e->active = false;
    e->h_svc = NULL;
    e->h_ev = NULL;
    if (h_ev && g_bt.unreg_event) g_bt.unreg_event(h_ev, BLUETOOTH_GATT_FLAG_NONE);
    if (h_svc && h_svc != INVALID_HANDLE_VALUE) CloseHandle(h_svc);
}

void drv_ble_sync(HANDLE wake_evt) {
    if (wake_evt) g_wake_evt = wake_evt;
    HDEVINFO di = SetupDiGetClassDevsW(&kGuidGattSvc, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) {
        drv_ble_close_all();
        return;
    }

    bool seen[MAX_BLE_DEVS];
    memset(seen, 0, sizeof(seen));

    uint8_t* det_buf = (uint8_t*)xalloc(1024);
    if (!det_buf) {
        SetupDiDestroyDeviceInfoList(di);
        return;
    }

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &kGuidGattSvc, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        if (!need || need > 1024) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_W* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)det_buf;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd;
        dd.cbSize = sizeof(dd);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, &dd)) continue;

        // Battery Service UUID 0000180f
        if (!wcontains_i(det->DevicePath, L"0000180f")) continue;

        wchar_t name[96], cid[40];
        DeviceKind kind = KIND_OTHER;
        bool connected = false;
        if (!resolve_ble_devnode(dd.DevInst, name, 96, cid, 40, &kind, &connected) || !connected) continue;

        // Check if already active
        int match = -1;
        AcquireSRWLockShared(&g_ble_lock);
        for (int k = 0; k < MAX_BLE_DEVS; k++) {
            if (g_ble[k].active && weqi(g_ble[k].path, det->DevicePath)) { match = k; break; }
        }
        ReleaseSRWLockShared(&g_ble_lock);

        if (match >= 0) {
            seen[match] = true;
            continue;
        }

        // Add new entry
        int slot = -1;
        AcquireSRWLockShared(&g_ble_lock);
        for (int k = 0; k < MAX_BLE_DEVS; k++) {
            if (!g_ble[k].active) { slot = k; break; }
        }
        ReleaseSRWLockShared(&g_ble_lock);
        if (slot < 0) continue;

        BleEntry temp;
        memset(&temp, 0, sizeof(temp));
        temp.active = true;
        wcpy(temp.path, 260, det->DevicePath);
        wcpy(temp.name, 96, name[0] ? name : L"Bluetooth LE device");
        wcpy(temp.container_id, 40, cid);
        wfmt(temp.id, 64, L"ble:%08X", fnv1a_w(cid[0] ? cid : det->DevicePath));
        temp.kind = kind;
        temp.gen = g_ble_gen++;

        // Put into slot before registering callback so ctx pointer is stable
        AcquireSRWLockExclusive(&g_ble_lock);
        g_ble[slot] = temp;
        ReleaseSRWLockExclusive(&g_ble_lock);

        if (open_and_subscribe_ble(&g_ble[slot])) {
            seen[slot] = true;
        } else {
            AcquireSRWLockExclusive(&g_ble_lock);
            g_ble[slot].active = false;
            ReleaseSRWLockExclusive(&g_ble_lock);
        }
    }
    xfree(det_buf);
    SetupDiDestroyDeviceInfoList(di);

    // Unsubscribe and close disconnected entries (unregister outside lock!)
    for (int k = 0; k < MAX_BLE_DEVS; k++) {
        if (g_ble[k].active && !seen[k]) {
            BleEntry copy;
            AcquireSRWLockExclusive(&g_ble_lock);
            copy = g_ble[k];
            memset(&g_ble[k], 0, sizeof(g_ble[k]));
            ReleaseSRWLockExclusive(&g_ble_lock);
            close_ble_entry(&copy);
        }
    }
}

void drv_ble_emit(ScanCtx* ctx) {
    AcquireSRWLockShared(&g_ble_lock);
    for (int k = 0; k < MAX_BLE_DEVS; k++) {
        BleEntry* e = &g_ble[k];
        if (!e->active) continue;
        // Skip if a vendor HID driver (e.g. Logitech HID++ over BLE) already claimed this physical device
        bool dup_hid = false;
        for (int i = 0; i < ctx->count; i++) {
            if (e->container_id[0] && weqi(ctx->obs[i].container_id, e->container_id)) {
                dup_hid = true;
                break;
            }
        }
        if (dup_hid) continue;

        Obs o;
        memset(&o, 0, sizeof(o));
        wcpy(o.id, 64, e->id);
        wcpy(o.name, 96, e->name);
        wcpy(o.via, 32, L"BLE GATT");
        wcpy(o.container_id, 40, e->container_id);
        o.kind = e->kind;
        o.has_kind = (e->kind != KIND_OTHER);
        o.state = STATE_BATTERY;
        o.percent = (int)InterlockedCompareExchange(&e->percent, 0, 0);
        o.is_bt = true;
        scan_ctx_add(ctx, &o);
        scan_ctx_add_bt_container(ctx, e->container_id);
    }
    ReleaseSRWLockShared(&g_ble_lock);
}

void drv_ble_close_all(void) {
    BleEntry copies[MAX_BLE_DEVS];
    AcquireSRWLockExclusive(&g_ble_lock);
    memcpy(copies, g_ble, sizeof(copies));
    memset(g_ble, 0, sizeof(g_ble));
    ReleaseSRWLockExclusive(&g_ble_lock);

    for (int k = 0; k < MAX_BLE_DEVS; k++) {
        if (copies[k].active) close_ble_entry(&copies[k]);
    }
}
