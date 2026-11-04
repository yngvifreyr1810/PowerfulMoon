// SPDX-License-Identifier: MIT
// Public loopback protocol only; no bridge/GPL source is linked into this host.
#pragma once
#include "controller_motion.h"
// All calls are serialized by the controller mutex.
void pm_hd_poll(uint64_t now, bool eligible);
bool pm_hd_active(uint64_t now);
void pm_hd_send(const PMVibration values[2], uint64_t now);
void pm_hd_close(void);
