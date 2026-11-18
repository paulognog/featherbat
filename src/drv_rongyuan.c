#include "drivers.h"

static const uint32_t kReceivers[] = {
    0x0DB0197E, 0x0DB08E9A, 0x145F0376, 0x145F038D, 0x14A5C045, 0x25AA2008, 0x25AA4008, 0x25AA4018,
    0x31514011, 0x31514014, 0x31514017, 0x31514021, 0x31514027, 0x3151402D, 0x31514038, 0x31515006,
    0x31515007, 0x31515008, 0x31515020, 0x31515026, 0x3151502B, 0x31515031, 0x31515037, 0x31515038,
    0x3151503D, 0x3151503F, 0x31515040, 0x31515042, 0x31515045, 0x3151504B, 0x3151504E, 0x31515050,
    0x31515052, 0x31515055, 0x3151505A, 0x342DE3E0, 0x342DE3E1, 0x342DE3E2, 0x342DE3E6, 0x342DE3E7,
    0x342DE3E8, 0x342DE3F5, 0x342DE3F6, 0x342DE3F7, 0x342DE3FB, 0x342DE3FC, 0x342DE3FD, 0x347AA302,
    0x347AA305, 0x374AA205, 0x374AA217, 0x374AA220, 0x374AA223, 0x374AA226, 0x374AA229, 0x374AA234,
    0x374AA237, 0x374AA251, 0x374AA263, 0x374AA268, 0x374AA311, 0x374AA314, 0x374AA319, 0x374AA322,
    0x374AA329, 0x374AA355, 0x379A1804, 0x379A1807, 0x38A90002, 0x38EE0000, 0x38EE0002, 0x38EE0039,
    0x391D1A04, 0x391D1A08, 0x391D1A0E, 0x391D1A13, 0x391D1A25, 0x391D1A2B, 0x391D1A34, 0x391D2017,
    0x391D2A05,
};

static bool is_rongyuan(uint16_t vid, uint16_t pid) {
    uint32_t key = ((uint32_t)vid << 16) | pid;
    for (size_t i = 0; i < ARRAYSIZE(kReceivers); i++) {
        if (kReceivers[i] == key) return true;
    }
    return false;
}

bool drv_rongyuan_present(const HidList* hids) {
    if (!hids) return false;
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (is_rongyuan(c->vid, c->pid) && c->iface == 2 &&
            c->usage_page == 0xFFFF && c->usage == 0x0002 && c->feat_len >= 7) {
            return true;
        }
    }
    return false;
}

static bool query_status(HidDev* d, uint8_t* reply, int cap) {
    uint8_t cmd[128];
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) Sleep(100);
        memset(cmd, 0, sizeof(cmd));
        cmd[1] = 0xF7;
        if (!hid_set_feature(d, cmd, d->feat_len < (int)sizeof(cmd) ? d->feat_len : (int)sizeof(cmd))) return false;
        Sleep(20);
        int rlen = 0;
        if (hid_get_feature(d, 0, reply, cap, &rlen) && rlen >= 7 &&
            (reply[2] | reply[3] | reply[4] | reply[5] | reply[6]) != 0) {
            return true;
        }
    }
    return false;
}

void drv_rongyuan_scan(ScanCtx* ctx) {
    if (!ctx->hids) return;
    for (int i = 0; i < ctx->hids->count; i++) {
        const HidColl* c = &ctx->hids->items[i];
        if (!is_rongyuan(c->vid, c->pid) || scan_ctx_is_claimed(ctx, c->vid, c->pid)) continue;
        if (c->iface != 2 || c->usage_page != 0xFFFF || c->usage != 0x0002 || c->feat_len < 7) continue;

        HidDev d;
        if (!hid_open(&d, c, 0, false)) continue;
        uint8_t r[128];
        bool ok = query_status(&d, r, sizeof(r));
        hid_close(&d);
        if (!ok) continue;

        wchar_t product[96];
        hid_product_name(c->path, product, 96);
        bool generic = !product[0] || wcontains_i(product, L"Receiver") || wcontains_i(product, L"2.4G");

        for (int slot = 0; slot < 2; slot++) {
            bool kbd = (slot == 0);
            if (r[kbd ? 4 : 5] != 0) continue;
            uint8_t level = r[kbd ? 2 : 3];
            Obs o;
            memset(&o, 0, sizeof(o));
            wfmt(o.id, 64, L"rongyuan:%04x:%04x:%s", c->vid, c->pid, kbd ? L"kb" : L"ms");
            if (c->vid == 0x3151 && c->pid == 0x402D && !kbd) {
                wcpy(o.name, 96, L"Attack Shark R3");
            } else if (generic) {
                wcpy(o.name, 96, kbd ? L"Wireless keyboard" : L"Wireless mouse");
            } else {
                wcpy(o.name, 96, product);
            }
            wcpy(o.via, 32, L"2.4 GHz");
            o.kind = kbd ? KIND_KEYBOARD : KIND_MOUSE;
            o.has_kind = true;
            if (level >= 1 && level <= 100) {
                o.state = STATE_BATTERY;
                o.percent = level;
            } else {
                o.state = STATE_ASLEEP;
                o.percent = -1;
            }
            o.vid = c->vid;
            o.pid = c->pid;
            scan_ctx_add(ctx, &o);
        }
        scan_ctx_claim(ctx, c->vid, c->pid);
    }
}
