// RomFS mods: folders in mods/ (in the data root) with game files that replace the game's own or add new ones, as mods
// are shipped for the Switch emulators - each in its own folder with a romfs/ inside: mods/Luigi/romfs/ObjectData/...
// Also found: the loose files of the first builds (mods/romfs/) and the console / emulator layouts
// (mods/Luigi/atmosphere/contents/0100000000010000/romfs/, mods/contents/0100000000010000/Luigi/romfs/).
// runtime/services/fs.c serves their files in place of the game's; the menu's Mods tab switches them on and off
// (powermoon.ini, applies the next time the game starts). Code patches (exefs/) can't apply: the game code is
// recompiled from the original - such mods are listed, greyed out.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[512];    // its romfs/ (or exefs/) folder relative to mods/: the key in powermoon.ini
    char name[256];  // for the menu: the folders on the way, without the layouts' own (atmosphere, contents, title id)
    bool on;         // not switched off in the menu
    bool romfs;      // has game files (no: only code patches)
    bool code;       // has code patches (exefs/): not applied
    bool applied;    // served at this start (its counts below are this start's)
    int files;       // files in its romfs/
    // of them: replacing a game file / new / also in a mod before it in the list (that one's is used) / in a folder
    // the game doesn't have (left out)
    int replaced, added, shadowed, skipped;
} ModInfo;

// One file of a switched-on mod (runtime/services/fs.c): its path in the RomFS and on the host
typedef struct { char *rel, *host; uint64_t size; int mod; bool used; } ModFile;
enum { MOD_REPLACED, MOD_ADDED, MOD_SKIPPED };

#ifdef __cplusplus
extern "C" {
#endif
int mods_count(void);  // the mods in mods/, in priority order (their names, alphabetical)
const ModInfo *mods_get(int i);
void mods_set_on(int i, bool on);  // menu: saved in powermoon.ini, applies the next time the game starts
bool mods_changed(void);           // switched on or off since the game started
int mods_applied(void);            // mods in use at this start (the crash notice mentions them)
bool mods_dir_url(char *out, size_t cap);  // file:// URL of mods/ (made if missing): the menu's "Open mods folder"
// fs.c: the switched-on mods' files, sorted by path (case-insensitive), one per path - from the first mod in the list
// that has it. mods_find looks one up; mods_tally counts what became of it (MOD_*) for the menu.
int mods_files(ModFile **out);
ModFile *mods_find(ModFile *v, int n, const char *rel);
void mods_tally(int mod, int what);
void mods_files_free(ModFile *v, int n);
#ifdef __cplusplus
}
#endif
