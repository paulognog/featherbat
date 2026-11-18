#include "widget.h"
#include "render.h"
#include "alert.h"
#include "io.h"
#include <commctrl.h>
#include <dbt.h>

#define TIMER_RETRY_BAR 1
#define PBT_APMRESUMEAUTOMATIC_VAL 0x12
#define WM_APP_MENU (WM_APP + 2)
#define CMD_REFRESH 101
#define CMD_EXIT    102

static HINSTANCE g_inst = NULL;
static HWND g_ctrl = NULL;
static HWND g_widget = NULL;
static HWND g_tip = NULL;
static HWND g_taskbar = NULL;
static HWND g_tray_notify = NULL;
static UINT g_taskbar_created_msg = 0;
static HWINEVENTHOOK g_hook_loc = NULL;
static HWINEVENTHOOK g_hook_reo = NULL;

static bool g_light = false;
static bool g_hover = false;
static bool g_quitting = false;
static int  g_width = 0;
static UINT g_last_dpi = 0;
static RECT g_last_bar = { 0 };
static RECT g_last_tray = { 0 };
static int  g_last_x = -1;
static int  g_last_w = -1;

static Device g_devs[MAX_DEVICES];
static int    g_dev_count = 0;

static bool read_light_theme(void) {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        return val != 0;
    }
    return false;
}

static void apply_tip_theme(void) {
    if (!g_tip) return;
    typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    SetWindowThemeFn set_theme = (SetWindowThemeFn)(void*)GetProcAddress(ux, "SetWindowTheme");
    if (set_theme) {
        set_theme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", NULL);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

static void unhook_events(void) {
    if (g_hook_loc) { UnhookWinEvent(g_hook_loc); g_hook_loc = NULL; }
    if (g_hook_reo) { UnhookWinEvent(g_hook_reo); g_hook_reo = NULL; }
}

static void redraw(void);
static void place_widget(void);
static void ensure_widget(void);

static void CALLBACK win_event_cb(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG id_obj, LONG id_child, DWORD thread, DWORD time) {
    (void)hook; (void)event; (void)id_child; (void)thread; (void)time;
    if (g_quitting || id_obj != OBJID_WINDOW || !hwnd) return;
    if (!g_taskbar || !IsWindow(g_taskbar)) return;
    if (hwnd != g_taskbar && hwnd != g_tray_notify && GetParent(hwnd) != g_taskbar) return;

    UINT dpi = GetDpiForWindow(g_taskbar);
    if (dpi < 96) dpi = 96;
    if (dpi != g_last_dpi) {
        redraw();
    } else {
        place_widget();
    }
}

static void hook_taskbar_events(void) {
    unhook_events();
    if (!g_taskbar || !IsWindow(g_taskbar)) return;
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(g_taskbar, &pid);
    if (!tid) return;
    g_hook_loc = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                 NULL, win_event_cb, pid, tid, WINEVENT_OUTOFCONTEXT);
    g_hook_reo = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER,
                                 NULL, win_event_cb, pid, tid, WINEVENT_OUTOFCONTEXT);
}

static void update_tooltip(void) {
    if (!g_tip || !g_widget) return;
    WBuf tip;
    wb_init(&tip, 256);
    bool pt = is_pt_ui();
    bool first = true;
    for (int i = 0; i < g_dev_count; i++) {
        const Device* d = &g_devs[i];
        if (d->state == STATE_OFF) continue;
        if (!first) wb_puts(&tip, L"\r\n");
        first = false;
        wb_puts(&tip, d->name);
        wb_puts(&tip, L": ");
        if (d->percent >= 0) {
            wb_fmt(&tip, L"%d%%", d->percent);
            if (d->charging) wb_puts(&tip, pt ? L" (carregando)" : L" (charging)");
        } else if (d->level_text[0]) {
            wb_puts(&tip, d->level_text);
            if (d->charging) wb_puts(&tip, pt ? L" (carregando)" : L" (charging)");
        } else if (d->charging) {
            wb_puts(&tip, pt ? L"carregando" : L"charging");
        } else {
            wb_puts(&tip, L"?");
        }
        if (d->state == STATE_ASLEEP) wb_puts(&tip, pt ? L" (em repouso)" : L" (asleep)");
    }
    TTTOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_ctrl;
    ti.uId = (UINT_PTR)g_widget;
    ti.lpszText = tip.data ? tip.data : L"";
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    wb_free(&tip);
}

static void place_widget(void) {
    if (!g_widget || !IsWindow(g_widget)) return;
    if (g_width <= 0) {
        if (IsWindowVisible(g_widget)) ShowWindow(g_widget, SW_HIDE);
        return;
    }
    if (!g_taskbar || !IsWindow(g_taskbar)) return;
    if (!g_tray_notify || !IsWindow(g_tray_notify)) {
        g_tray_notify = FindWindowExW(g_taskbar, NULL, L"TrayNotifyWnd", NULL);
    }

    RECT bar = { 0 };
    GetWindowRect(g_taskbar, &bar);
    int h = bar.bottom - bar.top;
    if (h <= 0 || h > (bar.right - bar.left)) {
        // Vertical or collapsed taskbar
        if (IsWindowVisible(g_widget)) ShowWindow(g_widget, SW_HIDE);
        return;
    }

    RECT tr = { 0 };
    bool rtl = (GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    int edge = rtl ? bar.left : bar.right;
    if (g_tray_notify && GetWindowRect(g_tray_notify, &tr) && tr.right > tr.left) {
        edge = rtl ? tr.right : tr.left;
    }

    // Optional Windows 10 News & Interests window support if running on Win10
    HWND feeds = FindWindowExW(g_taskbar, NULL, L"ShellFeedsTaskbarViewWindow", NULL);
    if (!rtl && feeds && IsWindowVisible(feeds)) {
        RECT fr = { 0 };
        if (GetWindowRect(feeds, &fr) && fr.left < edge && fr.right > fr.left) edge = fr.left;
    }

    UINT dpi = GetDpiForWindow(g_taskbar);
    if (dpi < 96) dpi = 96;
    int margin = MulDiv(4, (int)dpi, 96);

    POINT pt_edge = { edge, bar.top };
    ScreenToClient(g_taskbar, &pt_edge);
    int x = pt_edge.x - g_width - margin;

    DWORD custom_offset = 0, sz = sizeof(custom_offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\featherbat", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, NULL, &custom_offset, &sz) == ERROR_SUCCESS) {
        x -= MulDiv((int)custom_offset, (int)dpi, 96);
    }

    bool covered = GetWindow(g_widget, GW_HWNDPREV) != NULL;
    bool visible = IsWindowVisible(g_widget) != 0;
    if (!covered && visible && x == g_last_x && g_width == g_last_w &&
        EqualRect(&bar, &g_last_bar) && EqualRect(&tr, &g_last_tray)) {
        return;
    }

    g_last_bar = bar;
    g_last_tray = tr;
    g_last_x = x;
    g_last_w = g_width;
    SetWindowPos(g_widget, HWND_TOP, x, 0, g_width, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void redraw(void) {
    if (!g_widget || !IsWindow(g_widget) || !g_taskbar || !IsWindow(g_taskbar)) return;
    RECT bar = { 0 };
    GetWindowRect(g_taskbar, &bar);
    int h = bar.bottom - bar.top;
    UINT dpi = GetDpiForWindow(g_taskbar);
    if (dpi < 96) dpi = 96;
    g_last_dpi = dpi;

    update_tooltip();
    g_width = render_widget(g_widget, g_devs, g_dev_count, dpi, h, g_light, g_hover);
    place_widget();
}

static void destroy_widget(void) {
    unhook_events();
    if (g_tip) { DestroyWindow(g_tip); g_tip = NULL; }
    if (g_widget) { DestroyWindow(g_widget); g_widget = NULL; }
    g_taskbar = NULL;
    g_tray_notify = NULL;
    g_hover = false;
    g_width = 0;
    g_last_w = -1;
}

static void ensure_widget(void) {
    if (g_widget && IsWindow(g_widget) && g_taskbar && IsWindow(g_taskbar)) {
        KillTimer(g_ctrl, TIMER_RETRY_BAR);
        return;
    }
    destroy_widget();

    HWND bar = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND tray = bar ? FindWindowExW(bar, NULL, L"TrayNotifyWnd", NULL) : NULL;
    if (!bar || !tray) {
        // Explorer not ready yet; retry briefly until it appears, then stop timer.
        SetTimer(g_ctrl, TIMER_RETRY_BAR, 500, NULL);
        return;
    }
    KillTimer(g_ctrl, TIMER_RETRY_BAR);

    g_taskbar = bar;
    g_tray_notify = tray;
    g_widget = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"FeatherBatWidget", L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1,
        bar, NULL, g_inst, NULL);
    if (!g_widget) {
        g_taskbar = NULL;
        g_tray_notify = NULL;
        SetTimer(g_ctrl, TIMER_RETRY_BAR, 1000, NULL);
        return;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        g_ctrl, NULL, g_inst, NULL);
    if (g_tip) {
        apply_tip_theme();
        TTTOOLINFOW ti;
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_ctrl;
        ti.uId = (UINT_PTR)g_widget;
        ti.lpszText = L"";
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }

    hook_taskbar_events();
    redraw();
}

static void apply_menu_theme(bool light) {
    typedef int (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)(void);
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    SetPreferredAppModeFn set_mode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(light ? 3 : 2);
    if (flush) flush();
}

static void show_context_menu(HWND hwnd) {
    apply_menu_theme(g_light);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    bool pt = is_pt_ui();
    bool any = false;
    for (int i = 0; i < g_dev_count; i++) {
        const Device* d = &g_devs[i];
        if (d->state == STATE_OFF) continue;
        any = true;
        wchar_t line[160];
        if (d->percent >= 0) {
            wfmt(line, ARRAYSIZE(line), L"%s  —  %d%%%s%s",
                 d->name, d->percent,
                 d->charging ? (pt ? L" (carregando)" : L" (charging)") : L"",
                 d->state == STATE_ASLEEP ? (pt ? L" (em repouso)" : L" (asleep)") : L"");
        } else if (d->charging) {
            wfmt(line, ARRAYSIZE(line), L"%s  —  %s", d->name, pt ? L"carregando" : L"charging");
        } else {
            wfmt(line, ARRAYSIZE(line), L"%s  —  %s", d->name, d->level_text[0] ? d->level_text : L"?");
        }
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
    }
    if (!any) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    pt ? L"Nenhum dispositivo detectado" : L"No devices found");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_REFRESH, pt ? L"Atualizar agora" : L"Refresh now");
    AppendMenuW(menu, MF_STRING, CMD_EXIT, pt ? L"Fechar" : L"Exit");

    POINT pt_cur;
    GetCursorPos(&pt_cur);
    SetForegroundWindow(hwnd);
    UINT cmd = (UINT)TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
        pt_cur.x, pt_cur.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == CMD_REFRESH) {
        io_refresh_now();
    } else if (cmd == CMD_EXIT) {
        DestroyWindow(hwnd);
    }
}

static LRESULT CALLBACK widget_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!g_hover) {
                g_hover = true;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                redraw();
            }
            return 0;
        case WM_MOUSELEAVE:
            if (g_hover) {
                g_hover = false;
                redraw();
            }
            return 0;
        case WM_RBUTTONUP:
            if (g_ctrl) PostMessageW(g_ctrl, WM_APP_MENU, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DESTROY:
            if (!g_quitting && hwnd == g_widget) {
                // Explorer killed/restarting: wait for TaskbarCreated (plus temporary safety retry)
                unhook_events();
                if (g_tip) { DestroyWindow(g_tip); g_tip = NULL; }
                g_widget = NULL;
                g_taskbar = NULL;
                g_tray_notify = NULL;
                g_hover = false;
                SetTimer(g_ctrl, TIMER_RETRY_BAR, 1000, NULL);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ctrl_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbar_created_msg && msg != 0) {
        ensure_widget();
        return 0;
    }
    switch (msg) {
        case WM_APP_UPDATE:
            g_dev_count = io_snapshot(g_devs, MAX_DEVICES);
            alert_check(hwnd, g_inst, g_devs, g_dev_count);
            redraw();
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            return 0;

        case WM_TIMER:
            if (wp == TIMER_RETRY_BAR) {
                ensure_widget();
            } else if (wp == 2) {
                alert_cleanup(hwnd);
            }
            return 0;

        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE: {
            bool light = read_light_theme();
            if (light != g_light) {
                g_light = light;
                apply_tip_theme();
            }
            redraw();
            return 0;
        }

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) {
                io_notify_pnp();
            }
            return TRUE;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC_VAL) {
                io_notify_pnp();
            }
            return TRUE;

        case WM_APP_TRAY_CB:
            alert_on_tray_msg(hwnd, lp);
            return 0;

        case WM_APP_MENU:
            show_context_menu(hwnd);
            return 0;

        case WM_ENDSESSION:
            if (wp) DestroyWindow(hwnd);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_quitting = true;
            KillTimer(hwnd, TIMER_RETRY_BAR);
            alert_cleanup(hwnd);
            destroy_widget();
            io_stop();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int widget_run(HINSTANCE inst) {
    g_inst = inst;
    g_light = read_light_theme();
    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ctrl_proc;
    wc.hInstance = inst;
    wc.lpszClassName = FEATHERBAT_CTRL_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = widget_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"FeatherBatWidget";
    RegisterClassExW(&wc);

    g_ctrl = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        FEATHERBAT_CTRL_CLASS, L"FeatherBat",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, inst, NULL);
    if (!g_ctrl) return 1;

    ensure_widget();
    io_start(g_ctrl);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
