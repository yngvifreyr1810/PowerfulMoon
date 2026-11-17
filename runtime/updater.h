// Updates from the project's GitHub releases (runtime/updater.c; Windows): look for a newer release, download its
// Windows zip, put its files over this install's program files (the game files, saves, settings and caches are not in
// it and stay), and start the new version. A changed runtime rebuilds the game code and re-prepares shaders by itself
// (runtime/gamebuild.c, nvn/vulkan/device.c PREPARED_MARK).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UPD_UNCONFIGURED,  // no repository set (runtime/version.h APP_UPDATE_REPO): nothing to check
    UPD_IDLE,          // not checked yet
    UPD_CHECKING,
    UPD_LATEST,        // this is the newest release
    UPD_AVAILABLE,     // a newer one: latest, url
    UPD_DOWNLOADING,   // progress 0..1
    UPD_READY,         // installed: restart to use it
    UPD_FAILED,        // error
};
typedef struct {
    int state;
    char current[32], latest[32];
    char page[512];    // the release's page on GitHub
    float progress;
    bool can_install;  // a newer release with a Windows download
    char error[256];
} UpdStatus;

// main(), before anything else touches the program's files: a restart after an update waits here for the old process
// to end, then the old files left behind by the last update are removed.
void updater_startup(void);
// After the settings are loaded: the check at start (Settings.check_updates), in the background.
void updater_auto_check(void);
void updater_check(void);    // check now (background)
void updater_install(void);  // download and install the available release (background)
void updater_restart(void);  // start the new version and end this one (after UPD_READY)
void updater_status(UpdStatus *out);
const char *updater_repo(void);  // "owner/name" or ""

#ifdef __cplusplus
}
#endif
