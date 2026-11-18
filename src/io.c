#include "io.h"
#include "drivers.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>

static const GUID kGuidHid = {
    0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }
};
static const GUID kGuidGattSvc = {
    0x6e3bb679, 0x4372, 0x40c8, { 0x9e, 0xaa, 0x45, 0x09, 0xdf, 0x26, 0x0c, 0xd8 }
};
static const GUID kGuidBleDev = {
    0x781aee18, 0x7733, 0x4ce4, { 0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92 }
};
static const GUID kGuidBthPort = {
    0x0850302A, 0xB344, 0x4fda, { 0x9B, 0xE9, 0x90, 0x57, 0x6B, 0x8D, 0x46, 0xF0 }
};
static const GUID kGuidAudioRender = {
    0xE6327CAD, 0xDCEC, 0x4949, { 0xAE, 0x8A, 0x99, 0x1E, 0x97, 0x6A, 0x79, 0xD2 }
};

static HWND    g_ctrl_hwnd = NULL;
static HANDLE  g_stop_evt  = NULL;
static HANDLE  g_wake_evt  = NULL;
static HANDLE  g_io_thread = NULL;
static volatile LONG g_pnp_dirty = 0;
static volatile LONG g_force_refresh = 0;

static HCMNOTIFICATION g_cm_notifs[5];

static SRWLOCK g_snap_lock = SRWLOCK_INIT;
static Device  g_tracked[MAX_DEVICES];
static int     g_tracked_count = 0;

static Obs     g_fast_obs[MAX_DEVICES];
static int     g_fast_count = 0;
static Obs     g_slow_obs[MAX_DEVICES];
static int     g_slow_count = 0;

static bool g_has_fast_pull = false;
static bool g_has_genhid_dev = false;

static DWORD CALLBACK cm_notify_cb(HCMNOTIFICATION hNotify, PVOID ctx, CM_NOTIFY_ACTION action,
                                   PCM_NOTIFY_EVENT_DATA evData, DWORD evSize) {
    (void)hNotify; (void)ctx; (void)evData; (void)evSize;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
        action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        io_notify_pnp();
    }
    return ERROR_SUCCESS;
}

static bool devices_visually_equal(const Device* a, int na, const Device* b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        if (a[i].kind != b[i].kind || a[i].state != b[i].state ||
            a[i].percent != b[i].percent || a[i].charging != b[i].charging ||
            !weq(a[i].id, b[i].id) || !weq(a[i].name, b[i].name) ||
            !weq(a[i].level_text, b[i].level_text)) {
            return false;
        }
    }
    return true;
}

static void sort_devices(Device* devs, int n) {
    for (int i = 1; i < n; i++) {
        Device key = devs[i];
        int j = i - 1;
        while (j >= 0) {
            bool gt = (devs[j].kind > key.kind) ||
                      (devs[j].kind == key.kind && wcmpi(devs[j].name, key.name) > 0);
            if (!gt) break;
            devs[j + 1] = devs[j];
            j--;
        }
        devs[j + 1] = key;
    }
}

static bool merge_observations(const ScanCtx* ctx) {
    ULONGLONG now = GetTickCount64();
    Device prev[MAX_DEVICES];
    int prev_n = 0;

    AcquireSRWLockExclusive(&g_snap_lock);
    prev_n = g_tracked_count;
    if (prev_n > 0) memcpy(prev, g_tracked, (size_t)prev_n * sizeof(Device));

    bool seen[MAX_DEVICES];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < ctx->count; i++) {
        const Obs* o = &ctx->obs[i];
        int pos = -1;
        for (int j = 0; j < g_tracked_count; j++) {
            if (weq(g_tracked[j].id, o->id)) { pos = j; break; }
        }
        if (pos < 0) {
            if (g_tracked_count >= MAX_DEVICES) continue;
            pos = g_tracked_count++;
            memset(&g_tracked[pos], 0, sizeof(Device));
            wcpy(g_tracked[pos].id, 64, o->id);
            g_tracked[pos].percent = -1;
        }
        seen[pos] = true;
        Device* d = &g_tracked[pos];
        if (o->name[0]) wcpy(d->name, 96, o->name);
        if (o->via[0])  wcpy(d->via, 32, o->via);
        wcpy(d->level_text, 16, o->level_text);
        if (o->container_id[0]) wcpy(d->container_id, 40, o->container_id);
        if (o->has_kind) d->kind = o->kind;
        else if (d->name[0] && d->kind == KIND_OTHER) d->kind = kind_from_name(d->name);
        d->millivolts = o->millivolts;
        d->charging = o->charging;
        d->is_bt = o->is_bt;
        d->grace_ms = o->grace_ms;
        d->vid = o->vid;
        d->pid = o->pid;

        if (o->state == STATE_BATTERY) {
            d->state = STATE_BATTERY;
            if (o->percent >= 0) d->percent = o->percent;
            d->last_seen_ms = now;
        } else if (o->state == STATE_ASLEEP) {
            d->state = (d->percent >= 0) ? STATE_ASLEEP : STATE_OFF;
        } else {
            d->state = STATE_OFF;
        }
    }

    for (int j = 0; j < g_tracked_count; j++) {
        if (!seen[j]) {
            Device* d = &g_tracked[j];
            if (d->grace_ms > 0 && (d->state == STATE_BATTERY || d->state == STATE_ASLEEP) &&
                d->percent >= 0 && (now - d->last_seen_ms) < d->grace_ms) {
                d->state = STATE_ASLEEP;
            } else {
                d->state = STATE_OFF;
            }
        }
    }

    sort_devices(g_tracked, g_tracked_count);
    bool changed = !devices_visually_equal(prev, prev_n, g_tracked, g_tracked_count);
    ReleaseSRWLockExclusive(&g_snap_lock);
    return changed;
}

static bool emit_and_update(const HidList* hids, bool run_fast_pulls, bool run_slow_pulls, bool once_mode) {
    ScanCtx ctx;
    scan_ctx_init(&ctx, hids);

    drv_logi_emit(&ctx);
    drv_ble_emit(&ctx);
    drv_xenta_emit(&ctx);

    if (run_fast_pulls && hids) {
        int start = ctx.count;
        if (drv_rongyuan_present(hids)) drv_rongyuan_scan(&ctx);
        if (drv_kysona_present(hids))   drv_kysona_scan(&ctx);
        if (drv_hyperx_present(hids))   drv_hyperx_scan(&ctx);
        if (drv_ps_present(hids))       drv_ps_scan(&ctx);
        if (once_mode || g_has_genhid_dev) {
            int before = ctx.count;
            drv_genhid_scan(&ctx);
            if (ctx.count > before) g_has_genhid_dev = true;
            else if (once_mode) g_has_genhid_dev = false;
        }
        if (drv_xbox_present(hids)) drv_xbox_scan(&ctx);

        g_fast_count = 0;
        for (int i = start; i < ctx.count && g_fast_count < MAX_DEVICES; i++) {
            g_fast_obs[g_fast_count++] = ctx.obs[i];
        }
    } else {
        for (int i = 0; i < g_fast_count; i++) {
            const Obs* o = &g_fast_obs[i];
            if (o->vid && scan_ctx_is_claimed(&ctx, o->vid, o->pid)) continue;
            scan_ctx_add(&ctx, o);
            if (o->vid) scan_ctx_claim(&ctx, o->vid, o->pid);
        }
    }

    if (run_slow_pulls) {
        int start = ctx.count;
        drv_btc_scan(&ctx);
        if (drv_hsc_present()) drv_hsc_scan(&ctx);

        g_slow_count = 0;
        for (int i = start; i < ctx.count && g_slow_count < MAX_DEVICES; i++) {
            g_slow_obs[g_slow_count++] = ctx.obs[i];
        }
    } else {
        for (int i = 0; i < g_slow_count; i++) {
            const Obs* o = &g_slow_obs[i];
            if (o->container_id[0] && scan_ctx_has_bt_container(&ctx, o->container_id)) continue;
            bool dup = false;
            for (int k = 0; k < ctx.count; k++) {
                if ((o->container_id[0] && weqi(ctx.obs[k].container_id, o->container_id)) ||
                    (o->name[0] && weq(ctx.obs[k].name, o->name))) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                scan_ctx_add(&ctx, o);
                if (o->container_id[0]) scan_ctx_add_bt_container(&ctx, o->container_id);
            }
        }
    }

    bool changed = merge_observations(&ctx);
    scan_ctx_free(&ctx);
    return changed;
}

static bool check_fast_pull_presence(const HidList* hids) {
    return drv_rongyuan_present(hids) || drv_kysona_present(hids) ||
           drv_hyperx_present(hids) || drv_ps_present(hids) ||
           drv_xbox_present(hids) || drv_logi_g435_present(hids) ||
           g_has_genhid_dev;
}

static DWORD WINAPI io_thread_proc(LPVOID param) {
    (void)param;
    HidList hids;
    memset(&hids, 0, sizeof(hids));

    ULONGLONG last_fast_ms = 0;
    ULONGLONG last_logi_ms = 0;
    ULONGLONG last_slow_ms = 0;

    // Initial full scan
    hid_enum(&hids);
    drv_logi_sync(&hids, false);
    drv_ble_sync(g_wake_evt);
    drv_xenta_sync(&hids, false);
    g_has_genhid_dev = true; // check once on startup / PnP change
    if (emit_and_update(&hids, true, true, true) && g_ctrl_hwnd) {
        PostMessageW(g_ctrl_hwnd, WM_APP_UPDATE, 0, 0);
    }
    g_has_fast_pull = check_fast_pull_presence(&hids);
    ULONGLONG now = GetTickCount64();
    last_fast_ms = now;
    last_logi_ms = now;
    last_slow_ms = now;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    for (;;) {
        now = GetTickCount64();

        // Compute nearest scheduled timer
        DWORD wait_ms = 300000; // 5 min slow pull for classic BT / optional HSC
        ULONGLONG slow_elapsed = now - last_slow_ms;
        if (slow_elapsed >= 300000ULL) wait_ms = 0;
        else if ((DWORD)(300000ULL - slow_elapsed) < wait_ms) wait_ms = (DWORD)(300000ULL - slow_elapsed);

        if (g_has_fast_pull) {
            ULONGLONG fast_elapsed = now - last_fast_ms;
            if (fast_elapsed >= 60000ULL) wait_ms = 0;
            else if ((DWORD)(60000ULL - fast_elapsed) < wait_ms) wait_ms = (DWORD)(60000ULL - fast_elapsed);
        }

        // Logitech awake safety refresh (180 s)
        HANDLE waits[MAXIMUM_WAIT_OBJECTS];
        waits[0] = g_stop_evt;
        waits[1] = g_wake_evt;
        int nw = 2;
        int logi_h = drv_logi_wait_handles(waits + nw, MAXIMUM_WAIT_OBJECTS - nw);
        nw += logi_h;
        int xenta_h = drv_xenta_wait_handles(waits + nw, MAXIMUM_WAIT_OBJECTS - nw);
        nw += xenta_h;

        if (logi_h > 0) {
            ULONGLONG logi_elapsed = now - last_logi_ms;
            if (logi_elapsed >= 180000ULL) wait_ms = 0;
            else if ((DWORD)(180000ULL - logi_elapsed) < wait_ms) wait_ms = (DWORD)(180000ULL - logi_elapsed);
        }

        DWORD wr = WaitForMultipleObjects((DWORD)nw, waits, FALSE, wait_ms);
        if (wr == WAIT_OBJECT_0) break; // stop

        bool do_notify = false;
        if (wr == WAIT_OBJECT_0 + 1) {
            if (InterlockedExchange(&g_pnp_dirty, 0) != 0 || InterlockedExchange(&g_force_refresh, 0) != 0) {
                bool was_force = (InterlockedExchange(&g_force_refresh, 0) != 0);
                // Coalesce bursty PnP arrival events (skip delay if user clicked Refresh now)
                if (!was_force && WaitForSingleObject(g_stop_evt, 1200) == WAIT_OBJECT_0) break;
                InterlockedExchange(&g_pnp_dirty, 0);

                hid_list_free(&hids);
                hid_enum(&hids);
                drv_logi_sync(&hids, false);
                drv_ble_sync(g_wake_evt);
                drv_xenta_sync(&hids, false);
                g_has_genhid_dev = true;
                do_notify = emit_and_update(&hids, true, true, true);
                g_has_fast_pull = check_fast_pull_presence(&hids);
                now = GetTickCount64();
                last_fast_ms = now;
                last_logi_ms = now;
                last_slow_ms = now;
            } else {
                // BLE GATT push notification arrived
                do_notify = emit_and_update(&hids, false, false, false);
            }
        } else if (wr >= WAIT_OBJECT_0 + 2 && wr < (DWORD)(WAIT_OBJECT_0 + nw)) {
            bool ch = false;
            ch |= drv_logi_on_io_ready();
            ch |= drv_xenta_on_io_ready();
            if (ch) do_notify = emit_and_update(&hids, false, false, false);
        } else if (wr == WAIT_TIMEOUT) {
            now = GetTickCount64();
            bool need_fast = g_has_fast_pull && (now - last_fast_ms) >= 60000ULL;
            bool need_logi = logi_h > 0 && (now - last_logi_ms) >= 180000ULL;
            bool need_slow = (now - last_slow_ms) >= 300000ULL;

            if (need_logi) {
                drv_logi_refresh_awake();
                last_logi_ms = now;
            }
            if (need_fast) {
                drv_xenta_drain_all();
                if (drv_logi_g435_present(&hids)) drv_logi_sync(&hids, false);
                last_fast_ms = now;
            }
            if (need_slow) {
                last_slow_ms = now;
            }
            do_notify = emit_and_update(&hids, need_fast, need_slow, false);
        }

        if (do_notify && g_ctrl_hwnd) {
            PostMessageW(g_ctrl_hwnd, WM_APP_UPDATE, 0, 0);
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    }

    drv_logi_close_all();
    drv_ble_close_all();
    drv_xenta_close_all();
    hid_list_free(&hids);
    return 0;
}

void io_start(HWND ctrl_hwnd) {
    g_ctrl_hwnd = ctrl_hwnd;
    g_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wake_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    static const GUID* kGuids[5] = {
        &kGuidHid, &kGuidGattSvc, &kGuidBleDev, &kGuidBthPort, &kGuidAudioRender
    };
    for (int i = 0; i < 5; i++) {
        CM_NOTIFY_FILTER f;
        memset(&f, 0, sizeof(f));
        f.cbSize = sizeof(f);
        f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        f.u.DeviceInterface.ClassGuid = *kGuids[i];
        CM_Register_Notification(&f, NULL, cm_notify_cb, &g_cm_notifs[i]);
    }

    g_io_thread = CreateThread(NULL, 0, io_thread_proc, NULL, 0, NULL);
}

void io_stop(void) {
    for (int i = 0; i < 5; i++) {
        if (g_cm_notifs[i]) {
            CM_Unregister_Notification(g_cm_notifs[i]);
            g_cm_notifs[i] = NULL;
        }
    }
    if (g_stop_evt) SetEvent(g_stop_evt);
    if (g_io_thread) {
        WaitForSingleObject(g_io_thread, 4000);
        CloseHandle(g_io_thread);
        g_io_thread = NULL;
    }
    if (g_stop_evt) { CloseHandle(g_stop_evt); g_stop_evt = NULL; }
    if (g_wake_evt) { CloseHandle(g_wake_evt); g_wake_evt = NULL; }
}

void io_notify_pnp(void) {
    InterlockedExchange(&g_pnp_dirty, 1);
    if (g_wake_evt) SetEvent(g_wake_evt);
}

void io_refresh_now(void) {
    InterlockedExchange(&g_force_refresh, 1);
    if (g_wake_evt) SetEvent(g_wake_evt);
}

int io_snapshot(Device* out, int max_count) {
    AcquireSRWLockShared(&g_snap_lock);
    int n = g_tracked_count < max_count ? g_tracked_count : max_count;
    if (n > 0) memcpy(out, g_tracked, (size_t)n * sizeof(Device));
    ReleaseSRWLockShared(&g_snap_lock);
    return n;
}

static const wchar_t* kind_label(DeviceKind k) {
    switch (k) {
        case KIND_HEADSET:  return L"Headset";
        case KIND_EARBUDS:  return L"Earbuds";
        case KIND_MOUSE:    return L"Mouse";
        case KIND_KEYBOARD: return L"Keyboard";
        case KIND_GAMEPAD:  return L"Gamepad";
        default:            return L"Other";
    }
}

int io_run_once(void) {
    con_init();
    HidList hids;
    hid_enum(&hids);

    drv_logi_sync(&hids, true);
    drv_ble_sync(NULL);
    drv_xenta_sync(&hids, true);

    ScanCtx ctx;
    scan_ctx_init(&ctx, &hids);
    drv_logi_emit(&ctx);
    drv_ble_emit(&ctx);
    drv_xenta_emit(&ctx);
    drv_rongyuan_scan(&ctx);
    drv_kysona_scan(&ctx);
    drv_hyperx_scan(&ctx);
    drv_ps_scan(&ctx);
    drv_btc_scan(&ctx);
    drv_genhid_scan(&ctx);
    drv_xbox_scan(&ctx);
    if (drv_hsc_present()) drv_hsc_scan(&ctx);

    if (ctx.count == 0) {
        con_puts(L"No wireless battery devices found.\n");
    } else {
        for (int i = 0; i < ctx.count; i++) {
            const Obs* o = &ctx.obs[i];
            DeviceKind k = o->has_kind ? o->kind : kind_from_name(o->name);
            con_fmt(L"%-10s %-34s ", kind_label(k), o->name[0] ? o->name : L"Device");
            if (o->state == STATE_OFF) {
                con_puts(L"off");
            } else {
                if (o->percent >= 0) con_fmt(L"%3d%%", o->percent);
                else if (o->level_text[0]) con_fmt(L"%s", o->level_text);
                else con_puts(L"  ?%");
                if (o->charging) con_puts(L" (charging)");
                if (o->state == STATE_ASLEEP) con_puts(L" (asleep)");
                if (o->millivolts > 0) con_fmt(L" (%d mV)", o->millivolts);
            }
            con_fmt(L"  [%s]  (%s)\n", o->via[0] ? o->via : L"?", o->id);
        }
    }

    scan_ctx_free(&ctx);
    drv_logi_close_all();
    drv_ble_close_all();
    drv_xenta_close_all();
    hid_list_free(&hids);
    con_close();
    return 0;
}

int io_run_dump(void) {
    con_init();
    HidList hids;
    hid_enum(&hids);

    WBuf out;
    wb_init(&out, 4096);
    wb_puts(&out, L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n");
    for (int i = 0; i < hids.count; i++) {
        const HidColl* c = &hids.items[i];
        wchar_t prod[96];
        hid_product_name(c->path, prod, 96);
        wb_fmt(&out, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   %s\n",
               c->vid, c->pid, c->iface, c->usage_page, c->usage,
               c->in_len, c->out_len, c->feat_len, prod);
    }
    drv_xenta_dump(&hids, &out);

    // Also dump Bluetooth PnP nodes for easy troubleshooting
    wb_puts(&out, L"\nBluetooth PnP nodes:\n");
    HDEVINFO di = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di != INVALID_HANDLE_VALUE) {
        static const DEVPROPKEY kBat = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
        static const DEVPROPKEY kConn = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };
        static const DEVPROPKEY kName = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
        wchar_t* inst = (wchar_t*)xalloc(512 * sizeof(wchar_t));
        SP_DEVINFO_DATA dd;
        dd.cbSize = sizeof(dd);
        if (inst) {
            for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
                if (!SetupDiGetDeviceInstanceIdW(di, &dd, inst, 512, NULL)) continue;
                if (!wstartsi(inst, L"BTHENUM\\") && !wstartsi(inst, L"BTHLE\\") && !wstartsi(inst, L"BTHLEDEVICE\\")) continue;
                DEVPROPTYPE t = 0;
                uint8_t bat = 255, conn = 255;
                wchar_t name[96];
                name[0] = 0;
                SetupDiGetDevicePropertyW(di, &dd, &kName, &t, (PBYTE)name, sizeof(name) - 2, NULL, 0);
                if (!SetupDiGetDevicePropertyW(di, &dd, &kConn, &t, &conn, 1, NULL, 0) || t != DEVPROP_TYPE_BOOLEAN) conn = 255;
                if (!SetupDiGetDevicePropertyW(di, &dd, &kBat, &t, &bat, 1, NULL, 0) || t != DEVPROP_TYPE_BYTE) bat = 255;
                wb_fmt(&out, L"  conn=%d bat=%d name=\"%s\" id=%s\n",
                       conn == 255 ? -1 : (conn != 0), bat == 255 ? -1 : (int)bat, name, inst);
            }
            xfree(inst);
        }
        SetupDiDestroyDeviceInfoList(di);
    }

    if (out.data) con_puts(out.data);
    wb_free(&out);
    hid_list_free(&hids);
    con_close();
    return 0;
}
