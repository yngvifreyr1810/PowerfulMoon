#pragma once
#include <stdbool.h>

enum { PTBR_INSTALL_IDLE, PTBR_INSTALL_DOWNLOAD, PTBR_INSTALL_VERIFY, PTBR_INSTALL_EXTRACT,
       PTBR_INSTALL_PUBLISH, PTBR_INSTALL_READY, PTBR_INSTALL_FAILED, PTBR_INSTALL_CANCELLED };
typedef struct {
    int state;
    float progress;
    char error[256];
} PtbrInstallStatus;

#ifdef __cplusplus
extern "C" {
#endif
void ptbr_install_status(PtbrInstallStatus *out);
bool ptbr_install_start(const char *data_root);
// O mesmo fluxo com um pacote local: usado por testes sem rede, sempre com checksum fixo.
bool ptbr_install_local(const char *data_root, const char *archive);
void ptbr_install_cancel(void);
bool ptbr_install_busy(void);
#ifdef __cplusplus
}
#endif
