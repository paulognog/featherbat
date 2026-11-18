#pragma once

#include "mini.h"

#ifdef __cplusplus
extern "C" {
#endif

bool settings_autostart_enabled(void);
bool settings_autostart_opt_out(void);
void settings_set_autostart(bool on);
bool settings_get_install_path(wchar_t* out, size_t cap);
void settings_remove_installed_files(void);
void settings_remove_all_reg(void);

#ifdef __cplusplus
}
#endif
