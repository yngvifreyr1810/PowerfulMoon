// Parser, persistencia e descoberta reais; os testes usam pastas temporarias.
#include <stdio.h>
#include <stdint.h>

#include "settings.h"
#include "mods.h"
#include "localization.h"

bool plat_is_steam_deck(void) { return false; }
uint64_t plat_total_ram(void) { return 16ull << 30; }
bool bind_settings_load(const char *key, const char *value) { (void)key; (void)value; return false; }
void bind_settings_save(FILE *f) { (void)f; }

int main(void) {
    settings_load();
    ModFile *files = NULL;
    int n = mods_files(&files);
    printf("{\"language\":%d,\"code\":\"%s\",\"name\":\"%s\",\"status\":%d,\"mods\":%d,\"files\":%d}\n",
           g_settings.language, settings_language(), settings_language_name(), localization_ptbr_status(), mods_count(), n);
    mods_files_free(files, n);
    settings_save();
    return 0;
}
