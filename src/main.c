#include "widget.h"
#include "io.h"
#include "settings.h"

static void quit_running_instance(void) {
    HWND w = FindWindowW(FEATHERBAT_CTRL_CLASS, NULL);
    if (w) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        for (int i = 0; i < 30 && FindWindowW(FEATHERBAT_CTRL_CLASS, NULL); i++) Sleep(100);
    }
}

static bool try_self_install_and_relaunch(void) {
    wchar_t cur[MAX_PATH], target[MAX_PATH], local[MAX_PATH], dir[MAX_PATH];
    if (!GetModuleFileNameW(NULL, cur, MAX_PATH)) return false;
    if (!settings_get_install_path(target, MAX_PATH)) return false;
    if (weqi(cur, target)) return false;

    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wfmt(dir, MAX_PATH, L"%s\\Programs", local);
    CreateDirectoryW(dir, NULL);
    wfmt(dir, MAX_PATH, L"%s\\Programs\\featherbat", local);
    CreateDirectoryW(dir, NULL);

    // Close any older running instance so the installed binary can be updated cleanly
    quit_running_instance();

    if (!CopyFileW(cur, target, FALSE)) return false;

    if (!settings_autostart_opt_out()) {
        settings_set_autostart(true);
    }

    wchar_t cmdline[MAX_PATH + 24];
    wfmt(cmdline, ARRAYSIZE(cmdline), L"\"%s\" --installed", target);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(target, cmdline, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static int do_uninstall(void) {
    quit_running_instance();
    settings_remove_all_reg();
    settings_remove_installed_files();
    con_init();
    con_puts(is_pt_ui()
        ? L"FeatherBat removido da inicializacao e pasta instalada limpa.\n"
        : L"FeatherBat uninstalled from autostart and installed folder removed.\n");
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

    bool no_autostart = (cmd && wcontains_i(cmd, L"--no-autostart"));
    bool installed_flag = (cmd && wcontains_i(cmd, L"--installed"));
    if (!no_autostart && !installed_flag) {
        if (try_self_install_and_relaunch()) {
            ExitProcess(0);
        }
    }

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\FeatherBatMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        ExitProcess(0);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!no_autostart && !settings_autostart_opt_out()) {
        settings_set_autostart(true);
    }

    rc = widget_run(inst);
    if (mtx) {
        ReleaseMutex(mtx);
        CloseHandle(mtx);
    }
    ExitProcess((UINT)rc);
}
