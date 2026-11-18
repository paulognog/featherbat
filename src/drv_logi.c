#include "drivers.h"

#define LOGI_VID 0x046D
#define LOGI_G435_PID 0x0ADF
#define LOGI_SW_ID 0x0B

#define FEAT_FW_VERSION     0x0003
#define FEAT_DEVICE_NAME    0x0005
#define FEAT_BATTERY_STATUS 0x1000
#define FEAT_BATTERY_VOLT   0x1001
#define FEAT_UNIFIED_BAT    0x1004
#define FEAT_ADC_MEAS       0x1F20

#define MAX_LOGI_EPS   8
#define MAX_LOGI_SLOTS 7
#define MAX_GHUB_CURVES 24
#define MAX_CURVE_PTS   96

typedef struct {
    uint16_t mv;
    uint64_t used_e3; // used_energy * 1000
} CurvePt;

typedef struct {
    uint16_t pid;
    int count;
    CurvePt pts[MAX_CURVE_PTS];
    uint64_t total_e3;
} GhubCurve;

static GhubCurve g_curves[MAX_GHUB_CURVES];
static int       g_curve_count = 0;
static bool      g_curves_loaded = false;

static bool parse_dec_scaled(const char** p, uint64_t scale, uint64_t* out) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    uint64_t ip = 0;
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (uint64_t)(*s - '0');
        s++;
    }
    uint64_t fp = 0;
    uint64_t div = 1;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (div < scale) {
                fp = fp * 10 + (uint64_t)(*s - '0');
                div *= 10;
            }
            s++;
        }
    }
    while (div < scale) { fp *= 10; div *= 10; }
    *out = ip * scale + fp;
    *p = s;
    return true;
}

static const char* find_ascii(const char* hay, size_t hay_len, const char* needle) {
    size_t nl = alen(needle);
    if (!nl || hay_len < nl) return NULL;
    for (size_t i = 0; i + nl <= hay_len; i++) {
        size_t j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return hay + i;
    }
    return NULL;
}

static void parse_ghub_xml(const wchar_t* path) {
    if (g_curve_count >= MAX_GHUB_CURVES) return;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 262144) { CloseHandle(h); return; }
    char* buf = (char*)xalloc(sz + 1);
    if (!buf) { CloseHandle(h); return; }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sz, &got, NULL);
    CloseHandle(h);
    if (!ok || got == 0) { xfree(buf); return; }
    buf[got] = 0;

    const char* pm = find_ascii(buf, got, "<powermodel");
    if (!pm) { xfree(buf); return; }
    const char* pid_attr = find_ascii(pm, (size_t)(buf + got - pm), "pid=\"0x");
    if (!pid_attr) { xfree(buf); return; }
    uint32_t pid_u = 0;
    if (!aparse_u32_hex(pid_attr + 7, &pid_u) || pid_u == 0 || pid_u > 0xFFFF) { xfree(buf); return; }

    const char* dis = find_ascii(buf, got, "<discharge");
    if (!dis) { xfree(buf); return; }
    const char* cdata = find_ascii(dis, (size_t)(buf + got - dis), "<![CDATA[");
    if (!cdata) { xfree(buf); return; }
    cdata += 9;
    const char* cend = find_ascii(cdata, (size_t)(buf + got - cdata), "]]>");
    if (!cend) { xfree(buf); return; }

    GhubCurve c;
    memset(&c, 0, sizeof(c));
    c.pid = (uint16_t)pid_u;
    const char* p = cdata;
    while (p < cend && c.count < MAX_CURVE_PTS) {
        while (p < cend && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= cend) break;
        const char* line_end = p;
        while (line_end < cend && *line_end != '\n') line_end++;
        // time,volts,used_energy
        uint64_t dummy_t = 0, mv_val = 0, used_e3 = 0;
        const char* q = p;
        if (parse_dec_scaled(&q, 1, &dummy_t)) {
            while (q < line_end && *q != ',') q++;
            if (q < line_end && *q == ',') {
                q++;
                if (parse_dec_scaled(&q, 1000, &mv_val)) {
                    while (q < line_end && *q != ',') q++;
                    if (q < line_end && *q == ',') {
                        q++;
                        if (parse_dec_scaled(&q, 1000, &used_e3) && mv_val > 0 && mv_val < 10000) {
                            c.pts[c.count].mv = (uint16_t)mv_val;
                            c.pts[c.count].used_e3 = used_e3;
                            c.count++;
                        }
                    }
                }
            }
        }
        p = line_end + 1;
    }
    xfree(buf);
    if (c.count >= 2 && c.pts[c.count - 1].used_e3 > 0) {
        c.total_e3 = c.pts[c.count - 1].used_e3;
        g_curves[g_curve_count++] = c;
    }
}

static void collect_ghub_dir(const wchar_t* dir, int depth) {
    if (depth > 3 || g_curve_count >= MAX_GHUB_CURVES) return;
    wchar_t* pat = (wchar_t*)xalloc(520 * sizeof(wchar_t));
    if (!pat) return;
    wfmt(pat, 520, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    xfree(pat);
    if (hf == INVALID_HANDLE_VALUE) return;

    wchar_t* sub = (wchar_t*)xalloc(520 * sizeof(wchar_t));
    if (sub) {
        do {
            if (weq(fd.cFileName, L".") || weq(fd.cFileName, L"..")) continue;
            wfmt(sub, 520, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                collect_ghub_dir(sub, depth + 1);
            } else if (wcontains_i(fd.cFileName, L"battery") && wends(fd.cFileName, L".xml")) {
                parse_ghub_xml(sub);
            }
        } while (FindNextFileW(hf, &fd) && g_curve_count < MAX_GHUB_CURVES);
        xfree(sub);
    }
    FindClose(hf);
}

static void ensure_curves_loaded(void) {
    if (g_curves_loaded) return;
    g_curves_loaded = true;
    collect_ghub_dir(L"C:\\ProgramData\\LGHUB\\depots", 0);
}

int logi_liion_percent(int mv) {
    static const int v[] = { 4186, 4067, 3989, 3922, 3859, 3811, 3778, 3751, 3717, 3671, 3646, 3579, 3500 };
    static const int p[] = { 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 2, 0 };
    return interpolate_curve(mv, v, p, 13);
}

int logi_headset_percent(uint16_t pid, int mv) {
    static const int g533v[] = { 4200, 3850, 3790, 3750, 3680, 3330 }, g533p[] = { 100, 50, 30, 20, 5, 0 };
    static const int g535v[] = { 4175, 3817, 3766, 3730, 3664, 3310 }, g535p[] = { 100, 50, 30, 20, 5, 0 };
    static const int g633v[] = { 4100, 3950, 3850, 3750, 3650, 3500, 3300, 3150 }, g633p[] = { 100, 80, 60, 40, 20, 10, 5, 0 };
    static const int gprov[] = { 4150, 3830, 3780, 3740, 3670, 3320 }, gprop[] = { 100, 50, 30, 20, 5, 0 };
    static const int defv[]  = { 4100, 3850, 3780, 3730, 3650, 3300 }, defp[]  = { 100, 50, 30, 20, 5, 0 };
    switch (pid) {
        case 0x0A66: return interpolate_curve(mv, g533v, g533p, 6);
        case 0x0AC4: return interpolate_curve(mv, g535v, g535p, 6);
        case 0x0A5C: case 0x0A89: case 0x0A5B: case 0x0A87: case 0x0AB5: case 0x0AFE:
            return interpolate_curve(mv, g633v, g633p, 8);
        case 0x0AA7: case 0x0AAA: case 0x0ABA: case 0x0AFB:
            return interpolate_curve(mv, gprov, gprop, 6);
        default:
            return interpolate_curve(mv, defv, defp, 6);
    }
}

static int curve_percent(const uint16_t* pids, int npids, int mv) {
    ensure_curves_loaded();
    for (int i = 0; i < npids; i++) {
        for (int c = 0; c < g_curve_count; c++) {
            if (g_curves[c].pid == pids[i]) {
                const GhubCurve* gc = &g_curves[c];
                if (mv >= gc->pts[0].mv) return 100;
                uint64_t used = gc->total_e3;
                for (int k = 0; k < gc->count; k++) {
                    if (gc->pts[k].mv <= mv) {
                        used = gc->pts[k].used_e3;
                        break;
                    }
                }
                if (used >= gc->total_e3) return 0;
                uint64_t rem = gc->total_e3 - used;
                return clamp100((int)((rem * 100 + gc->total_e3 / 2) / gc->total_e3));
            }
        }
    }
    return logi_liion_percent(mv);
}

typedef struct {
    bool active;
    bool known;
    uint8_t dev_idx;
    wchar_t id[64];
    wchar_t name[96];
    wchar_t unit_id[16];
    DeviceKind kind;
    uint16_t model_pids[4];
    int model_pid_count;
    uint16_t bat_feat_id;
    uint8_t  bat_feat_idx;
    bool     hidpp10;
    DeviceState state;
    int percent;
    int millivolts;
    wchar_t level_text[16];
    bool charging;
    uint64_t last_query_ms;
} LogiSlot;

typedef struct {
    bool active;
    wchar_t phys_key[128];
    wchar_t product[96];
    wchar_t container_id[40];
    uint16_t pid;
    bool is_receiver;
    bool is_bluetooth;
    bool is_headset_page;
    HidDev s_dev;
    HidDev l_dev;
    LogiSlot slots[MAX_LOGI_SLOTS];
    int slot_count;
} LogiEp;

static LogiEp g_eps[MAX_LOGI_EPS];

// G435 (046D:0ADF) state
static Device g_g435_dev;
static bool   g_g435_present = false;
static bool   g_g435_has_reading = false;

static void cancel_ep_async(LogiEp* ep) {
    if (ep->l_dev.alive && ep->l_dev.read_pending) {
        CancelIoEx(ep->l_dev.h, &ep->l_dev.ov);
        DWORD d = 0; GetOverlappedResult(ep->l_dev.h, &ep->l_dev.ov, &d, TRUE);
        ep->l_dev.read_pending = false;
    }
    if (ep->s_dev.alive && ep->s_dev.read_pending) {
        CancelIoEx(ep->s_dev.h, &ep->s_dev.ov);
        DWORD d = 0; GetOverlappedResult(ep->s_dev.h, &ep->s_dev.ov, &d, TRUE);
        ep->s_dev.read_pending = false;
    }
}

static void arm_ep_async(LogiEp* ep) {
    if (ep->l_dev.alive && !ep->l_dev.read_pending) hid_start_async_read(&ep->l_dev);
    if (ep->s_dev.alive && !ep->s_dev.read_pending) hid_start_async_read(&ep->s_dev);
}

static void close_ep(LogiEp* ep) {
    hid_close(&ep->l_dev);
    hid_close(&ep->s_dev);
    memset(ep, 0, sizeof(*ep));
}

static bool is_known_receiver_pid(uint16_t pid) {
    static const uint16_t pids[] = {
        0xC517, 0xC518, 0xC51A, 0xC51B, 0xC521, 0xC525, 0xC526, 0xC52B,
        0xC52E, 0xC52F, 0xC531, 0xC532, 0xC534, 0xC537, 0xC539, 0xC53A,
        0xC53D, 0xC53F, 0xC541, 0xC545, 0xC547, 0xC548, 0xC54D, 0xC54F,
    };
    for (size_t i = 0; i < ARRAYSIZE(pids); i++) {
        if (pids[i] == pid) return true;
    }
    return false;
}

static void physical_key(const wchar_t* path, wchar_t* out, size_t cap) {
    out[0] = 0;
    wchar_t tmp[260];
    wcpy(tmp, 260, path);
    wto_lower(tmp);
    const wchar_t* a = wfind_i(tmp, L"#");
    const wchar_t* b = a ? wfind_i(a + 1, L"#") : NULL;
    const wchar_t* e = b ? wfind_i(b + 1, L"#") : NULL;
    if (!e) { wcpy(out, cap, tmp); return; }

    wchar_t hw[128], inst[128];
    size_t hw_len = (size_t)(b - (a + 1));
    if (hw_len >= 128) hw_len = 127;
    memcpy(hw, a + 1, hw_len * sizeof(wchar_t));
    hw[hw_len] = 0;

    // Strip "&colNN"
    wchar_t* col = (wchar_t*)wfind_i(hw, L"&col");
    if (col && wlen(col) >= 6) {
        wcpy(col, 128 - (size_t)(col - hw), col + 6);
    }

    size_t inst_len = (size_t)(e - (b + 1));
    if (inst_len >= 128) inst_len = 127;
    memcpy(inst, b + 1, inst_len * sizeof(wchar_t));
    inst[inst_len] = 0;
    // Strip trailing "&NNNN"
    wchar_t* last_amp = NULL;
    for (wchar_t* q = inst; *q; q++) {
        if (*q == L'&') last_amp = q;
    }
    if (last_amp) *last_amp = 0;
    wfmt(out, cap, L"%s#%s", hw, inst);
}

static DeviceKind kind_from_logi_type(uint8_t t) {
    switch (t) {
        case 0x00: case 0x02: return KIND_KEYBOARD;
        case 0x03: case 0x04: case 0x05: return KIND_MOUSE;
        case 0x08: return KIND_HEADSET;
        case 0x0B: case 0x0C: return KIND_GAMEPAD;
        default: return KIND_OTHER;
    }
}

static void apply_battery_payload(LogiEp* ep, LogiSlot* sl, uint16_t feat_id, const uint8_t* p, int plen) {
    if (plen < 3) return;
    if (feat_id == FEAT_UNIFIED_BAT) {
        int soc = p[0], level = p[1], st = p[2];
        sl->level_text[0] = 0;
        if (soc > 0 && soc <= 100) {
            sl->percent = soc;
        } else if (level) {
            sl->percent = (level & 8) ? 100 : (level & 4) ? 60 : (level & 2) ? 20 : 5;
            wcpy(sl->level_text, 16, (level & 8) ? L"Full" : (level & 4) ? L"Good" : (level & 2) ? L"Low" : L"Critical");
        }
        sl->charging = (st == 1 || st == 2 || st == 3);
        sl->state = STATE_BATTERY;
    } else if (feat_id == FEAT_BATTERY_STATUS) {
        int st = p[2];
        sl->level_text[0] = 0;
        sl->charging = (st >= 1 && st <= 4);
        sl->percent = p[0] <= 100 ? p[0] : -1;
        if (sl->percent == 0 && st == 3) sl->percent = 100;
        sl->state = (sl->percent >= 0) ? STATE_BATTERY : STATE_ASLEEP;
    } else if (feat_id == FEAT_BATTERY_VOLT) {
        int mv = ((int)p[0] << 8) | p[1];
        sl->millivolts = mv;
        sl->charging = (p[2] & 0x80) != 0;
        sl->level_text[0] = 0;
        if (mv > 0) sl->percent = curve_percent(sl->model_pids, sl->model_pid_count, mv);
        sl->state = (sl->percent >= 0) ? STATE_BATTERY : STATE_ASLEEP;
    } else if (feat_id == FEAT_ADC_MEAS) {
        if ((p[2] & 0x01) == 0) {
            sl->state = STATE_OFF;
            return;
        }
        int mv = ((int)p[0] << 8) | p[1];
        sl->millivolts = mv;
        sl->charging = (p[2] & 0x02) != 0;
        sl->level_text[0] = 0;
        if (mv > 0) sl->percent = logi_headset_percent(ep->pid, mv);
        sl->state = (sl->percent >= 0) ? STATE_BATTERY : STATE_ASLEEP;
    }
}

static bool handle_unsolicited_packet(LogiEp* ep, const uint8_t* r, int len) {
    if (len < 6 || (r[0] != 0x10 && r[0] != 0x11)) return false;
    uint8_t idx = r[1];
    // HID++ 1.0 Device Connection notification (0x41)
    if (r[2] == 0x41 && len >= 5) {
        for (int i = 0; i < ep->slot_count; i++) {
            LogiSlot* sl = &ep->slots[i];
            if (sl->dev_idx != idx) continue;
            bool link_lost = (r[4] & 0x40) != 0;
            if (link_lost) {
                if (sl->known && sl->state == STATE_BATTERY) {
                    sl->state = STATE_ASLEEP;
                    return true;
                }
            } else {
                // Device woke up! Will be queried on next step if needed
                sl->last_query_ms = 0;
                return true;
            }
        }
        return false;
    }
    // HID++ 2.0 unsolicited broadcast (sw_id == 0, fn == 0)
    if ((r[3] & 0x0F) == 0 && (r[3] >> 4) == 0) {
        for (int i = 0; i < ep->slot_count; i++) {
            LogiSlot* sl = &ep->slots[i];
            if (sl->dev_idx == idx && sl->bat_feat_idx != 0 && r[2] == sl->bat_feat_idx) {
                int old_pct = sl->percent;
                bool old_chg = sl->charging;
                DeviceState old_st = sl->state;
                apply_battery_payload(ep, sl, sl->bat_feat_id, r + 4, len - 4);
                sl->last_query_ms = GetTickCount64();
                return (sl->percent != old_pct || sl->charging != old_chg || sl->state != old_st);
            }
        }
    }
    return false;
}

// Synchronous request while async reads on ep are NOT pending.
// Returns 1 = ok (params in `out` 16 bytes), 0 = protocol error (`err`), -1 = timeout/io error
static int hidpp_request(LogiEp* ep, uint8_t idx, uint8_t b2, uint8_t b3,
                         const uint8_t* params, int plen, uint8_t* out16, uint8_t* err,
                         bool force_short, int timeout_ms) {
    bool use_long = ep->l_dev.alive && !force_short;
    if (!use_long && !ep->s_dev.alive) return -1;
    uint8_t msg[20];
    memset(msg, 0, sizeof(msg));
    msg[0] = use_long ? 0x11 : 0x10;
    msg[1] = idx;
    msg[2] = b2;
    msg[3] = b3;
    int mlen = use_long ? 20 : 7;
    if (params && plen > 0) {
        int cp = plen < (mlen - 4) ? plen : (mlen - 4);
        memcpy(msg + 4, params, (size_t)cp);
    }
    HidDev* tx = use_long ? &ep->l_dev : &ep->s_dev;
    if (!hid_write(tx, msg, mlen, 250)) return -1;

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
    uint8_t rx[64];
    for (;;) {
        if (ep->s_dev.alive) {
            int n = hid_read_sync(&ep->s_dev, rx, sizeof(rx), 0);
            if (n >= 6 && (rx[0] == 0x10 || rx[0] == 0x11)) {
                if (rx[1] == idx && rx[2] == b2 && rx[3] == b3) {
                    memset(out16, 0, 16);
                    int m = (n - 4) < 16 ? (n - 4) : 16;
                    if (m > 0) memcpy(out16, rx + 4, (size_t)m);
                    return 1;
                }
                if (rx[1] == idx && (rx[2] == 0xFF || rx[2] == 0x8F) && rx[3] == b2 && rx[4] == b3) {
                    if (err) *err = rx[5];
                    return 0;
                }
                handle_unsolicited_packet(ep, rx, n);
            }
        }
        LONGLONG rem = (LONGLONG)deadline - (LONGLONG)GetTickCount64();
        if (rem <= 0) return -1;
        int slice = rem > 15 ? 15 : (int)rem;
        if (ep->l_dev.alive) {
            int n = hid_read_sync(&ep->l_dev, rx, sizeof(rx), slice);
            if (n >= 6 && (rx[0] == 0x10 || rx[0] == 0x11)) {
                if (rx[1] == idx && rx[2] == b2 && rx[3] == b3) {
                    memset(out16, 0, 16);
                    int m = (n - 4) < 16 ? (n - 4) : 16;
                    if (m > 0) memcpy(out16, rx + 4, (size_t)m);
                    return 1;
                }
                if (rx[1] == idx && (rx[2] == 0xFF || rx[2] == 0x8F) && rx[3] == b2 && rx[4] == b3) {
                    if (err) *err = rx[5];
                    return 0;
                }
                handle_unsolicited_packet(ep, rx, n);
            }
        } else {
            Sleep((DWORD)slice);
        }
    }
}

static bool hidpp_call(LogiEp* ep, uint8_t idx, uint8_t feat_idx, uint8_t fn,
                       const uint8_t* params, int plen, uint8_t* out16) {
    uint8_t err = 0;
    return hidpp_request(ep, idx, feat_idx, (uint8_t)((fn << 4) | LOGI_SW_ID),
                         params, plen, out16, &err, false, 450) == 1;
}

static uint8_t hidpp_feature_index(LogiEp* ep, uint8_t idx, uint16_t feat_id) {
    uint8_t params[2] = { (uint8_t)(feat_id >> 8), (uint8_t)(feat_id & 0xFF) };
    uint8_t out[16];
    if (hidpp_call(ep, idx, 0x00, 0, params, 2, out)) return out[0];
    return 0;
}

static void enable_receiver_notifications(LogiEp* ep) {
    if (!ep->is_receiver || !ep->s_dev.alive) return;
    uint8_t out[16], err = 0;
    if (hidpp_request(ep, 0xFF, 0x81, 0x00, NULL, 0, out, &err, true, 200) == 1) {
        if ((out[1] & 0x01) == 0) {
            uint8_t p[3] = { out[0], (uint8_t)(out[1] | 0x01), out[2] };
            hidpp_request(ep, 0xFF, 0x80, 0x00, p, 3, out, &err, true, 200);
        }
    }
}

static void identify_slot(LogiEp* ep, LogiSlot* sl) {
    uint8_t idx = sl->dev_idx;
    sl->model_pid_count = 0;
    if (!ep->is_receiver) sl->model_pids[sl->model_pid_count++] = ep->pid;

    uint8_t f_name = hidpp_feature_index(ep, idx, FEAT_DEVICE_NAME);
    if (f_name) {
        uint8_t out[16];
        if (hidpp_call(ep, idx, f_name, 0, NULL, 0, out) && out[0] > 0) {
            int len = out[0];
            if (len > 90) len = 90;
            char ascii[96];
            int pos = 0;
            while (pos < len) {
                uint8_t off = (uint8_t)pos;
                if (!hidpp_call(ep, idx, f_name, 1, &off, 1, out)) break;
                int before = pos;
                for (int k = 0; k < 16 && pos < len && out[k]; k++) {
                    ascii[pos++] = (char)out[k];
                }
                if (pos == before) break;
            }
            ascii[pos] = 0;
            if (pos > 0) {
                wchar_t wname[96];
                wfrom_utf8(ascii, pos, wname, 96);
                wtrim_end(wname);
                if (wname[0]) {
                    if (wstartsi(wname, L"Logitech") || wstartsi(wname, L"G ") || wstartsi(wname, L"MX")) {
                        wcpy(sl->name, 96, wname);
                    } else {
                        wfmt(sl->name, 96, L"Logitech %s", wname);
                    }
                }
            }
        }
        if (hidpp_call(ep, idx, f_name, 2, NULL, 0, out)) {
            sl->kind = kind_from_logi_type(out[0]);
        }
    }
    if (!sl->name[0]) {
        wcpy(sl->name, 96, (!ep->is_receiver && ep->product[0]) ? ep->product : L"Logitech device");
    }
    if (sl->kind == KIND_OTHER && ep->is_headset_page) sl->kind = KIND_HEADSET;
    if (sl->kind == KIND_OTHER) sl->kind = kind_from_name(sl->name);

    uint8_t f_info = hidpp_feature_index(ep, idx, FEAT_FW_VERSION);
    if (f_info) {
        uint8_t out[16];
        if (hidpp_call(ep, idx, f_info, 0, NULL, 0, out)) {
            if (out[1] | out[2] | out[3] | out[4]) {
                wfmt(sl->unit_id, 16, L"%02X%02X%02X%02X", out[1], out[2], out[3], out[4]);
                wfmt(sl->id, 64, L"logi:unit:%s", sl->unit_id);
            }
            for (int i = 0; i < 3 && sl->model_pid_count < 4; i++) {
                uint16_t mp = (uint16_t)(((uint16_t)out[7 + 2 * i] << 8) | out[8 + 2 * i]);
                if (mp != 0) sl->model_pids[sl->model_pid_count++] = mp;
            }
        }
    }
    if (!sl->id[0]) {
        wfmt(sl->id, 64, L"logi:%04x:%s:%02x", ep->pid, ep->phys_key, idx);
    }
}

static bool query_slot_battery(LogiEp* ep, LogiSlot* sl) {
    uint8_t idx = sl->dev_idx;
    if (sl->hidpp10) {
        uint8_t out[16], err = 0;
        if (hidpp_request(ep, idx, 0x81, 0x0D, NULL, 0, out, &err, true, 400) == 1 && out[0] > 0 && out[0] <= 100) {
            sl->percent = out[0];
            sl->charging = (out[2] >= 0x21 && out[2] <= 0x26);
            sl->state = STATE_BATTERY;
            sl->last_query_ms = GetTickCount64();
            return true;
        }
        if (hidpp_request(ep, idx, 0x81, 0x07, NULL, 0, out, &err, true, 400) == 1 && out[0] > 0) {
            static const int pct[] = { 0, 5, 10, 20, 35, 50, 75, 100 };
            int idx_p = out[0] > 7 ? 7 : out[0];
            sl->percent = pct[idx_p];
            wcpy(sl->level_text, 16, out[0] >= 7 ? L"Full" : out[0] >= 5 ? L"Good" : out[0] >= 3 ? L"Low" : L"Critical");
            sl->charging = (out[1] >= 0x21 && out[1] <= 0x26);
            sl->state = STATE_BATTERY;
            sl->last_query_ms = GetTickCount64();
            return true;
        }
        return false;
    }

    if (sl->bat_feat_idx == 0) {
        static const uint16_t feats[4] = { FEAT_UNIFIED_BAT, FEAT_BATTERY_STATUS, FEAT_BATTERY_VOLT, FEAT_ADC_MEAS };
        for (int i = 0; i < 4; i++) {
            uint8_t fi = hidpp_feature_index(ep, idx, feats[i]);
            if (fi) {
                sl->bat_feat_id = feats[i];
                sl->bat_feat_idx = fi;
                break;
            }
        }
    }
    if (!sl->bat_feat_idx) return false;

    uint8_t out[16];
    uint8_t fn = (sl->bat_feat_id == FEAT_UNIFIED_BAT) ? 1 : 0;
    if (!hidpp_call(ep, idx, sl->bat_feat_idx, fn, NULL, 0, out)) return false;

    apply_battery_payload(ep, sl, sl->bat_feat_id, out, 16);
    sl->last_query_ms = GetTickCount64();
    return true;
}

static void probe_endpoint(LogiEp* ep, bool once_mode) {
    cancel_ep_async(ep);
    uint8_t indices[6] = { 0xFF, 0, 0, 0, 0, 0 };
    int nidx = 1;
    if (ep->pid == 0xC54F) {
        indices[0] = 1;
    } else if (ep->is_bluetooth) {
        indices[0] = 0xFF;
    } else {
        uint8_t p[16], err = 0;
        uint8_t ping[3] = { 0, 0, 0x5A };
        bool direct_ok = !ep->is_receiver &&
            hidpp_request(ep, 0xFF, 0x00, (uint8_t)(0x10 | LOGI_SW_ID), ping, 3, p, &err, false, 350) == 1 &&
            p[0] >= 2;
        if (direct_ok) {
            indices[0] = 0xFF;
        } else {
            ep->is_receiver = true;
            nidx = 6;
            for (int i = 0; i < 6; i++) indices[i] = (uint8_t)(i + 1);
            enable_receiver_notifications(ep);
        }
    }

    ep->slot_count = 0;
    for (int i = 0; i < nidx; i++) {
        uint8_t idx = indices[i];
        LogiSlot sl;
        memset(&sl, 0, sizeof(sl));
        sl.active = true;
        sl.dev_idx = idx;
        sl.percent = -1;
        sl.kind = KIND_OTHER;

        uint8_t p[16], err = 0;
        uint8_t ping[3] = { 0, 0, 0x5A };
        int r = hidpp_request(ep, idx, 0x00, (uint8_t)(0x10 | LOGI_SW_ID), ping, 3, p, &err, false, 400);
        if (r < 0 && ep->is_receiver && once_mode) {
            // Brief retry for sleeping receiver device in --once mode
            Sleep(350);
            r = hidpp_request(ep, idx, 0x00, (uint8_t)(0x10 | LOGI_SW_ID), ping, 3, p, &err, false, 400);
        }

        if (r == 1 && p[0] >= 2) {
            identify_slot(ep, &sl);
            if (query_slot_battery(ep, &sl)) {
                sl.known = true;
                ep->slots[ep->slot_count++] = sl;
            }
        } else if (r == 0 && err == 0x01) {
            sl.hidpp10 = true;
            wfmt(sl.id, 64, L"logi:%04x:%s:%02x", ep->pid, ep->phys_key, idx);
            wcpy(sl.name, 96, (!ep->is_receiver && ep->product[0]) ? ep->product : L"Logitech device");
            sl.kind = kind_from_name(sl.name);
            if (query_slot_battery(ep, &sl)) {
                sl.known = true;
                ep->slots[ep->slot_count++] = sl;
            }
        } else if (r < 0 && !ep->is_receiver &&
                   (wcontains_i(ep->product, L"wireless") || wcontains_i(ep->product, L"headset"))) {
            // Direct headset dongle present while headset is switched off
            wfmt(sl.id, 64, L"logi:%04x:%s:ff", ep->pid, ep->phys_key);
            wcpy(sl.name, 96, ep->product[0] ? ep->product : L"Logitech Headset");
            sl.kind = KIND_HEADSET;
            sl.state = STATE_OFF;
            sl.known = true;
            ep->slots[ep->slot_count++] = sl;
        }
    }
    arm_ep_async(ep);
}

// ---- G435 (046D:0ADF) ----
static uint16_t g435_crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool g435_transact(HidDev* d, uint8_t cmd, const uint8_t* body, int body_len, int wait_ms, int* mv_out) {
    uint8_t out[65];
    memset(out, 0, sizeof(out));
    out[1] = cmd;
    out[2] = (uint8_t)body_len;
    if (body && body_len > 0) {
        int cp = body_len < 60 ? body_len : 60;
        memcpy(out + 5, body, (size_t)cp);
    }
    uint16_t crc = g435_crc16(out + 1, 64);
    out[3] = (uint8_t)(crc & 0xFF);
    out[4] = (uint8_t)(crc >> 8);
    hid_flush(d);
    if (!hid_write(d, out, 65, 250)) return false;

    ULONGLONG end = GetTickCount64() + (ULONGLONG)wait_ms;
    uint8_t r[65];
    for (;;) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0) return true;
        int n = hid_read_sync(d, r, sizeof(r), (int)left);
        if (n < 8) continue;
        if (mv_out) {
            for (int i = 0; i + 8 <= n; i++) {
                if (r[i] == 0x00 && r[i + 1] == 0x07 && r[i + 2] == 0x00 && r[i + 3] == 0x06 && r[i + 5] == 0x00) {
                    int mv = r[i + 6] | (r[i + 7] << 8);
                    if (mv >= 2500 && mv <= 4500) { *mv_out = mv; return true; }
                }
            }
        }
    }
}

static void poll_g435(const HidList* hids) {
    const HidColl* target = NULL;
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid == LOGI_VID && c->pid == LOGI_G435_PID &&
            c->usage_page == 0xFF00 && c->usage == 0x0001 &&
            c->in_len == 65 && c->out_len == 65) {
            target = c;
            break;
        }
    }
    if (!target) {
        g_g435_present = false;
        g_g435_has_reading = false;
        return;
    }
    g_g435_present = true;
    HidDev d;
    if (!hid_open(&d, target, GENERIC_READ | GENERIC_WRITE, true)) return;

    static const uint8_t init_body[] = {
        0x50, 0x41, 0x06, 0x01, 0x0D, 0x00, 0x25, 0x34, 0x12, 0x5A,
        0x5A, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00
    };
    static const uint8_t s_open[] = { 0x01 }, s_close[] = { 0x02 };
    int mv = -1;
    if (g435_transact(&d, 0x84, s_open, 1, 50, NULL)) {
        g435_transact(&d, 0x85, s_open, 1, 50, NULL);
        g435_transact(&d, 0x80, init_body, sizeof(init_body), 450, NULL);
        uint8_t pa1[] = { 0x50, 0x41, 0x0E, 2, 1, 0xE0 };
        uint8_t pa2[] = { 0x50, 0x41, 0x0E, 3, 2, 0xE1, 0x01 };
        uint8_t pa3[] = { 0x50, 0x41, 0x0E, 4, 1, 0xE0 };
        uint8_t pa4[] = { 0x50, 0x41, 0x06, 5, 1, 0x00, 0x31 };
        g435_transact(&d, 0x80, pa1, sizeof(pa1), 120, NULL);
        g435_transact(&d, 0x80, pa2, sizeof(pa2), 120, NULL);
        g435_transact(&d, 0x80, pa3, sizeof(pa3), 120, NULL);
        g435_transact(&d, 0x80, pa4, sizeof(pa4), 250, &mv);
        g435_transact(&d, 0x85, s_close, 1, 50, NULL);
    }
    hid_close(&d);

    memset(&g_g435_dev, 0, sizeof(g_g435_dev));
    wcpy(g_g435_dev.id, 64, L"g435:0adf");
    wcpy(g_g435_dev.name, 96, L"Logitech G435");
    wcpy(g_g435_dev.via, 32, L"2.4 GHz");
    g_g435_dev.kind = KIND_HEADSET;
    g_g435_dev.vid = LOGI_VID;
    g_g435_dev.pid = LOGI_G435_PID;
    if (mv >= 0) {
        static const int v[] = { 4200, 4150, 4100, 4050, 4000, 3950, 3900, 3850, 3800, 3750, 3700, 3650, 3600, 3550, 3500, 3400 };
        static const int p[] = { 100, 95, 90, 85, 80, 75, 70, 62, 55, 45, 35, 25, 15, 10, 5, 0 };
        g_g435_dev.state = STATE_BATTERY;
        g_g435_dev.millivolts = mv;
        g_g435_dev.percent = interpolate_curve(mv, v, p, 16);
    } else {
        g_g435_dev.state = STATE_OFF;
        g_g435_dev.percent = -1;
    }
    g_g435_has_reading = true;
}

bool drv_logi_g435_present(const HidList* hids) {
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid == LOGI_VID && c->pid == LOGI_G435_PID &&
            c->usage_page == 0xFF00 && c->usage == 0x0001) return true;
    }
    return false;
}

void drv_logi_sync(const HidList* hids, bool once_mode) {
    // Group current Logitech HID++ collections by physical_key
    typedef struct {
        wchar_t key[128];
        uint16_t pid;
        const HidColl* short_c;
        const HidColl* long_c;
    } Cand;
    Cand cands[MAX_LOGI_EPS];
    int ncands = 0;
    memset(cands, 0, sizeof(cands));

    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->vid != LOGI_VID || c->pid == LOGI_G435_PID) continue;
        bool is_short = (c->usage_page == 0xFF00 && c->usage == 0x0001 && c->out_len == 7);
        bool is_long  = (((c->usage_page == 0xFF00 && c->usage == 0x0002) ||
                          (c->usage_page == 0xFF43 && c->usage == 0x0202)) && c->out_len == 20);
        if (!is_short && !is_long) continue;

        wchar_t pk[128];
        physical_key(c->path, pk, 128);
        int pos = -1;
        for (int k = 0; k < ncands; k++) {
            if (weq(cands[k].key, pk)) { pos = k; break; }
        }
        if (pos < 0) {
            if (ncands >= MAX_LOGI_EPS) continue;
            pos = ncands++;
            wcpy(cands[pos].key, 128, pk);
            cands[pos].pid = c->pid;
        }
        if (is_short && !cands[pos].short_c) cands[pos].short_c = c;
        if (is_long  && !cands[pos].long_c)  cands[pos].long_c  = c;
    }

    // Close removed or dead endpoints
    for (int e = 0; e < MAX_LOGI_EPS; e++) {
        if (!g_eps[e].active) continue;
        bool found = false;
        for (int k = 0; k < ncands; k++) {
            if (weq(g_eps[e].phys_key, cands[k].key)) { found = true; break; }
        }
        if (!found) close_ep(&g_eps[e]);
    }

    // Open new endpoints & probe
    for (int k = 0; k < ncands; k++) {
        if (!cands[k].long_c && !cands[k].short_c) continue;
        int existing = -1;
        for (int e = 0; e < MAX_LOGI_EPS; e++) {
            if (g_eps[e].active && weq(g_eps[e].phys_key, cands[k].key)) { existing = e; break; }
        }
        if (existing >= 0) continue;

        int slot_e = -1;
        for (int e = 0; e < MAX_LOGI_EPS; e++) {
            if (!g_eps[e].active) { slot_e = e; break; }
        }
        if (slot_e < 0) break;

        LogiEp* ep = &g_eps[slot_e];
        memset(ep, 0, sizeof(*ep));
        wcpy(ep->phys_key, 128, cands[k].key);
        ep->pid = cands[k].pid;
        const HidColl* ref_c = cands[k].long_c ? cands[k].long_c : cands[k].short_c;
        hid_product_name(ref_c->path, ep->product, 96);
        hid_container_id(ref_c->path, ep->container_id, 40);
        ep->is_bluetooth = wcontains_i(ref_c->path, L"{00001812-") || wcontains_i(ref_c->path, L"{00001124-");
        ep->is_receiver = is_known_receiver_pid(ep->pid) || wcontains_i(ep->product, L"Receiver");
        ep->is_headset_page = (cands[k].long_c && cands[k].long_c->usage_page == 0xFF43);

        if (cands[k].long_c)  hid_open(&ep->l_dev, cands[k].long_c, GENERIC_READ | GENERIC_WRITE, true);
        if (cands[k].short_c) hid_open(&ep->s_dev, cands[k].short_c, GENERIC_READ | GENERIC_WRITE, true);
        if (!ep->l_dev.alive && !ep->s_dev.alive) {
            close_ep(ep);
            continue;
        }
        ep->active = true;
        probe_endpoint(ep, once_mode);
    }

    poll_g435(hids);
}

int drv_logi_wait_handles(HANDLE* out, int max_count) {
    int n = 0;
    for (int e = 0; e < MAX_LOGI_EPS && n < max_count; e++) {
        if (!g_eps[e].active) continue;
        if (g_eps[e].l_dev.alive && g_eps[e].l_dev.read_pending && n < max_count) {
            out[n++] = g_eps[e].l_dev.ov.hEvent;
        }
        if (g_eps[e].s_dev.alive && g_eps[e].s_dev.read_pending && n < max_count) {
            out[n++] = g_eps[e].s_dev.ov.hEvent;
        }
    }
    return n;
}

bool drv_logi_on_io_ready(void) {
    bool changed = false;
    uint8_t rx[64];
    for (int e = 0; e < MAX_LOGI_EPS; e++) {
        LogiEp* ep = &g_eps[e];
        if (!ep->active) continue;

        bool need_query = false;
        HidDev* devs[2] = { &ep->l_dev, &ep->s_dev };
        for (int d = 0; d < 2; d++) {
            HidDev* hd = devs[d];
            if (!hd->alive || !hd->read_pending) continue;
            bool io_err = false;
            int n = hid_poll_async_read(hd, rx, sizeof(rx), &io_err);
            if (io_err) {
                close_ep(ep);
                changed = true;
                break;
            }
            if (n > 0) {
                if (handle_unsolicited_packet(ep, rx, n)) changed = true;
                for (int s = 0; s < ep->slot_count; s++) {
                    if (ep->slots[s].last_query_ms == 0) need_query = true;
                }
            }
        }
        if (!ep->active) continue;
        if (need_query) {
            cancel_ep_async(ep);
            for (int s = 0; s < ep->slot_count; s++) {
                LogiSlot* sl = &ep->slots[s];
                if (sl->last_query_ms == 0) {
                    if (query_slot_battery(ep, sl)) changed = true;
                    else sl->last_query_ms = GetTickCount64();
                }
            }
        }
        arm_ep_async(ep);
    }
    return changed;
}

bool drv_logi_refresh_awake(void) {
    bool changed = false;
    uint64_t now = GetTickCount64();
    for (int e = 0; e < MAX_LOGI_EPS; e++) {
        LogiEp* ep = &g_eps[e];
        if (!ep->active) continue;

        bool any_due = false;
        for (int s = 0; s < ep->slot_count; s++) {
            LogiSlot* sl = &ep->slots[s];
            if (sl->state == STATE_BATTERY && (now - sl->last_query_ms) >= 180000ULL) {
                any_due = true;
                break;
            }
        }
        if (!any_due) continue;

        cancel_ep_async(ep);
        for (int s = 0; s < ep->slot_count; s++) {
            LogiSlot* sl = &ep->slots[s];
            if (sl->state != STATE_BATTERY || (now - sl->last_query_ms) < 180000ULL) continue;
            int old_pct = sl->percent;
            bool old_chg = sl->charging;
            DeviceState old_st = sl->state;
            if (!query_slot_battery(ep, sl)) {
                // Mouse/keyboard stopped answering -> asleep, keep last known reading!
                if (ep->is_receiver) {
                    sl->state = STATE_ASLEEP;
                } else if (ep->is_headset_page || wcontains_i(ep->product, L"headset")) {
                    sl->state = STATE_OFF;
                }
                sl->last_query_ms = now;
            }
            if (sl->percent != old_pct || sl->charging != old_chg || sl->state != old_st) {
                changed = true;
            }
        }
        arm_ep_async(ep);
    }
    return changed;
}

void drv_logi_emit(ScanCtx* ctx) {
    for (int e = 0; e < MAX_LOGI_EPS; e++) {
        const LogiEp* ep = &g_eps[e];
        if (!ep->active) continue;
        bool any = false;
        for (int s = 0; s < ep->slot_count; s++) {
            const LogiSlot* sl = &ep->slots[s];
            if (!sl->known) continue;
            Obs o;
            memset(&o, 0, sizeof(o));
            wcpy(o.id, 64, sl->id);
            wcpy(o.name, 96, sl->name);
            wcpy(o.via, 32, ep->is_bluetooth ? L"Bluetooth" : (ep->is_receiver ? L"Logitech" : L"USB"));
            wcpy(o.level_text, 16, sl->level_text);
            wcpy(o.container_id, 40, ep->container_id);
            o.kind = sl->kind;
            o.has_kind = true;
            o.state = sl->state;
            o.percent = sl->percent;
            o.millivolts = sl->millivolts;
            o.charging = sl->charging;
            o.is_bt = ep->is_bluetooth;
            o.grace_ms = ep->is_receiver ? 1800000u : 60000u;
            o.vid = LOGI_VID;
            o.pid = ep->pid;
            scan_ctx_add(ctx, &o);
            any = true;
        }
        if (any) scan_ctx_claim(ctx, LOGI_VID, ep->pid);
    }

    if (g_g435_present && g_g435_has_reading) {
        Obs o;
        memset(&o, 0, sizeof(o));
        wcpy(o.id, 64, g_g435_dev.id);
        wcpy(o.name, 96, g_g435_dev.name);
        wcpy(o.via, 32, g_g435_dev.via);
        o.kind = KIND_HEADSET;
        o.has_kind = true;
        o.state = g_g435_dev.state;
        o.percent = g_g435_dev.percent;
        o.millivolts = g_g435_dev.millivolts;
        scan_ctx_add(ctx, &o);
        scan_ctx_claim(ctx, LOGI_VID, LOGI_G435_PID);
    }
}

void drv_logi_close_all(void) {
    for (int e = 0; e < MAX_LOGI_EPS; e++) {
        if (g_eps[e].active) close_ep(&g_eps[e]);
    }
}
