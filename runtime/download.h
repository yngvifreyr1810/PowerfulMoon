#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Retornar false interrompe a transferencia. Implementado com o transporte do updater.
typedef bool (*DownloadProgress)(uint64_t got, uint64_t total, void *ctx);
bool download_https_file(const char *url, const char *path, uint64_t limit,
                         DownloadProgress progress, void *ctx, char *why, size_t cap);
