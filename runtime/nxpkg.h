// Reading the player's own game package (an NSP or XCI dumped from their console) in place, with their own prod.keys:
// the base game's ExeFS (code modules) and its RomFS image, decrypted and verified (runtime/nxpkg.c). Used by the
// setup instead of an extracted exefs/romfs folder. Key values are only ever held in memory and never logged.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NxPkg NxPkg;

// Opens the package and finds the base game's Program NCA (title 0100000000010000). NULL: not usable (why: a short
// message for the player). keys: the player's prod.keys.
NxPkg *nxpkg_open(const char *package, const char *keys, char *why, size_t why_size);
// The same for an update package (its Program NCA patches the base game: title 0100000000010800's).
NxPkg *nxpkg_open_update(const char *package, const char *keys, char *why, size_t why_size);
void nxpkg_close(NxPkg *p);

// The ExeFS files (main, main.npdm, rtld, sdk, subsdk0) into the folder dir (created), every block hash-checked.
bool nxpkg_extract_exefs(NxPkg *p, const char *dir, char *why, size_t why_size);

// Bytes of the RomFS image (the game data).
uint64_t nxpkg_romfs_size(NxPkg *p);

// The RomFS image to the file out, all six hash levels verified. progress (may be NULL): bytes done, total.
bool nxpkg_extract_romfs(NxPkg *p, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                         size_t why_size);

// The update's RomFS image (its BKTR section over the base game's RomFS) to the file out, all six hash levels of the
// result verified. nxpkg_romfs_size(update) is its size.
bool nxpkg_extract_romfs_patched(NxPkg *base, NxPkg *update, const char *out, void (*progress)(uint64_t done, uint64_t total),
                                 char *why, size_t why_size);

// Copies the folder `dir` (e.g. "MovieData") out of a RomFS image file into the folder out (created).
bool romfs_image_extract_dir(const char *image, const char *dir, const char *out, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
