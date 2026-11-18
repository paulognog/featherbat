#include "drivers.h"

static const uint16_t kKysonaVids[] = { 0x3554, 0x373B, 0x3770, 0x33E4, 0x3710, 0x35BB };

static bool is_kysona_vid(uint16_t vid) {
    for (size_t i = 0; i < ARRAYSIZE(kKysonaVids); i++) {
        if (kKysonaVids[i] == vid) return true;
    }
    return false;
}

static bool is_atk_serial(uint16_t vid, uint16_t pid) {
    return vid == 0x373B && (pid == 0x11CA || pid == 0x11CB || pid == 0x1203 || pid == 0x1204);
}

static bool is_redragon(uint16_t vid, uint16_t pid) {
    return vid == 0x3554 && (pid == 0xF55D || pid == 0xF5D5 || pid == 0xF55E);
}

static bool is_vxe_r1(uint16_t vid, uint16_t pid) {
    return vid == 0x3554 && (pid == 0xF58A || pid == 0xF58C || pid == 0xF58E ||
                             pid == 0xF58F || pid == 0xF505 || pid == 0xF506);
}

static bool is_cable(uint16_t vid, uint16_t pid) {
    bool hecate = vid == 0x35BB && (pid == 0xD3FA || pid == 0xD3FC || pid == 0xD3FD);
    bool r = vid == 0x3554 && (pid == 0xF55E || pid == 0xF58C || pid == 0xF58F || pid == 0xF506);
    return hecate || r;
}

uint8_t kysona_checksum(const uint8_t* b16) {
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) sum += b16[i];
    return (uint8_t)(0x55 - sum);
}

static bool query_kysona(HidDev* d, uint8_t cmd, uint8_t* r, int rcap) {
    uint8_t req[17];
    memset(req, 0, sizeof(req));
    req[0] = 0x08;
    req[1] = cmd;
    req[16] = kysona_checksum(req);
    hid_flush(d);
    if (!hid_write(d, req, 17, 250)) return false;

    ULONGLONG end = GetTickCount64() + 450;
    for (;;) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0) return false;
        int n = hid_read_sync(d, r, rcap, (int)left);
        if (n < 17) return false;
        if (r[0] == 0x08 && r[1] == cmd && r[16] == kysona_checksum(r)) return true;
    }
}

bool drv_kysona_present(const HidList* hids) {
    if (!hids) return false;
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (is_kysona_vid(c->vid) && !is_atk_serial(c->vid, c->pid)) {
            if ((c->out_len == 17 && c->in_len == 17) ||
                (c->usage_page >= 0xFF00 && c->out_len >= 17 && c->in_len >= 17)) {
                return true;
            }
        }
    }
    return false;
}

static void clean_name(const wchar_t* raw, uint16_t vid, uint16_t pid, wchar_t* out, size_t cap) {
    if (is_redragon(vid, pid)) { wcpy(out, cap, L"Redragon wireless mouse"); return; }
    if (is_vxe_r1(vid, pid)) { wcpy(out, cap, L"VXE Dragonfly R1"); return; }
    if (vid == 0x3770) { wcpy(out, cap, L"Hitscan Hyperlight"); return; }
    if (vid == 0x35BB) { wcpy(out, cap, L"HECATE mouse"); return; }

    wcpy(out, cap, raw);
    static const wchar_t* sufs[] = { L" Dongle", L" Receiver", L" 2.4G", L" Wireless Receiver" };
    for (size_t i = 0; i < ARRAYSIZE(sufs); i++) {
        size_t nl = wlen(out), sl = wlen(sufs[i]);
        if (nl > sl && weqi(out + nl - sl, sufs[i])) {
            out[nl - sl] = 0;
            break;
        }
    }
    if (!out[0]) wcpy(out, cap, vid == 0x373B ? L"ATK wireless mouse" : L"Wireless mouse");
}

void drv_kysona_scan(ScanCtx* ctx) {
    if (!ctx->hids) return;
    // Try FF02:0002 collections first, then others
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ctx->hids->count; i++) {
            const HidColl* c = &ctx->hids->items[i];
            if (!is_kysona_vid(c->vid) || is_atk_serial(c->vid, c->pid)) continue;
            if (scan_ctx_is_claimed(ctx, c->vid, c->pid)) continue;
            bool match_coll = (c->out_len == 17 && c->in_len == 17) ||
                              (c->usage_page >= 0xFF00 && c->out_len >= 17 && c->in_len >= 17);
            if (!match_coll) continue;
            bool is_ff02 = (c->usage_page == 0xFF02 && c->usage == 0x0002);
            if ((pass == 0 && !is_ff02) || (pass == 1 && is_ff02)) continue;

            HidDev d;
            if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) continue;
            uint8_t r[64];
            bool answered = false, connected = false, asleep = false, charging = false;
            int pct = -1, mv = 0;
            if (query_kysona(&d, 0x03, r, sizeof(r))) {
                answered = true;
                connected = (r[6] != 0);
            }
            if ((!answered || connected) && query_kysona(&d, 0x04, r, sizeof(r))) {
                answered = true;
                connected = true;
                if (r[6] <= 100) pct = r[6];
                charging = (r[7] != 0);
                mv = ((int)r[8] << 8) | r[9];
            } else if (answered && connected) {
                asleep = true;
            }
            hid_close(&d);

            if (!answered) continue;
            scan_ctx_claim(ctx, c->vid, c->pid);
            if (!connected) continue;

            wchar_t raw_name[96], name[96];
            hid_product_name(c->path, raw_name, 96);
            clean_name(raw_name, c->vid, c->pid, name, 96);

            // Deduplicate if plugged in by cable + dongle at the same time
            bool dup = false;
            for (int k = 0; k < ctx->count; k++) {
                if (weq(ctx->obs[k].name, name)) {
                    if (charging) ctx->obs[k].charging = true;
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            Obs o;
            memset(&o, 0, sizeof(o));
            wfmt(o.id, 64, L"kysona:%04x:%04x", c->vid, c->pid);
            wcpy(o.name, 96, name);
            wcpy(o.via, 32, is_cable(c->vid, c->pid) ? L"USB" : L"2.4 GHz");
            o.kind = kind_from_hid(ctx->hids, c->vid, c->pid);
            if (o.kind == KIND_OTHER) o.kind = KIND_MOUSE;
            o.has_kind = true;
            o.state = asleep ? STATE_ASLEEP : (pct >= 0 ? STATE_BATTERY : STATE_ASLEEP);
            o.percent = pct;
            o.millivolts = mv;
            o.charging = charging;
            o.grace_ms = 1800000u;
            o.vid = c->vid;
            o.pid = c->pid;
            scan_ctx_add(ctx, &o);
        }
    }
}
