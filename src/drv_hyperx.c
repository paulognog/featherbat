#include "drivers.h"

static bool hyperx_query(HidDev* d, const uint8_t* req, int req_len, uint8_t* r, int rcap,
                         int timeout_ms, bool (*match)(const uint8_t*, int, uint16_t), uint16_t ctx_arg) {
    if (!hid_write(d, req, req_len, 250)) return false;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)timeout_ms;
    for (;;) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0) return false;
        int n = hid_read_sync(d, r, rcap, (int)left);
        if (n < 0) return false;
        if (match(r, n, ctx_arg)) return true;
    }
}

static void init_hx(Obs* o, uint16_t vid, uint16_t pid, const wchar_t* name) {
    memset(o, 0, sizeof(*o));
    wfmt(o->id, 64, L"hyperx:%04x", pid);
    wcpy(o->name, 96, name);
    wcpy(o->via, 32, L"2.4 GHz");
    o->kind = KIND_HEADSET;
    o->has_kind = true;
    o->percent = -1;
    o->vid = vid;
    o->pid = pid;
}

static bool match_cloud3(const uint8_t* r, int n, uint16_t cmd) {
    return n >= 5 && r[0] == 0x66 && r[1] == (uint8_t)cmd;
}

static bool poll_cloud3(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[2] = { 0x66, 0x82 };
    if (!hyperx_query(&d, req, 2, r, sizeof(r), 800, match_cloud3, 0x82)) {
        hid_close(&d);
        return false;
    }
    const wchar_t* name = (c->pid == 0x069F || c->pid == 0x0995)
        ? L"HyperX Cloud II Core Wireless"
        : L"HyperX Cloud III Wireless";
    init_hx(o, c->vid, c->pid, name);
    if (r[2] != 0) {
        o->state = STATE_BATTERY;
        req[1] = 0x8A;
        if (hyperx_query(&d, req, 2, r, sizeof(r), 800, match_cloud3, 0x8A)) {
            o->charging = (r[2] == 1);
        }
        req[1] = 0x89;
        if (hyperx_query(&d, req, 2, r, sizeof(r), 800, match_cloud3, 0x89) && r[4] <= 100) {
            o->percent = r[4];
            o->millivolts = ((int)r[2] << 8) | r[3];
        } else {
            o->state = STATE_OFF;
        }
    } else {
        o->state = STATE_OFF;
    }
    hid_close(&d);
    return true;
}

static bool match_cloud2(const uint8_t* r, int n, uint16_t cmd) {
    return n >= 8 && r[0] == 0x06 && r[1] == 0xFF && r[2] == 0xBB && r[3] == (uint8_t)cmd;
}

static bool poll_cloud2(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[4] = { 0x06, 0xFF, 0xBB, 0x02 };
    if (!hyperx_query(&d, req, 4, r, sizeof(r), 900, match_cloud2, 0x02) || r[7] > 100) {
        hid_close(&d);
        return false;
    }
    init_hx(o, c->vid, c->pid, L"HyperX Cloud II Wireless");
    o->state = STATE_BATTERY;
    o->percent = r[7];
    o->millivolts = ((int)r[5] << 8) | r[6];
    req[3] = 0x03;
    if (hyperx_query(&d, req, 4, r, sizeof(r), 900, match_cloud2, 0x03)) {
        o->charging = (r[4] == 1);
    }
    hid_close(&d);
    return true;
}

static bool match_cloud2_kg(const uint8_t* r, int n, uint16_t cmd) {
    return n >= 8 && r[0] == 0x0B && r[2] == 0xBB && r[3] == (uint8_t)cmd;
}

static bool poll_cloud2_kg(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[17] = { 0x06, 0x00, 0x02, 0x00, 0x9A, 0x00, 0x00, 0x68, 0x4A, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0xBB, 0x02, 0x00 };
    if (!hyperx_query(&d, req, 17, r, sizeof(r), 900, match_cloud2_kg, 0x02) || r[7] > 100) {
        hid_close(&d);
        return false;
    }
    init_hx(o, c->vid, c->pid, L"HyperX Cloud II Wireless");
    o->state = STATE_BATTERY;
    o->percent = r[7];
    req[15] = 0x03;
    if (hyperx_query(&d, req, 17, r, sizeof(r), 900, match_cloud2_kg, 0x03)) {
        o->charging = (r[4] == 1);
    }
    hid_close(&d);
    return true;
}

static bool match_alpha(const uint8_t* r, int n, uint16_t cmd) {
    return n >= 4 && r[0] == 0x21 && r[2] == (uint8_t)cmd;
}

static bool poll_alpha(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[3] = { 0x21, 0xBB, 0x03 };
    if (!hyperx_query(&d, req, 3, r, sizeof(r), 900, match_alpha, 0x03)) {
        hid_close(&d);
        return false;
    }
    init_hx(o, c->vid, c->pid, L"HyperX Cloud Alpha Wireless");
    if (r[3] == 0x01) {
        o->state = STATE_OFF;
        hid_close(&d);
        return true;
    }
    o->state = STATE_BATTERY;
    req[2] = 0x0C;
    if (hyperx_query(&d, req, 3, r, sizeof(r), 900, match_alpha, 0x0C)) o->charging = (r[3] == 0x01);
    req[2] = 0x0B;
    if (hyperx_query(&d, req, 3, r, sizeof(r), 900, match_alpha, 0x0B) && r[3] <= 100) o->percent = r[3];
    else o->state = STATE_OFF;
    hid_close(&d);
    return true;
}

static bool match_alpha2(const uint8_t* r, int n, uint16_t exp) {
    return n >= 8 && r[0] == (uint8_t)(exp >> 8) && r[1] == (uint8_t)(exp & 0xFF);
}

static bool poll_alpha2(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[2] = { 0x52, 0x01 };
    if (!hyperx_query(&d, req, 2, r, sizeof(r), 900, match_alpha2, 0x5301)) {
        hid_close(&d);
        return false;
    }
    init_hx(o, c->vid, c->pid, L"HyperX Cloud Alpha 2 Wireless");
    if (r[2] == 0) {
        o->state = STATE_OFF;
        hid_close(&d);
        return true;
    }
    req[0] = 0x50; req[1] = 0x02;
    if (hyperx_query(&d, req, 2, r, sizeof(r), 900, match_alpha2, 0x5102) && r[2] <= 100) {
        o->state = STATE_BATTERY;
        o->percent = r[2];
        o->charging = (r[3] == 0x01);
        o->millivolts = r[6] | (r[7] << 8);
    } else {
        o->state = STATE_OFF;
    }
    hid_close(&d);
    return true;
}

static bool match_flight(const uint8_t* r, int n, uint16_t unused) {
    (void)r; (void)unused;
    return n >= 5;
}

static bool poll_flight(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[3] = { 0x21, 0xFF, 0x05 };
    if (!hyperx_query(&d, req, 3, r, sizeof(r), 900, match_flight, 0)) {
        hid_close(&d);
        return false;
    }
    hid_close(&d);
    int mv = ((int)r[3] << 8) | r[4];
    init_hx(o, c->vid, c->pid, L"HyperX Cloud Flight");
    o->millivolts = mv;
    if (mv > 0x100B) {
        o->charging = true;
        o->state = STATE_ASLEEP;
    } else {
        static const int kMv[]  = { 3975, 3920, 3870, 3825, 3785, 3750, 3715, 3680, 3648, 3500 };
        static const int kPct[] = { 100,  90,   80,   70,   60,   50,   35,   18,   5,    0 };
        o->percent = interpolate_curve(mv, kMv, kPct, 10);
        o->state = STATE_BATTERY;
    }
    return true;
}

static bool match_cloud3s(const uint8_t* r, int n, uint16_t cmd) {
    return n >= 7 && r[0] == 0x0C && r[5] == (uint8_t)cmd && r[6] != 0xFF;
}

static bool poll_cloud3s(const HidColl* c, Obs* o) {
    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    uint8_t r[64];
    uint8_t req[6] = { 0x0C, 0x02, 0x03, 0x01, 0x00, 0x02 };
    if (!hyperx_query(&d, req, 6, r, sizeof(r), 900, match_cloud3s, 0x02)) {
        hid_close(&d);
        return false;
    }
    init_hx(o, c->vid, c->pid, L"HyperX Cloud III S Wireless");
    if (r[6] != 2) {
        o->state = STATE_OFF;
        hid_close(&d);
        return true;
    }
    o->state = STATE_BATTERY;
    req[5] = 0x06;
    if (hyperx_query(&d, req, 6, r, sizeof(r), 900, match_cloud3s, 0x06) && r[6] <= 100) o->percent = r[6];
    else o->state = STATE_OFF;
    req[5] = 0x48;
    if (hyperx_query(&d, req, 6, r, sizeof(r), 900, match_cloud3s, 0x48)) o->charging = (r[6] == 1 || r[6] == 2);
    hid_close(&d);
    return true;
}

static bool poll_stinger_core(const HidColl* c, Obs* o) {
    static const uint8_t kConn[20] = {
        0xFF, 0x07, 0x00, 0xFD, 0x04, 0x00, 0x00, 0x02, 0x82, 0x03,
        0x05, 0x29, 0x0F, 0x00, 0x00, 0x03, 0x68, 0x10, 0x00, 0x00
    };
    static const uint8_t kBat[12] = { 0xFF, 0x07, 0x00, 0xFD, 0x04, 0x0C, 0xF1, 0x02, 0x01, 0x04, 0xF0, 0x0C };
    static const int kPct[]  = { 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0 };
    static const int kChg[]  = { 4130, 4090, 4050, 4000, 3967, 3946, 3930, 3916, 3871, 3831, 3400 };
    static const int kDis[]  = { 4121, 4042, 3954, 3905, 3838, 3785, 3763, 3738, 3700, 3651, 3517 };

    HidDev d;
    if (!hid_open(&d, c, GENERIC_READ | GENERIC_WRITE, true)) return false;
    bool ps = (c->pid == 0x0A93);
    uint8_t r[64];
    int rlen = 0;

    if (!hid_set_feature(&d, kConn, sizeof(kConn))) { hid_close(&d); return false; }
    bool got_conn = false;
    for (int i = 0; i < (ps ? 10 : 1); i++) {
        Sleep(20);
        if (!hid_get_feature(&d, 0xFF, r, sizeof(r), &rlen) || rlen < 13) break;
        if (!ps || !(r[1] == 0x01 && r[2] == 0x00)) { got_conn = true; break; }
    }
    if (!got_conn && !(ps && rlen >= 13)) { hid_close(&d); return false; }

    init_hx(o, c->vid, c->pid, L"HyperX Cloud Stinger Core Wireless");
    if (r[11] != 0x01) {
        o->state = STATE_OFF;
        hid_close(&d);
        return true;
    }

    if (hid_set_feature(&d, kBat, sizeof(kBat))) {
        for (int i = 0; i < (ps ? 10 : 1); i++) {
            Sleep(20);
            if (!hid_get_feature(&d, 0xFF, r, sizeof(r), &rlen) || rlen < 13) break;
            if (!ps || (r[0] == 0xFF && r[1] == 0x12 && r[2] == 0x05)) {
                uint8_t code = r[9];
                if (code == 0x03 || code == 0x05 || code == 0x06) {
                    o->millivolts = r[11] | (r[12] << 8);
                    o->charging = (code == 0x05);
                    o->percent = (code == 0x06) ? 100
                        : interpolate_curve(o->millivolts, code == 0x05 ? kChg : kDis, kPct, 11);
                    o->state = STATE_BATTERY;
                }
                break;
            }
        }
    }
    hid_close(&d);
    return true;
}

static bool match_hx_model(const HidColl* c, int* kind_idx) {
    if (c->vid == 0x03F0 && (c->pid == 0x05B7 || c->pid == 0x0C9D)) {
        if (c->out_len == 62 && c->in_len == 62) { *kind_idx = 0; return true; }
    } else if (c->vid == 0x03F0 && (c->pid == 0x069F || c->pid == 0x0995)) {
        if (c->usage_page >= 0xFF00 && c->out_len >= 62 && c->in_len >= 62) { *kind_idx = 0; return true; }
    } else if (c->vid == 0x03F0 && (c->pid == 0x06BE || c->pid == 0x02CC)) {
        if (c->out_len == 64 && c->in_len == 64) { *kind_idx = 6; return true; }
    } else if (c->vid == 0x03F0 && (c->pid == 0x0696 || c->pid == 0x018B || c->pid == 0x0D93)) {
        if (c->usage_page == 0xFF90 && c->usage == 0x0303) { *kind_idx = 1; return true; }
    } else if (c->vid == 0x0951 && c->pid == 0x1718) {
        if (c->out_len == 62) { *kind_idx = 2; return true; }
    } else if (c->vid == 0x03F0 && c->pid == 0x098D) {
        if (c->usage_page >= 0xFF00 && c->out_len >= 3) { *kind_idx = 3; return true; }
    } else if (c->vid == 0x03F0 && c->pid == 0x08BE) {
        if (c->usage_page == 0xFF13 && c->usage == 0xFF00) { *kind_idx = 4; return true; }
    } else if ((c->vid == 0x0951 && (c->pid == 0x16C4 || c->pid == 0x1723 || c->pid == 0x1749)) ||
               (c->vid == 0x03F0 && c->pid == 0x0E90)) {
        if (c->usage_page >= 0xFF00 && c->out_len >= 3) { *kind_idx = 5; return true; }
    } else if ((c->vid == 0x03F0 && c->pid == 0x0A93) || (c->vid == 0x0951 && c->pid == 0x170B)) {
        if (c->usage_page == 0xFF00 && c->usage == 0x0001 && c->feat_len >= 32) { *kind_idx = 7; return true; }
    }
    return false;
}

bool drv_hyperx_present(const HidList* hids) {
    if (!hids) return false;
    int k = 0;
    for (int i = 0; i < hids->count; i++) {
        if (match_hx_model(&hids->items[i], &k)) return true;
    }
    return false;
}

void drv_hyperx_scan(ScanCtx* ctx) {
    if (!ctx->hids) return;
    for (int i = 0; i < ctx->hids->count; i++) {
        const HidColl* c = &ctx->hids->items[i];
        if (scan_ctx_is_claimed(ctx, c->vid, c->pid)) continue;
        int k = -1;
        if (!match_hx_model(c, &k)) continue;
        Obs o;
        bool ok = false;
        switch (k) {
            case 0: ok = poll_cloud3(c, &o); break;
            case 1: ok = poll_cloud2(c, &o); break;
            case 2: ok = poll_cloud2_kg(c, &o); break;
            case 3: ok = poll_alpha(c, &o); break;
            case 4: ok = poll_alpha2(c, &o); break;
            case 5: ok = poll_flight(c, &o); break;
            case 6: ok = poll_cloud3s(c, &o); break;
            case 7: ok = poll_stinger_core(c, &o); break;
        }
        if (ok) {
            scan_ctx_add(ctx, &o);
            scan_ctx_claim(ctx, c->vid, c->pid);
        }
    }
}
