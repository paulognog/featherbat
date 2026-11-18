#include "alert.h"
#include <shellapi.h>

#define BALLOON_UID 1
#define TIMER_BALLOON_CLEANUP 2

static bool  g_balloon_icon_added = false;
static HICON g_app_icon = NULL;
static wchar_t g_alerted_ids[MAX_DEVICES][64];
static int     g_alerted_count = 0;

static bool is_alerted(const wchar_t* id) {
    for (int i = 0; i < g_alerted_count; i++) {
        if (weq(g_alerted_ids[i], id)) return true;
    }
    return false;
}

static void mark_alerted(const wchar_t* id) {
    if (is_alerted(id) || g_alerted_count >= MAX_DEVICES) return;
    wcpy(g_alerted_ids[g_alerted_count++], 64, id);
}

static void clear_alerted(const wchar_t* id) {
    for (int i = 0; i < g_alerted_count; i++) {
        if (weq(g_alerted_ids[i], id)) {
            if (i + 1 < g_alerted_count) {
                wcpy(g_alerted_ids[i], 64, g_alerted_ids[g_alerted_count - 1]);
            }
            g_alerted_count--;
            return;
        }
    }
}

static bool alerts_enabled(void) {
    DWORD val = 1, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\featherbat", L"LowBatteryAlerts",
                     RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        return val != 0;
    }
    return true;
}

static void show_balloon(HWND hwnd, HINSTANCE inst, const wchar_t* title, const wchar_t* body) {
    if (!g_app_icon && inst) {
        g_app_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    }
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = BALLOON_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY_CB;
    nid.hIcon = g_app_icon;
    wcpy(nid.szTip, ARRAYSIZE(nid.szTip), L"FeatherBat");
    wcpy(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    wcpy(nid.szInfo, ARRAYSIZE(nid.szInfo), body);
    nid.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;

    if (!g_balloon_icon_added) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_balloon_icon_added = true;
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    SetTimer(hwnd, TIMER_BALLOON_CLEANUP, 10000, NULL);
}

void alert_check(HWND ctrl_hwnd, HINSTANCE inst, Device* devs, int count) {
    if (!ctrl_hwnd || !devs) return;
    bool enabled = alerts_enabled();
    bool pt = is_pt_ui();

    for (int i = 0; i < count; i++) {
        Device* d = &devs[i];
        if (d->state != STATE_BATTERY || d->percent < 0) continue;

        if (d->charging || d->percent > REARM_BATTERY_PCT) {
            clear_alerted(d->id);
        } else if (d->percent <= LOW_BATTERY_PCT && !is_alerted(d->id)) {
            mark_alerted(d->id);
            if (enabled) {
                wchar_t body[256];
                if (pt) {
                    wfmt(body, ARRAYSIZE(body), L"%s está com %d%%. Hora de carregar.", d->name, d->percent);
                    show_balloon(ctrl_hwnd, inst, L"Bateria baixa", body);
                } else {
                    wfmt(body, ARRAYSIZE(body), L"%s is at %d%%. Time to charge it.", d->name, d->percent);
                    show_balloon(ctrl_hwnd, inst, L"Low battery", body);
                }
            }
        }
    }
}

void alert_cleanup(HWND ctrl_hwnd) {
    if (ctrl_hwnd) KillTimer(ctrl_hwnd, TIMER_BALLOON_CLEANUP);
    if (g_balloon_icon_added && ctrl_hwnd) {
        NOTIFYICONDATAW nid;
        memset(&nid, 0, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = ctrl_hwnd;
        nid.uID = BALLOON_UID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_balloon_icon_added = false;
    }
}

void alert_on_tray_msg(HWND ctrl_hwnd, LPARAM lp) {
    UINT ev = LOWORD(lp);
    if (ev == NIN_BALLOONTIMEOUT || ev == NIN_BALLOONUSERCLICK) {
        alert_cleanup(ctrl_hwnd);
    }
}
