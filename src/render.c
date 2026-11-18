#include "render.h"

#define MAX_SEGS (MAX_DEVICES * 3)

typedef struct {
    wchar_t text[16];
    int len;
    int x, w, h;
    int gap_after;
    uint32_t color;
    uint8_t opacity;
    bool is_icon;
} Seg;

static HFONT make_font(const wchar_t* face, const wchar_t* fallback, int height) {
    const wchar_t* faces[2] = { face, fallback };
    HDC dc = CreateCompatibleDC(NULL);
    HFONT chosen = NULL;
    for (int i = 0; i < 2; i++) {
        if (!faces[i]) continue;
        LOGFONTW lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = height;
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcpy(lf.lfFaceName, LF_FACESIZE, faces[i]);
        HFONT f = CreateFontIndirectW(&lf);
        if (!f) continue;
        if (i == 1 || !fallback) {
            chosen = f;
            break;
        }
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t got[LF_FACESIZE];
        memset(got, 0, sizeof(got));
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (weqi(got, faces[i])) {
            chosen = f;
            break;
        }
        DeleteObject(f);
    }
    DeleteDC(dc);
    return chosen;
}

static wchar_t kind_glyph(DeviceKind k, bool use_pair_earbuds) {
    switch (k) {
        case KIND_HEADSET:  return 0xE7F6;
        case KIND_EARBUDS:  return use_pair_earbuds ? 0xF4C0 : 0xF27C;
        case KIND_MOUSE:    return 0xE962;
        case KIND_KEYBOARD: return 0xE765;
        case KIND_GAMEPAD:  return 0xE7FC;
        default:            return 0xE83F;
    }
}

static int scale_dpi(int v, UINT dpi) {
    return MulDiv(v, (int)dpi, 96);
}

int render_widget(HWND hwnd, const Device* devs, int count, UINT dpi, int height, bool light, bool hover) {
    if (!hwnd || height <= 0) return 0;

    // Filter active devices
    int shown_idx[MAX_DEVICES];
    int shown_count = 0;
    for (int i = 0; i < count && shown_count < MAX_DEVICES; i++) {
        if (devs[i].state != STATE_OFF) {
            shown_idx[shown_count++] = i;
        }
    }
    if (shown_count == 0) return 0;

    const uint32_t fg    = light ? 0x000000 : 0xFFFFFF;
    const uint32_t red   = light ? 0xC42B1C : 0xFF6B6B;
    const uint32_t green = light ? 0x0F7B0F : 0x6CCB5F;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return 0;

    HFONT text_font = make_font(L"Segoe UI Variable Text", L"Segoe UI", -scale_dpi(12, dpi));
    HFONT icon_font = make_font(L"Segoe Fluent Icons", L"Segoe MDL2 Assets", -scale_dpi(15, dpi));

    // Check if Segoe Fluent Icons has single earbud 0xF27C
    bool use_pair_earbuds = false;
    SelectObject(dc, icon_font);
    wchar_t single_ch = 0xF27C;
    WORD glyph = 0;
    if (GetGlyphIndicesW(dc, &single_ch, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || glyph == 0xFFFF) {
        use_pair_earbuds = true;
    }

    Seg* segs = (Seg*)xalloc(MAX_SEGS * sizeof(Seg));
    if (!segs) {
        DeleteObject(text_font);
        DeleteObject(icon_font);
        DeleteDC(dc);
        return 0;
    }
    int nseg = 0;

    for (int i = 0; i < shown_count && nseg + 3 <= MAX_SEGS; i++) {
        const Device* d = &devs[shown_idx[i]];
        int gap = (i == shown_count - 1) ? 0 : scale_dpi(12, dpi);
        uint8_t op = (d->state == STATE_ASLEEP) ? 130 : 255;
        bool low = (d->percent >= 0 && d->percent <= LOW_BATTERY_PCT && !d->charging);
        uint32_t col = low ? red : fg;

        Seg* s_icon = &segs[nseg++];
        s_icon->text[0] = kind_glyph(d->kind, use_pair_earbuds);
        s_icon->text[1] = 0;
        s_icon->len = 1;
        s_icon->is_icon = true;
        s_icon->color = col;
        s_icon->opacity = op;
        s_icon->gap_after = scale_dpi(5, dpi);

        if (d->percent >= 0 || d->level_text[0] || !d->charging) {
            Seg* s_txt = &segs[nseg++];
            if (d->percent >= 0) {
                wfmt(s_txt->text, 16, L"%d%%", d->percent);
            } else if (d->level_text[0]) {
                wcpy(s_txt->text, 16, d->level_text);
            } else {
                wcpy(s_txt->text, 16, L"?");
            }
            s_txt->len = (int)wlen(s_txt->text);
            s_txt->is_icon = false;
            s_txt->color = col;
            s_txt->opacity = op;
            s_txt->gap_after = d->charging ? scale_dpi(2, dpi) : gap;
        }

        if (d->charging) {
            Seg* s_bolt = &segs[nseg++];
            s_bolt->text[0] = 0xE945;
            s_bolt->text[1] = 0;
            s_bolt->len = 1;
            s_bolt->is_icon = true;
            s_bolt->color = green;
            s_bolt->opacity = op;
            s_bolt->gap_after = gap;
        }
    }

    int pad = scale_dpi(6, dpi);
    int x = pad;
    for (int i = 0; i < nseg; i++) {
        SelectObject(dc, segs[i].is_icon ? icon_font : text_font);
        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(dc, segs[i].text, segs[i].len, &sz);
        segs[i].x = x;
        segs[i].w = sz.cx;
        segs[i].h = sz.cy;
        x += sz.cx + segs[i].gap_after;
    }
    int width = x + pad;
    if (width <= 0) width = 1;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        xfree(segs);
        DeleteObject(text_font);
        DeleteObject(icon_font);
        DeleteDC(dc);
        return width;
    }

    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, 0x00FFFFFF);
    for (int i = 0; i < nseg; i++) {
        SelectObject(dc, segs[i].is_icon ? icon_font : text_font);
        TextOutW(dc, segs[i].x, (height - segs[i].h) / 2, segs[i].text, segs[i].len);
    }
    GdiFlush();

    uint32_t* col_color = (uint32_t*)xalloc((size_t)width * sizeof(uint32_t));
    uint8_t*  col_alpha = (uint8_t*)xalloc((size_t)width * sizeof(uint8_t));
    if (col_color && col_alpha) {
        for (int c = 0; c < width; c++) {
            col_color[c] = fg;
            col_alpha[c] = 255;
        }
        for (int i = 0; i < nseg; i++) {
            int start = segs[i].x < 0 ? 0 : segs[i].x;
            int end = segs[i].x + segs[i].w + segs[i].gap_after;
            if (end > width) end = width;
            for (int c = start; c < end; c++) {
                col_color[c] = segs[i].color;
                col_alpha[c] = segs[i].opacity;
            }
        }

        uint32_t* px = (uint32_t*)bits;
        int inset = scale_dpi(4, dpi);
        int rad = scale_dpi(4, dpi);
        uint32_t h_rgb = fg;
        uint32_t h_a = light ? 20 : 26;

        for (int y = 0; y < height; y++) {
            for (int c = 0; c < width; c++) {
                uint32_t p = px[y * width + c];
                uint32_t r = (p >> 16) & 0xFF;
                uint32_t g = (p >> 8) & 0xFF;
                uint32_t b = p & 0xFF;
                uint32_t cov = r > g ? (r > b ? r : b) : (g > b ? g : b);
                uint32_t a = cov * col_alpha[c] / 255;

                uint32_t bg_a = 0;
                if (hover && y >= inset && y < height - inset) {
                    int dx = c < rad ? rad - c : (c >= width - rad ? c - (width - rad - 1) : 0);
                    int dy = y < inset + rad ? inset + rad - y : (y >= height - inset - rad ? y - (height - inset - rad - 1) : 0);
                    if (dx * dx + dy * dy <= rad * rad) bg_a = h_a;
                }

                if (a == 0 && bg_a == 0) {
                    px[y * width + c] = 0x01000000;
                } else if (bg_a == 0) {
                    uint32_t col = col_color[c];
                    uint32_t pr = ((col >> 16) & 0xFF) * a / 255;
                    uint32_t pg = ((col >> 8) & 0xFF) * a / 255;
                    uint32_t pb = (col & 0xFF) * a / 255;
                    px[y * width + c] = (a << 24) | (pr << 16) | (pg << 8) | pb;
                } else {
                    uint32_t col = col_color[c];
                    uint32_t inv = 255 - a;
                    uint32_t oa = a + bg_a * inv / 255;
                    uint32_t br = ((h_rgb >> 16) & 0xFF) * bg_a / 255;
                    uint32_t bg = ((h_rgb >> 8) & 0xFF) * bg_a / 255;
                    uint32_t bb = (h_rgb & 0xFF) * bg_a / 255;
                    uint32_t pr = ((col >> 16) & 0xFF) * a / 255 + br * inv / 255;
                    uint32_t pg = ((col >> 8) & 0xFF) * a / 255 + bg * inv / 255;
                    uint32_t pb = (col & 0xFF) * a / 255 + bb * inv / 255;
                    px[y * width + c] = (oa << 24) | (pr << 16) | (pg << 8) | pb;
                }
            }
        }
    }
    xfree(col_color);
    xfree(col_alpha);
    xfree(segs);

    SIZE sz = { width, height };
    POINT origin = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, NULL, NULL, &sz, dc, &origin, 0, &blend, ULW_ALPHA);

    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteObject(text_font);
    DeleteObject(icon_font);
    DeleteDC(dc);
    return width;
}
