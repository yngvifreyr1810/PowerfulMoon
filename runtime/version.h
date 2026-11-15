// The app's version: the updater compares it with the newest release's tag (runtime/updater.c). Keep it the same as
// runtime/platform/windows.rc's FILEVERSION / ProductVersion (make dist checks).
#pragma once
#include "release_info.h"
#define APP_VERSION POWERFULMOON_VERSION

// Where updates come from: the GitHub repository ("owner/name") whose releases carry the Windows zip
// (make dist: "Super Odyssey PC Recomp-<version>-windows.zip"). Empty: no update checks (the menu says so); a fork sets its own.
// ODYSSEY_UPDATE_REPO=<owner/name> overrides it (tests).
#define APP_UPDATE_REPO "yngvifreyr1810/PowerfulMoon"
