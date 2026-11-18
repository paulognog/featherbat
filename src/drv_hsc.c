#include "drivers.h"

static wchar_t g_hsc_exe[MAX_PATH];
static bool    g_hsc_checked = false;

static bool find_hsc_exe(void) {
    if (g_hsc_checked) return g_hsc_exe[0] != 0;
    g_hsc_checked = true;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    size_t last_sep = 0;
    for (size_t i = 0; self[i]; i++) {
        if (self[i] == L'\\' || self[i] == L'/') last_sep = i + 1;
    }
    self[last_sep] = 0;

    static const wchar_t* names[] = { L"headsetcontrol.exe", L"headsetcontrol-windows-x86_64.exe" };
    for (int i = 0; i < 2; i++) {
        wchar_t cand[MAX_PATH];
        wfmt(cand, MAX_PATH, L"%s%s", self, names[i]);
        if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            wcpy(g_hsc_exe, MAX_PATH, cand);
            return true;
        }
        if (SearchPathW(NULL, names[i], NULL, MAX_PATH, cand, NULL) > 0) {
            wcpy(g_hsc_exe, MAX_PATH, cand);
            return true;
        }
    }
    return false;
}

bool drv_hsc_present(void) {
    return find_hsc_exe();
}

static const char* find_json_str(const char* p, const char* end, const char* key, char* out, int cap) {
    size_t kl = alen(key);
    while (p < end) {
        const char* q = NULL;
        for (const char* s = p; s + kl + 2 < end; s++) {
            if (*s == '"' && s[kl + 1] == '"') {
                size_t j = 0;
                while (j < kl && s[1 + j] == key[j]) j++;
                if (j == kl) { q = s + kl + 2; break; }
            }
        }
        if (!q) return NULL;
        while (q < end && (*q == ' ' || *q == '\t' || *q == ':' || *q == '\r' || *q == '\n')) q++;
        if (q < end && *q == '"') {
            q++;
            int idx = 0;
            while (q < end && *q != '"') {
                if (*q == '\\' && q + 1 < end) q++;
                if (idx + 1 < cap) out[idx++] = *q;
                q++;
            }
            out[idx] = 0;
            return q + 1;
        }
        p = q;
    }
    return NULL;
}

static bool find_json_int(const char* p, const char* end, const char* key, int* out) {
    size_t kl = alen(key);
    for (const char* s = p; s + kl + 2 < end; s++) {
        if (*s == '"' && s[kl + 1] == '"') {
            size_t j = 0;
            while (j < kl && s[1 + j] == key[j]) j++;
            if (j == kl) {
                const char* q = s + kl + 2;
                while (q < end && (*q == ' ' || *q == '\t' || *q == ':' || *q == '\r' || *q == '\n')) q++;
                return aparse_int(&q, out);
            }
        }
    }
    return false;
}

void drv_hsc_scan(ScanCtx* ctx) {
    if (!find_hsc_exe()) return;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;

    wchar_t cmd[MAX_PATH + 48];
    wfmt(cmd, ARRAYSIZE(cmd), L"\"%s\" -b -o json --timeout 1500", g_hsc_exe);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return; }

    char* buf = (char*)xalloc(16384);
    int total = 0;
    if (buf) {
        ULONGLONG end = GetTickCount64() + 5000;
        while (total < 16383) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
            if (avail > 0) {
                DWORD got = 0;
                if (!ReadFile(rd, buf + total, (DWORD)(16383 - total), &got, NULL) || got == 0) break;
                total += (int)got;
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            if (GetTickCount64() >= end) { TerminateProcess(pi.hProcess, 1); break; }
            Sleep(25);
        }
        buf[total] = 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (!buf || total == 0) { xfree(buf); return; }

    // Scan each device block starting at "id_vendor"
    const char* cur = buf;
    const char* bend = buf + total;
    while (cur < bend) {
        char vid_s[16] = { 0 }, pid_s[16] = { 0 }, dev_s[96] = { 0 }, st_s[32] = { 0 };
        const char* next = find_json_str(cur, bend, "id_vendor", vid_s, sizeof(vid_s));
        if (!next) break;
        const char* p_pid = find_json_str(next, bend, "id_product", pid_s, sizeof(pid_s));
        if (!p_pid) break;
        find_json_str(cur, bend, "device", dev_s, sizeof(dev_s));
        const char* p_st = find_json_str(p_pid, bend, "status", st_s, sizeof(st_s));
        int level = -1;
        if (p_st) find_json_int(p_pid, bend, "level", &level);

        uint32_t vid = 0, pid = 0;
        aparse_u32_hex(vid_s, &vid);
        aparse_u32_hex(pid_s, &pid);
        if (vid > 0 && pid > 0 && !scan_ctx_is_claimed(ctx, (uint16_t)vid, (uint16_t)pid)) {
            bool avail = aeq(st_s, "BATTERY_AVAILABLE");
            bool chg   = aeq(st_s, "BATTERY_CHARGING");
            if ((avail || chg) && level >= 0) {
                Obs o;
                memset(&o, 0, sizeof(o));
                wfmt(o.id, 64, L"hsc:%04x:%04x", (unsigned)vid, (unsigned)pid);
                wfrom_utf8(dev_s, -1, o.name, 96);
                if (!o.name[0]) wcpy(o.name, 96, L"Headset");
                wcpy(o.via, 32, L"HeadsetControl");
                o.kind = KIND_HEADSET;
                o.has_kind = true;
                o.state = STATE_BATTERY;
                o.percent = clamp100(level);
                o.charging = chg;
                o.vid = (uint16_t)vid;
                o.pid = (uint16_t)pid;
                scan_ctx_add(ctx, &o);
                scan_ctx_claim(ctx, (uint16_t)vid, (uint16_t)pid);
            }
        }
        cur = p_st ? p_st : p_pid;
    }
    xfree(buf);
}
