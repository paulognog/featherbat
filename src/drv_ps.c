#include "drivers.h"

#define SONY_VID 0x054C

static bool is_ds4(uint16_t pid) {
    return pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;
}

static bool is_dualsense(uint16_t pid) {
    return pid == 0x0CE6 || pid == 0x0DF2;
}

void ps_parse_dualsense(uint8_t status, int* pct, bool* charging) {
    int level = status & 0x0F;
    int state = status >> 4;
    *pct = clamp100(level * 10 + 5);
    *charging = (state == 0x01);
    if (state == 0x02) {
        *pct = 100;
        *charging = true;
    }
}

void ps_parse_ds4(uint8_t status, int* pct, bool* charging) {
    int level = status & 0x0F;
    bool cable = (status & 0x10) != 0;
    if (cable) {
        *pct = (level >= 11) ? 100 : clamp100(level * 10);
        *charging = (level < 11);
    } else {
        *pct = clamp100(level * 10 + 5);
        *charging = false;
    }
}

bool drv_ps_present(const HidList* hids) {
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid == SONY_VID && (is_ds4(c->pid) || is_dualsense(c->pid)) &&
            c->usage_page == 0x01 && c->usage == 0x05) {
            return true;
        }
    }
    return false;
}

void drv_ps_scan(ScanCtx* ctx) {
    if (!ctx->hids) return;
    for (int i = 0; i < ctx->hids->count; i++) {
        const HidColl* c = &ctx->hids->items[i];
        if (c->vid != SONY_VID || !(is_ds4(c->pid) || is_dualsense(c->pid))) continue;
        if (c->usage_page != 0x01 || c->usage != 0x05) continue;

        HidDev d;
        if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) {
            if (!hid_open(&d, c, GENERIC_READ, true)) continue;
        }
        bool bt = c->in_len > 64;
        if (bt) {
            uint8_t feat[128];
            int flen = 0;
            hid_get_feature(&d, 0x05, feat, sizeof(feat), &flen);
        }

        uint8_t r[128];
        int pct = -1;
        bool charging = false;
        bool found = false;
        ULONGLONG end = GetTickCount64() + 400;
        while (!found && (LONGLONG)(end - GetTickCount64()) > 0) {
            int n = hid_read_sync(&d, r, sizeof(r), 80);
            if (n < 10) continue;
            if (is_ds4(c->pid)) {
                int at = (r[0] == 0x01) ? 30 : (r[0] == 0x11) ? 32 : 0;
                if (!at || n <= at || (r[0] == 0x01 && bt)) continue;
                if (c->pid == 0x0BA0 && r[at] == 0) break; // wireless adapter without controller
                ps_parse_ds4(r[at], &pct, &charging);
                found = true;
            } else {
                int at = (r[0] == 0x01) ? 53 : (r[0] == 0x31) ? 54 : 0;
                if (!at || n <= at || (r[0] == 0x01 && bt)) continue;
                ps_parse_dualsense(r[at], &pct, &charging);
                found = true;
            }
        }
        hid_close(&d);
        if (!found) continue;

        Obs o;
        memset(&o, 0, sizeof(o));
        wfmt(o.id, 64, L"ps:%04x:%08X", c->pid, fnv1a_w(c->path));
        wcpy(o.name, 96, is_ds4(c->pid) ? L"DualShock 4" : (c->pid == 0x0DF2 ? L"DualSense Edge" : L"DualSense"));
        wcpy(o.via, 32, bt ? L"Bluetooth" : (c->pid == 0x0BA0 ? L"Wireless adapter" : L"USB"));
        hid_container_id(c->path, o.container_id, 40);
        o.kind = KIND_GAMEPAD;
        o.has_kind = true;
        o.state = STATE_BATTERY;
        o.percent = pct;
        o.charging = charging;
        o.is_bt = bt;
        o.vid = c->vid;
        o.pid = c->pid;
        scan_ctx_add(ctx, &o);
        scan_ctx_claim(ctx, c->vid, c->pid);
    }
}
