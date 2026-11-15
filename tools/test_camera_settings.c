// Real settings parser/serializer fixture. No game files or controller required.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "settings.h"

bool plat_is_steam_deck(void) { return false; }
uint64_t plat_total_ram(void) { return 16ull << 30; }
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *f) { (void)f; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifdef _WIN32
    _putenv_s("ODYSSEY_SETTINGS", argv[1]);
#else
    setenv("ODYSSEY_SETTINGS", argv[1], 1);
#endif
    settings_load();
    printf("{\"mouse\":[%d,%d,%d,%d],\"stick\":[%d,%d,%d,%d],\"motion\":%d}\n",
        g_settings.mouse_sensitivity, g_settings.mouse_sensitivity_y,
        g_settings.mouse_invert_x, g_settings.mouse_invert_y,
        g_settings.stick_camera_x, g_settings.stick_camera_y,
        g_settings.stick_camera_invert_x, g_settings.stick_camera_invert_y, g_settings.motion);
    settings_save();
    return 0;
}
