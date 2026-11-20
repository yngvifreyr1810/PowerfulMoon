#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Cache driver information while the device is healthy; crash reporting never
// queries a lost device. A report contains text only, not process memory.
void diagnostics_init(void);
void diagnostics_gpu(const char *description);
void diagnostics_fault(const char *what, uint64_t address, uint64_t pc, uint64_t lr);
bool diagnostics_write_report(char *path, size_t capacity);   // (from the menu: not a crash)
bool diagnostics_capture_crash(char *path, size_t capacity);  // (a crash: also remembered for the next start)
void diagnostics_fault_where(const char *sym, uint64_t off);  // the faulting code (dispatch.c): its symbol and offset

// Why the game stopped, in plain words (2026-11-19): from the first exception, the log's last FATAL line and the PC's
// free memory. head: one line ("Out of graphics memory (VRAM)"), advice: what to try, detail: the error line it is
// based on. The report's first lines, the error dialog and the next start's notice all show it.
void diagnostics_reason(char *head, size_t hn, char *advice, size_t an, char *detail, size_t dn);
// The error dialog's text: the reason, what to try and the error line (the platform adds mods / the report's place)
void diagnostics_fatal_text(char *text, size_t capacity);

// The last crash's report (kept across restarts). reason: its head line; seen: its notice was shown at a start already.
bool diagnostics_last_crash(char *path, size_t capacity);
bool diagnostics_last_crash_info(char *path, size_t capacity, char *reason, size_t rn, bool *seen);
void diagnostics_last_crash_seen(void);  // (the next start's notice was shown: not again)

// Attaching a report: its text (trimmed to fit an issue comment) to the clipboard, or the file shown in the file manager
bool diagnostics_copy_report(const char *path);
int diagnostics_copy_status(void); // 1 copied, 0 queued, -1 failed
bool diagnostics_show_report(const char *path);
void diagnostics_fatal_dialog(const char *message);
