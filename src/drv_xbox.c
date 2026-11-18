#include "drivers.h"
#include <xinput.h>

typedef DWORD (WINAPI *FnXInputGetBattery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD (WINAPI *FnXInputGetState)(DWORD, XINPUT_STATE*);

static FnXInputGetBattery g_xbat = NULL;
static FnXInputGetState   g_xstate = NULL;
static bool g_xinput_tried = false;

static bool load_xinput(void) {
    if (g_xinput_tried) return g_xbat != NULL && g_xstate != NULL;
    g_xinput_tried = true;
    static const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 2; i++) {
        HMODULE m = LoadLibraryExW(dlls[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m) {
            g_xbat = (FnXInputGetBattery)(void*)GetProcAddress(m, "XInputGetBatteryInformation");
            g_xstate = (FnXInputGetState)(void*)GetProcAddress(m, "XInputGetState");
            if (g_xbat && g_xstate) return true;
        }
    }
    return false;
}

bool drv_xbox_present(const HidList* hids) {
    if (!hids) return false;
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid == 0x045E || wcontains_i(c->path, L"&ig_")) return true;
    }
    return false;
}

void drv_xbox_scan(ScanCtx* ctx) {
    if (!drv_xbox_present(ctx->hids)) return;

    // If an Xbox controller is already reported via Bluetooth/BLE GATT with exact %, don't duplicate
    bool bt_xbox = false;
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->obs[i].is_bt && wcontains_i(ctx->obs[i].name, L"xbox")) {
            bt_xbox = true;
            break;
        }
    }
    if (!load_xinput()) return;

    for (DWORD user = 0; user < 4; user++) {
        XINPUT_STATE st;
        memset(&st, 0, sizeof(st));
        if (g_xstate(user, &st) != ERROR_SUCCESS) continue;

        XINPUT_BATTERY_INFORMATION pad;
        memset(&pad, 0, sizeof(pad));
        bool has_pad = (g_xbat(user, BATTERY_DEVTYPE_GAMEPAD, &pad) == ERROR_SUCCESS &&
                        pad.BatteryType != BATTERY_TYPE_DISCONNECTED &&
                        pad.BatteryType != BATTERY_TYPE_WIRED);

        BYTE types[2] = { BATTERY_DEVTYPE_GAMEPAD, BATTERY_DEVTYPE_HEADSET };
        for (int t = 0; t < 2; t++) {
            BYTE type = types[t];
            if (type == BATTERY_DEVTYPE_GAMEPAD && bt_xbox) continue;

            XINPUT_BATTERY_INFORMATION bi;
            memset(&bi, 0, sizeof(bi));
            if (type == BATTERY_DEVTYPE_GAMEPAD) {
                if (!has_pad) continue;
                bi = pad;
            } else {
                if (g_xbat(user, type, &bi) != ERROR_SUCCESS ||
                    bi.BatteryType == BATTERY_TYPE_DISCONNECTED ||
                    bi.BatteryType == BATTERY_TYPE_WIRED ||
                    bi.BatteryType == BATTERY_TYPE_UNKNOWN) {
                    continue;
                }
                if (has_pad && bi.BatteryType == pad.BatteryType && bi.BatteryLevel == pad.BatteryLevel) {
                    continue; // mirrored controller battery, not a real headset
                }
            }

            Obs o;
            memset(&o, 0, sizeof(o));
            wfmt(o.id, 64, L"xinput:%u:%u", (unsigned)user, (unsigned)type);
            if (type == BATTERY_DEVTYPE_GAMEPAD) {
                wfmt(o.name, 96, L"Xbox controller %u", (unsigned)(user + 1));
                o.kind = KIND_GAMEPAD;
            } else {
                wcpy(o.name, 96, L"Xbox headset");
                o.kind = KIND_HEADSET;
            }
            wcpy(o.via, 32, L"XInput");
            o.has_kind = true;
            o.state = STATE_BATTERY;
            switch (bi.BatteryLevel) {
                case BATTERY_LEVEL_EMPTY:  o.percent = 5;   wcpy(o.level_text, 16, L"Empty");  break;
                case BATTERY_LEVEL_LOW:    o.percent = 25;  wcpy(o.level_text, 16, L"Low");    break;
                case BATTERY_LEVEL_MEDIUM: o.percent = 60;  wcpy(o.level_text, 16, L"Medium"); break;
                default:                   o.percent = 100; wcpy(o.level_text, 16, L"Full");   break;
            }
            scan_ctx_add(ctx, &o);
        }
    }
}
