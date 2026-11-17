#pragma once
#include <stdbool.h>

enum { PTBR_FILES = 4, PTBR_ALL = (1 << PTBR_FILES) - 1 };
enum { PTBR_MISSING, PTBR_READY, PTBR_DISABLED, PTBR_INCOMPLETE, PTBR_CONFLICT, PTBR_CODE };

// Identidade do pacote separado da JumpManClub Brasil v1.1; nenhum asset e incorporado.
int localization_ptbr_file(const char *rel);
const char *localization_ptbr_path(int file);
bool localization_ptbr_matches(int file, const char *host);
int localization_ptbr_status(void);
const char *localization_ptbr_message(void);
