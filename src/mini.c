#include "mini.h"

#ifdef __GNUC__
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    }
    return 0;
}

void* xalloc(size_t sz) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sz ? sz : 1);
}

void* xrealloc(void* p, size_t sz) {
    if (!p) return xalloc(sz);
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, sz ? sz : 1);
}

void xfree(void* p) {
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

static wchar_t wlow(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
}

size_t wlen(const wchar_t* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void wcpy(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void wcat(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0 || !src) return;
    size_t n = wlen(dst);
    if (n >= cap) return;
    wcpy(dst + n, cap - n, src);
}

int wcmp(const wchar_t* a, const wchar_t* b) {
    if (!a) a = L"";
    if (!b) b = L"";
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint16_t)*a - (int)(uint16_t)*b;
}

int wcmpi(const wchar_t* a, const wchar_t* b) {
    if (!a) a = L"";
    if (!b) b = L"";
    while (*a && wlow(*a) == wlow(*b)) { a++; b++; }
    return (int)(uint16_t)wlow(*a) - (int)(uint16_t)wlow(*b);
}

bool weq(const wchar_t* a, const wchar_t* b) { return wcmp(a, b) == 0; }
bool weqi(const wchar_t* a, const wchar_t* b) { return wcmpi(a, b) == 0; }

bool wstarts(const wchar_t* s, const wchar_t* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

bool wstartsi(const wchar_t* s, const wchar_t* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (wlow(*s) != wlow(*prefix)) return false;
        s++; prefix++;
    }
    return true;
}

bool wends(const wchar_t* s, const wchar_t* suffix) {
    size_t ls = wlen(s), lf = wlen(suffix);
    if (lf > ls) return false;
    return weq(s + (ls - lf), suffix);
}

const wchar_t* wfind_i(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle || !*needle) return hay;
    size_t nl = wlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && wlow(hay[i]) == wlow(needle[i])) i++;
        if (i == nl) return hay;
    }
    return NULL;
}

bool wcontains_i(const wchar_t* hay, const wchar_t* needle) {
    return wfind_i(hay, needle) != NULL;
}

void wto_lower(wchar_t* s) {
    if (!s) return;
    for (; *s; s++) *s = wlow(*s);
}

void wtrim_end(wchar_t* s) {
    if (!s) return;
    size_t n = wlen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n')) {
        s[--n] = 0;
    }
}

void wfrom_utf8(const char* src, int src_len, wchar_t* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    dst[0] = 0;
    if (!src || src_len == 0) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, src_len, dst, (int)dst_cap - 1);
    if (n < 0) n = 0;
    if ((size_t)n >= dst_cap) n = (int)dst_cap - 1;
    dst[n] = 0;
}

static wchar_t hex_digit(unsigned v, bool upper) {
    v &= 0xF;
    if (v < 10) return (wchar_t)(L'0' + v);
    return (wchar_t)((upper ? L'A' : L'a') + (v - 10));
}

void guid_to_wstr(const GUID* g, wchar_t* out, size_t cap) {
    if (!out || cap < 39) return;
    wfmt(out, cap, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
         (unsigned)g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
         (unsigned)g->Data4[0], (unsigned)g->Data4[1],
         (unsigned)g->Data4[2], (unsigned)g->Data4[3],
         (unsigned)g->Data4[4], (unsigned)g->Data4[5],
         (unsigned)g->Data4[6], (unsigned)g->Data4[7]);
}

uint32_t fnv1a_w(const wchar_t* s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (; *s; s++) {
        h ^= (uint16_t)*s;
        h *= 16777619u;
    }
    return h;
}

bool is_pt_ui(void) {
    LANGID id = GetUserDefaultUILanguage();
    return (id & 0x3FF) == LANG_PORTUGUESE;
}

size_t alen(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

bool aeq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

bool aparse_int(const char** p, int* out) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') { s++; }
    if (*s < '0' || *s > '9') return false;
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    *out = neg ? -val : val;
    *p = s;
    return true;
}

bool aparse_u32_hex(const char* s, uint32_t* out) {
    if (!s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    bool any = false;
    while (*s) {
        char c = *s;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        any = true;
        s++;
    }
    if (any) *out = v;
    return any;
}

void wb_init(WBuf* b, size_t initial_cap) {
    b->len = 0;
    b->cap = initial_cap < 64 ? 64 : initial_cap;
    b->data = (wchar_t*)xalloc(b->cap * sizeof(wchar_t));
    if (b->data) b->data[0] = 0;
}

void wb_free(WBuf* b) {
    xfree(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void wb_clear(WBuf* b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

static void wb_grow(WBuf* b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < b->len + need + 1) ncap *= 2;
    wchar_t* nd = (wchar_t*)xrealloc(b->data, ncap * sizeof(wchar_t));
    if (!nd) return;
    b->data = nd;
    b->cap = ncap;
}

void wb_putc(WBuf* b, wchar_t ch) {
    wb_grow(b, 1);
    if (b->len + 1 < b->cap) {
        b->data[b->len++] = ch;
        b->data[b->len] = 0;
    }
}

void wb_puts(WBuf* b, const wchar_t* s) {
    if (!s) return;
    size_t n = wlen(s);
    wb_grow(b, n);
    if (b->len + n < b->cap) {
        memcpy(b->data + b->len, s, n * sizeof(wchar_t));
        b->len += n;
        b->data[b->len] = 0;
    }
}

void wb_puta(WBuf* b, const char* s) {
    if (!s || !*s) return;
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 1) return;
    wb_grow(b, (size_t)need);
    if (b->len + (size_t)need <= b->cap) {
        int written = MultiByteToWideChar(CP_UTF8, 0, s, -1, b->data + b->len, need);
        if (written > 0) b->len += (size_t)(written - 1);
        b->data[b->len] = 0;
    }
}

static void wb_put_pad(WBuf* b, const wchar_t* tmp, int len, int width, wchar_t pad, bool left_align) {
    if (left_align) {
        for (int i = 0; i < len; i++) wb_putc(b, tmp[i]);
        for (int i = len; i < width; i++) wb_putc(b, L' ');
    } else {
        for (int i = len; i < width; i++) wb_putc(b, pad);
        for (int i = 0; i < len; i++) wb_putc(b, tmp[i]);
    }
}

void wb_vfmt(WBuf* b, const wchar_t* fmt, va_list ap) {
    for (const wchar_t* p = fmt; *p; p++) {
        if (*p != L'%') {
            wb_putc(b, *p);
            continue;
        }
        p++;
        if (*p == L'%') { wb_putc(b, L'%'); continue; }
        bool left = false;
        wchar_t pad = L' ';
        if (*p == L'-') { left = true; p++; }
        if (*p == L'0') { pad = L'0'; p++; }
        int width = 0;
        while (*p >= L'0' && *p <= L'9') {
            width = width * 10 + (*p - L'0');
            p++;
        }
        bool is_ll = false;
        if (*p == L'l') {
            p++;
            if (*p == L'l') { is_ll = true; p++; }
        }
        wchar_t spec = *p;
        if (!spec) break;
        if (spec == L's') {
            const wchar_t* s = va_arg(ap, const wchar_t*);
            if (!s) s = L"(null)";
            int len = (int)wlen(s);
            if (left) {
                wb_puts(b, s);
                for (int i = len; i < width; i++) wb_putc(b, L' ');
            } else {
                for (int i = len; i < width; i++) wb_putc(b, L' ');
                wb_puts(b, s);
            }
        } else if (spec == L'S') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            wb_puta(b, s);
        } else if (spec == L'c') {
            wchar_t ch = (wchar_t)va_arg(ap, int);
            wb_putc(b, ch);
        } else if (spec == L'd' || spec == L'i') {
            int64_t val = is_ll ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            wchar_t tmp[32];
            int idx = 0;
            bool neg = val < 0;
            uint64_t u = neg ? (uint64_t)(-val) : (uint64_t)val;
            if (u == 0) tmp[idx++] = L'0';
            while (u > 0) { tmp[idx++] = (wchar_t)(L'0' + (u % 10)); u /= 10; }
            if (neg) tmp[idx++] = L'-';
            for (int i = 0; i < idx / 2; i++) {
                wchar_t t = tmp[i]; tmp[i] = tmp[idx - 1 - i]; tmp[idx - 1 - i] = t;
            }
            wb_put_pad(b, tmp, idx, width, pad, left);
        } else if (spec == L'u') {
            uint64_t u = is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            wchar_t tmp[32];
            int idx = 0;
            if (u == 0) tmp[idx++] = L'0';
            while (u > 0) { tmp[idx++] = (wchar_t)(L'0' + (u % 10)); u /= 10; }
            for (int i = 0; i < idx / 2; i++) {
                wchar_t t = tmp[i]; tmp[i] = tmp[idx - 1 - i]; tmp[idx - 1 - i] = t;
            }
            wb_put_pad(b, tmp, idx, width, pad, left);
        } else if (spec == L'x' || spec == L'X') {
            bool upper = (spec == L'X');
            uint64_t u = is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            wchar_t tmp[32];
            int idx = 0;
            if (u == 0) tmp[idx++] = L'0';
            while (u > 0) { tmp[idx++] = hex_digit((unsigned)(u & 0xF), upper); u >>= 4; }
            for (int i = 0; i < idx / 2; i++) {
                wchar_t t = tmp[i]; tmp[i] = tmp[idx - 1 - i]; tmp[idx - 1 - i] = t;
            }
            wb_put_pad(b, tmp, idx, width, pad, left);
        }
    }
}

void wb_fmt(WBuf* b, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wb_vfmt(b, fmt, ap);
    va_end(ap);
}

void wfmt(wchar_t* dst, size_t cap, const wchar_t* fmt, ...) {
    if (!dst || cap == 0) return;
    WBuf b;
    wb_init(&b, cap < 128 ? 128 : cap);
    va_list ap;
    va_start(ap, fmt);
    wb_vfmt(&b, fmt, ap);
    va_end(ap);
    wcpy(dst, cap, b.data ? b.data : L"");
    wb_free(&b);
}

static HANDLE g_con_out = INVALID_HANDLE_VALUE;
static bool   g_con_owned = false;

void con_init(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_con_out = h;
        g_con_owned = false;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_con_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        g_con_owned = (g_con_out != INVALID_HANDLE_VALUE);
    }
}

void con_puts(const wchar_t* s) {
    if (!s || g_con_out == INVALID_HANDLE_VALUE || !g_con_out) return;
    int slen = (int)wlen(s);
    if (slen == 0) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, s, slen, NULL, 0, NULL, NULL);
    if (bytes <= 0) return;
    char* utf8 = (char*)xalloc((size_t)bytes);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, s, slen, utf8, bytes, NULL, NULL);
    DWORD written = 0;
    WriteFile(g_con_out, utf8, (DWORD)bytes, &written, NULL);
    xfree(utf8);
}

void con_fmt(const wchar_t* fmt, ...) {
    WBuf b;
    wb_init(&b, 256);
    va_list ap;
    va_start(ap, fmt);
    wb_vfmt(&b, fmt, ap);
    va_end(ap);
    if (b.data) con_puts(b.data);
    wb_free(&b);
}

void con_close(void) {
    if (g_con_owned && g_con_out != INVALID_HANDLE_VALUE) {
        CloseHandle(g_con_out);
        FreeConsole();
    }
    g_con_out = INVALID_HANDLE_VALUE;
    g_con_owned = false;
}

int clamp100(int v) {
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

int interpolate_curve(int mv, const int* mv_table, const int* pct_table, int n) {
    if (n <= 0) return 0;
    if (mv >= mv_table[0]) return pct_table[0];
    for (int i = 1; i < n; i++) {
        if (mv >= mv_table[i]) {
            int span = mv_table[i - 1] - mv_table[i];
            if (span <= 0) return pct_table[i];
            int num = (pct_table[i - 1] - pct_table[i]) * (mv - mv_table[i]) + span / 2;
            return pct_table[i] + num / span;
        }
    }
    return pct_table[n - 1];
}
