#include "../src/mini.h"
#include "../src/device.h"
#include "../src/drivers.h"

static int g_fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        con_fmt(L"FAIL line %d: %S\n", __LINE__, #cond); \
        g_fails++; \
    } \
} while (0)

void __attribute__((noreturn)) mainCRTStartup(void) {
    con_init();

    // 1. String & formatting helpers (mini.c)
    wchar_t buf[64];
    wfmt(buf, 64, L"%04X:%S:%-4s:%d", 0x046D, "ok", L"hi", -12);
    CHECK(weq(buf, L"046D:ok:hi  :-12"));
    CHECK(wcontains_i(L"Attack Shark X11", L"SHARK"));
    CHECK(wstartsi(L"BTHLE\\DEV_AABB", L"bthle\\dev_"));

    GUID g = { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } };
    guid_to_wstr(&g, buf, 64);
    CHECK(weq(buf, L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5}"));

    uint32_t hx = 0;
    CHECK(aparse_u32_hex("0xC539", &hx) && hx == 0xC539);
    const char* p = " -42,next";
    int iv = 0;
    CHECK(aparse_int(&p, &iv) && iv == -42 && *p == ',');

    // 2. Kind classification
    CHECK(kind_from_name(L"Renan's AirPods") == KIND_EARBUDS);
    CHECK(kind_from_name(L"AirPods Max") == KIND_HEADSET);
    CHECK(kind_from_name(L"Galaxy Buds2 Pro") == KIND_EARBUDS);
    CHECK(kind_from_name(L"PRO X Wireless Gaming Headset") == KIND_HEADSET);
    CHECK(kind_from_name(L"MX Master 3S Mouse") == KIND_MOUSE);
    CHECK(kind_from_name(L"Keychron K2 Keyboard") == KIND_KEYBOARD);
    CHECK(kind_from_name(L"Xbox Wireless Controller") == KIND_GAMEPAD);

    // 3. Logitech voltage curves
    CHECK(logi_liion_percent(4200) == 100);
    CHECK(logi_liion_percent(3811) == 50);
    CHECK(logi_liion_percent(3500) == 0);
    CHECK(logi_liion_percent(3400) == 0);
    CHECK(logi_headset_percent(0x0A66, 3850) == 50);
    CHECK(logi_headset_percent(0x0AB5, 3950) == 80);

    // 4. PlayStation battery parsers
    int pct = 0;
    bool chg = false;
    ps_parse_dualsense(0x08, &pct, &chg);
    CHECK(pct == 85 && !chg);
    ps_parse_dualsense(0x13, &pct, &chg);
    CHECK(pct == 35 && chg);
    ps_parse_dualsense(0x2A, &pct, &chg);
    CHECK(pct == 100 && chg);
    ps_parse_dualsense(0x0A, &pct, &chg);
    CHECK(pct == 100 && !chg);

    ps_parse_ds4(0x05, &pct, &chg);
    CHECK(pct == 55 && !chg);
    ps_parse_ds4(0x14, &pct, &chg);
    CHECK(pct == 40 && chg);
    ps_parse_ds4(0x1B, &pct, &chg);
    CHECK(pct == 100 && !chg);

    // 5. Kysona checksum
    uint8_t k04[16] = { 0x08, 0x04 };
    CHECK(kysona_checksum(k04) == 0x49);
    uint8_t k03[16] = { 0x08, 0x03 };
    CHECK(kysona_checksum(k03) == 0x4A);

    if (g_fails == 0) {
        con_puts(L"All unit tests passed.\n");
    } else {
        con_fmt(L"%d test(s) failed.\n", g_fails);
    }
    con_close();
    ExitProcess(g_fails == 0 ? 0u : 1u);
}
