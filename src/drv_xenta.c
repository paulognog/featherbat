#include "drivers.h"
#include <hidsdi.h>

#define XENTA_VID 0x1D57
#define MAX_XENTA 4

typedef struct {
    uint8_t code;
    const wchar_t* name;
    bool tens;
} XentaModel;

static const XentaModel kXentaModels[] = {
    { 0x3D, L"Delux M900 Pro", false },
    { 0x55, L"Attack Shark X11", false },
    { 0x4E, L"Attack Shark X3 Max", false },
    { 0xE5, L"Attack Shark V3 Pro", false },
    { 0x10, L"Attack Shark X6 / R1", true },
};

typedef struct {
    bool active;
    wchar_t path[260];
    uint16_t pid;
    HidDev dev;
    uint8_t last[5];
    uint8_t prev[5];
    bool has_last;
    bool has_prev;
    bool settled;
    ULONGLONG last_frame_at;
} XentaEntry;

static XentaEntry g_xenta[MAX_XENTA];

static bool is_xenta_pid(uint16_t pid) {
    return pid == 0xFA60 || pid == 0xFA65 || pid == 0xFA55 || pid == 0x2155;
}

static bool frames_agree(const XentaEntry* x) {
    return x->has_prev && x->has_last && memcmp(x->prev, x->last, 5) == 0;
}

static bool feed_frame(XentaEntry* x, const uint8_t* r, int len) {
    if (len < 5 || r[0] != 0x03 || r[2] != 0x40) return false;
    bool changed = !x->has_last || memcmp(x->last, r, 5) != 0;
    memcpy(x->prev, x->last, 5);
    x->has_prev = x->has_last;
    memcpy(x->last, r, 5);
    x->has_last = true;
    x->last_frame_at = GetTickCount64();
    bool was_settled = x->settled;
    if (frames_agree(x) || x->last[3] == 0x02 || x->last[3] == 0x03) {
        x->settled = true;
    }
    return changed || (!was_settled && x->settled);
}

static void close_xenta(XentaEntry* x) {
    hid_close(&x->dev);
    memset(x, 0, sizeof(*x));
}

void drv_xenta_sync(const HidList* hids, bool once_mode) {
    bool seen[MAX_XENTA];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid != XENTA_VID || !is_xenta_pid(c->pid)) continue;
        if (c->usage_page != 0x000A || c->in_len < 5) continue;

        int match = -1;
        for (int k = 0; k < MAX_XENTA; k++) {
            if (g_xenta[k].active && weqi(g_xenta[k].path, c->path)) { match = k; break; }
        }
        if (match >= 0) {
            seen[match] = true;
            continue;
        }

        int slot = -1;
        for (int k = 0; k < MAX_XENTA; k++) {
            if (!g_xenta[k].active) { slot = k; break; }
        }
        if (slot < 0) continue;

        XentaEntry* x = &g_xenta[slot];
        memset(x, 0, sizeof(*x));
        if (!hid_open(&x->dev, c, GENERIC_READ, true)) continue;
        HidD_SetNumInputBuffers(x->dev.h, 64);
        wcpy(x->path, 260, c->path);
        x->pid = c->pid;
        x->active = true;
        seen[slot] = true;

        if (once_mode) {
            ULONGLONG until = GetTickCount64() + 5000;
            uint8_t r[32];
            int got = 0;
            while (got < 2) {
                LONGLONG rem = (LONGLONG)until - (LONGLONG)GetTickCount64();
                if (rem <= 0) break;
                int n = hid_read_sync(&x->dev, r, sizeof(r), (int)rem);
                if (n < 5) break;
                if (feed_frame(x, r, n)) got++;
            }
        } else {
            hid_start_async_read(&x->dev);
        }
    }

    for (int k = 0; k < MAX_XENTA; k++) {
        if (g_xenta[k].active && !seen[k]) close_xenta(&g_xenta[k]);
    }
}

int drv_xenta_wait_handles(HANDLE* out, int max_count) {
    int n = 0;
    for (int k = 0; k < MAX_XENTA && n < max_count; k++) {
        XentaEntry* x = &g_xenta[k];
        if (x->active && x->dev.alive && x->dev.read_pending) {
            out[n++] = x->dev.ov.hEvent;
        }
    }
    return n;
}

bool drv_xenta_on_io_ready(void) {
    bool changed = false;
    uint8_t r[32];
    for (int k = 0; k < MAX_XENTA; k++) {
        XentaEntry* x = &g_xenta[k];
        if (!x->active || !x->dev.read_pending) continue;
        bool io_err = false;
        int n = hid_poll_async_read(&x->dev, r, sizeof(r), &io_err);
        if (io_err) {
            close_xenta(x);
            changed = true;
            continue;
        }
        if (n >= 5 && feed_frame(x, r, n)) changed = true;
        if (x->dev.alive) hid_start_async_read(&x->dev);
    }
    return changed;
}

bool drv_xenta_drain_all(void) {
    bool changed = false;
    uint8_t r[32];
    for (int k = 0; k < MAX_XENTA; k++) {
        XentaEntry* x = &g_xenta[k];
        if (!x->active || !x->dev.alive || x->dev.read_pending) continue;
        for (int tries = 0; tries < 64; tries++) {
            int n = hid_read_sync(&x->dev, r, sizeof(r), 0);
            if (n < 5) break;
            if (feed_frame(x, r, n)) changed = true;
        }
    }
    return changed;
}

void drv_xenta_emit(ScanCtx* ctx) {
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < MAX_XENTA; k++) {
        XentaEntry* x = &g_xenta[k];
        if (!x->active) continue;
        if (!x->has_last || (now - x->last_frame_at) > 15000ULL) continue;

        bool cable = (x->pid == 0xFA55 || x->pid == 0x2155);
        uint8_t code = x->last[1], st = x->last[3], level = x->last[4];
        const XentaModel* model = NULL;
        for (size_t i = 0; i < ARRAYSIZE(kXentaModels); i++) {
            if (kXentaModels[i].code == code) { model = &kXentaModels[i]; break; }
        }

        Obs o;
        memset(&o, 0, sizeof(o));
        wfmt(o.id, 64, L"xenta:%04x", x->pid);
        wcpy(o.name, 96, model ? model->name : L"Wireless mouse");
        wcpy(o.via, 32, cable ? L"USB" : L"2.4 GHz");
        o.kind = KIND_MOUSE;
        o.has_kind = true;
        o.percent = -1;
        o.grace_ms = 1800000u;
        o.vid = XENTA_VID;
        o.pid = x->pid;

        bool agreed = frames_agree(x);
        if (cable || st == 0x02 || st == 0x03) {
            o.charging = true;
            o.state = STATE_BATTERY;
            if (st == 0x02 && !cable) {
                o.percent = 100;
            } else if (!cable) {
                if (model && model->tens) {
                    if (level >= 1 && level <= 10) o.percent = level * 10;
                } else if (level >= 1 && level <= 100) {
                    o.percent = level;
                }
            }
        } else if (agreed) {
            if (model && model->tens) {
                if (level >= 1 && level <= 10) o.percent = level * 10;
            } else if (st == 0x01 && level >= 1 && level <= 100) {
                o.percent = level;
            }
            o.state = (o.percent >= 0) ? STATE_BATTERY : STATE_ASLEEP;
        } else {
            o.state = STATE_ASLEEP;
        }

        scan_ctx_add(ctx, &o);
        scan_ctx_claim(ctx, XENTA_VID, x->pid);
    }
}

void drv_xenta_close_all(void) {
    for (int k = 0; k < MAX_XENTA; k++) {
        if (g_xenta[k].active) close_xenta(&g_xenta[k]);
    }
}

void drv_xenta_dump(const HidList* hids, WBuf* out) {
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid != XENTA_VID || c->usage_page != 0x000A || c->in_len < 5) continue;
        wb_fmt(out, L"\nXenta %04X:%04X status frames (6 s):\n", c->vid, c->pid);
        HidDev d;
        if (!hid_open(&d, c, GENERIC_READ, true)) {
            wb_fmt(out, L"  cannot open (error %u)\n", (unsigned)GetLastError());
            continue;
        }
        uint8_t r[32];
        int frames = 0;
        ULONGLONG end = GetTickCount64() + 6000;
        while (frames < 16) {
            LONGLONG rem = (LONGLONG)end - (LONGLONG)GetTickCount64();
            if (rem <= 0) break;
            int n = hid_read_sync(&d, r, sizeof(r), (int)rem);
            if (n < 0) break;
            wb_puts(out, L" ");
            for (int b = 0; b < n; b++) wb_fmt(out, L" %02X", r[b]);
            wb_putc(out, L'\n');
            frames++;
        }
        if (frames == 0) wb_puts(out, L"  none (mouse off, asleep or on Bluetooth?)\n");
        hid_close(&d);
    }
}
