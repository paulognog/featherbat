#include "settings.h"

static const wchar_t* kRunKey         = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kStartupAppKey  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
static const wchar_t* kRunValue       = L"FeatherBat";
static const wchar_t* kAppKey         = L"Software\\featherbat";

static bool startup_approved_disabled(void) {
    BYTE buf[16];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kStartupAppKey, kRunValue, RRF_RT_REG_BINARY, NULL, buf, &sz) == ERROR_SUCCESS && sz >= 1) {
        // Windows Task Manager sets byte 0 to 0x03 (or 0x01/0x05..) when disabled, 0x02 when enabled
        return (buf[0] != 0x02 && buf[0] != 0x06);
    }
    return false;
}

bool settings_autostart_enabled(void) {
    DWORD sz = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, NULL, NULL, &sz) != ERROR_SUCCESS) {
        return false;
    }
    return !startup_approved_disabled();
}

bool settings_autostart_opt_out(void) {
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, kAppKey, L"AutostartOptOut", RRF_RT_REG_DWORD, NULL, &val, &sz) == ERROR_SUCCESS) {
        if (val != 0) return true;
    }
    return startup_approved_disabled();
}

static void write_opt_out(bool opt_out) {
    DWORD val = opt_out ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"AutostartOptOut", REG_DWORD, &val, sizeof(val));
}

static void set_startup_approved(bool on) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kStartupAppKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        if (on) {
            static const BYTE kEnabled[12] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            RegSetValueExW(k, kRunValue, 0, REG_BINARY, kEnabled, sizeof(kEnabled));
        } else {
            RegDeleteValueW(k, kRunValue);
        }
        RegCloseKey(k);
    }
}

void settings_set_autostart(bool on) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        if (on) {
            wchar_t target[MAX_PATH], cur[MAX_PATH], val[MAX_PATH + 4];
            if (!settings_get_install_path(target, MAX_PATH) ||
                GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
                GetModuleFileNameW(NULL, cur, MAX_PATH);
                wcpy(target, MAX_PATH, cur);
            }
            wfmt(val, ARRAYSIZE(val), L"\"%s\"", target);
            RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)val, (DWORD)((wlen(val) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, kRunValue);
        }
        RegCloseKey(k);
    }
    set_startup_approved(on);
    write_opt_out(!on);
}

bool settings_get_install_path(wchar_t* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wfmt(out, cap, L"%s\\Programs\\featherbat\\featherbat.exe", local);
    return true;
}

void settings_remove_installed_files(void) {
    wchar_t local[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wfmt(dir, MAX_PATH, L"%s\\Programs\\featherbat", local);
    wfmt(exe, MAX_PATH, L"%s\\featherbat.exe", dir);
    DeleteFileW(exe);
    RemoveDirectoryW(dir);
}

void settings_remove_all_reg(void) {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunValue);
        RegCloseKey(k);
    }
    set_startup_approved(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
}
