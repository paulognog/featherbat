#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Freestanding memory primitives (used by compiler codegen too)
void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

// Win32 ProcessHeap wrappers
void* xalloc(size_t sz);
void* xrealloc(void* p, size_t sz);
void  xfree(void* p);

// Wide string helpers
size_t wlen(const wchar_t* s);
void   wcpy(wchar_t* dst, size_t cap, const wchar_t* src);
void   wcat(wchar_t* dst, size_t cap, const wchar_t* src);
int    wcmp(const wchar_t* a, const wchar_t* b);
int    wcmpi(const wchar_t* a, const wchar_t* b);
bool   weq(const wchar_t* a, const wchar_t* b);
bool   weqi(const wchar_t* a, const wchar_t* b);
bool   wstarts(const wchar_t* s, const wchar_t* prefix);
bool   wstartsi(const wchar_t* s, const wchar_t* prefix);
bool   wends(const wchar_t* s, const wchar_t* suffix);
bool   wcontains_i(const wchar_t* hay, const wchar_t* needle);
const wchar_t* wfind_i(const wchar_t* hay, const wchar_t* needle);
void   wto_lower(wchar_t* s);
void   wtrim_end(wchar_t* s);
void   wfrom_utf8(const char* src, int src_len, wchar_t* dst, size_t dst_cap);
void   guid_to_wstr(const GUID* g, wchar_t* out, size_t cap);
uint32_t fnv1a_w(const wchar_t* s);
bool   is_pt_ui(void);

// ASCII helpers
size_t alen(const char* s);
bool   aeq(const char* a, const char* b);
bool   aparse_int(const char** p, int* out);
bool   aparse_u32_hex(const char* s, uint32_t* out);

// Dynamic wide string builder
typedef struct {
    wchar_t* data;
    size_t len;
    size_t cap;
} WBuf;

void wb_init(WBuf* b, size_t initial_cap);
void wb_free(WBuf* b);
void wb_clear(WBuf* b);
void wb_putc(WBuf* b, wchar_t ch);
void wb_puts(WBuf* b, const wchar_t* s);
void wb_puta(WBuf* b, const char* s);
void wb_vfmt(WBuf* b, const wchar_t* fmt, va_list ap);
void wb_fmt(WBuf* b, const wchar_t* fmt, ...);

// Fixed-buffer formatter (always NUL-terminated)
void wfmt(wchar_t* dst, size_t cap, const wchar_t* fmt, ...);

// Console output (for --once / --dump in -mwindows binary)
void con_init(void);
void con_puts(const wchar_t* s);
void con_fmt(const wchar_t* fmt, ...);
void con_close(void);

// Math helpers (integer-only)
int clamp100(int v);
int interpolate_curve(int mv, const int* mv_table, const int* pct_table, int n);

#ifdef __cplusplus
}
#endif
