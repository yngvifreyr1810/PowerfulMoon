#pragma once
// Reading the game's files on the host, outside the guest: a file of the RomFS image (extracted/romfs.bin) by its path,
// and unpacking Nintendo's archives (Yaz0, SARC). For the shader precompile (nvn/vulkan/exec.c), which reads the game's
// shader archives before the game runs.
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

uint8_t *romfs_read_file(const char *image, const char *path, size_t *size);  // malloc'd; NULL: not there
uint8_t *yaz0_decode(const uint8_t *in, size_t n, size_t *out_n);            // malloc'd; NULL: not Yaz0 / broken
const uint8_t *sarc_find(const uint8_t *d, size_t n, const char *name, size_t *size);  // inside d; NULL: not there
typedef bool (*RomfsFileVisitor)(const uint8_t *data, size_t size, void *arg);
bool romfs_visit_files(const char *image, RomfsFileVisitor visit, void *arg);
bool sarc_visit_files(const uint8_t *data, size_t size, RomfsFileVisitor visit, void *arg);
bool nso_visit_segments(const char *path, RomfsFileVisitor visit, void *arg);
