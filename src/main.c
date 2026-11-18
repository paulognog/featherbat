#include "widget.h"
#include "io.h"

static const wchar_t* kRunKey   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"FeatherBat";
static const wchar_t* kAppKey   = L"Software\\featherbat";

static void set_autostart(bool on) {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            wchar_t exe[MAX_PATH], val[MAX_PATH + 4];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            wfmt(val, ARRAYSIZE(val), L"\"%s\"", exe);
            RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)val, (DWORD)((wlen(val) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, kRunValue);
        }
        RegCloseKey(k);
    }
}

static bool autostart_opt_out(void) {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"AutostartOptOut", RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        return val != 0;
    }
    return false;
}

static void quit_running_instance(void) {
    HWND w = FindWindowW(FEATHERBAT_CTRL_CLASS, NULL);
    if (w) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 30 && FindWindowW(FEATHERBAT_CTRL_CLASS, NULL); i++) Sleep(100);
    }
}

static int do_uninstall(void) {
    quit_running_instance();
    set_autostart(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
    con_init();
    con_puts(L"FeatherBat uninstalled from autostart and settings removed.\n");
    con_close();
    return 0;
}

void __attribute__((noreturn)) wWinMainCRTStartup(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    const wchar_t* cmd = GetCommandLineW();

    int rc = 0;
    if (cmd && wcontains_i(cmd, L"--dump")) {
        rc = io_run_dump();
        ExitProcess((UINT)rc);
    }
    if (cmd && wcontains_i(cmd, L"--once")) {
        rc = io_run_once();
        ExitProcess((UINT)rc);
    }
    if (cmd && wcontains_i(cmd, L"--quit")) {
        quit_running_instance();
        ExitProcess(0);
    }
    if (cmd && wcontains_i(cmd, L"--uninstall")) {
        rc = do_uninstall();
        ExitProcess((UINT)rc);
    }

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\FeatherBatMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        ExitProcess(0);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if ((!cmd || !wcontains_i(cmd, L"--no-autostart")) && !autostart_opt_out()) {
        set_autostart(true);
    }

    rc = widget_run(inst);
    if (mtx) {
        ReleaseMutex(mtx);
        CloseHandle(mtx);
    }
    ExitProcess((UINT)rc);
}
