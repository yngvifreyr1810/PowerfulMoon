// SDK assertions and aborts (nn::diag). The SDK checks its arguments and state and stops the game with a message
// (condition, function, source file and line, a printf-style text) that went nowhere: print it, then stop. These are
// called from inside the SDK, so they are replaced at generation time (runtime/hle/hle_symbols.txt lists them;
// tools/gen.py makes each one's body a call to hle_<symbol>).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

static const char *gstr(uint64_t a) {
    return a >= GUEST_LO && a < GUEST_HI ? (const char *)(uintptr_t)a : "(null)";
}

// printf of a guest format string whose arguments start at integer register `reg` (then the stack, per AAPCS64)
static void format_guest(Cpu *c, const char *fmt, int reg, char *out, size_t n) {
    int stack = 0;
    size_t o = 0;
#define NEXT() (reg < 8 ? c->x[reg++] : ((uint64_t *)(uintptr_t)c->sp)[stack++])
    for (const char *p = fmt; *p && o + 1 < n; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[16] = "%";
        int k = 1;
        while (p[1] && strchr("-+ #0123456789.lhzjt", p[1]) && k < 12) spec[k++] = *++p;
        char conv = *++p;
        if (!conv) break;
        int w;
        if (conv == '%') { out[o++] = '%'; continue; }
        if (conv == 's') { spec[k++] = 's'; w = snprintf(out + o, n - o, spec, gstr(NEXT())); }
        else if (strchr("dioxXuc", conv)) {  // (as 64-bit: the l / h modifiers say how much of it was meant)
            k = 1;
            while (spec[k] && !strchr("lhzjt", spec[k])) k++;
            spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
            w = snprintf(out + o, n - o, spec, (long long)NEXT());
        } else if (conv == 'p') w = snprintf(out + o, n - o, "0x%llx", (unsigned long long)NEXT());
        else w = snprintf(out + o, n - o, "%%%c", conv);  // (floats: not decoded)
        if (w < 0) break;
        o += (size_t)w < n - o ? (size_t)w : n - o - 1;
    }
#undef NEXT
    out[o] = 0;
}

static void report(Cpu *c, const char *what, uint64_t cond, uint64_t fn, uint64_t file, int line, uint64_t result,
                   uint64_t fmt, int first_arg) {
    char text[1024] = "";
    if (fmt) format_guest(c, gstr(fmt), first_arg, text, sizeof text);
    LOG("\n[diag] %s: %s\n", what, cond ? gstr(cond) : "");
    if (fn) LOG("[diag]   in %s\n", gstr(fn));
    if (file) LOG("[diag]   at %s:%d\n", gstr(file), line);
    if (result) {
        uint32_t r = *(uint32_t *)(uintptr_t)result;
        LOG("[diag]   result %#x (module %u, description %u)\n", r, r & 0x1ff, (r >> 9) & 0x1fff);
    }
    if (text[0]) LOG("[diag]   %s\n", text);
    kernel_dump_threads();  // (what the other threads were doing: races between them, e.g. on a heap without a lock)
    FATAL("SDK %s (see [diag] lines)\n", what);
}

// OnAssertionFailure(AssertionType, condition, function, file, line[, format, ...])
void hle__ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_iS4_z(Cpu *c) {
    report(c, "assertion failed", c->x[1], c->x[2], c->x[3], (int)c->x[4], 0, c->x[5], 6);
}
void hle__ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_i(Cpu *c) {
    report(c, "assertion failed", c->x[1], c->x[2], c->x[3], (int)c->x[4], 0, 0, 0);
}
// AbortImpl(condition, function, file, line[, const Result *][, const UserExceptionInfo *][, format, ...])
void hle__ZN2nn4diag6detail9AbortImplEPKcS3_S3_i(Cpu *c) {
    report(c, "abort", c->x[0], c->x[1], c->x[2], (int)c->x[3], 0, 0, 0);
}
void hle__ZN2nn4diag6detail9AbortImplEPKcS3_S3_iS3_z(Cpu *c) {
    report(c, "abort", c->x[0], c->x[1], c->x[2], (int)c->x[3], 0, c->x[4], 5);
}
void hle__ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z(Cpu *c) {
    report(c, "abort", c->x[0], c->x[1], c->x[2], (int)c->x[3], c->x[4], c->x[5], 6);
}
void hle__ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_z(Cpu *c) {
    report(c, "abort", c->x[0], c->x[1], c->x[2], (int)c->x[3], c->x[4], c->x[6], 7);
}
void hle__ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_St9__va_list(Cpu *c) {
    report(c, "abort", c->x[0], c->x[1], c->x[2], (int)c->x[3], c->x[4], c->x[6], 8);  // (va_list: not decoded)
}

// sead, the game's engine library (in main): its halts, and its debug output if the game prints any

// A sead::Heap and its parents (ref/sead heap/seadHeap.h; offsets as Odyssey's ExpHeap::tryAlloc uses them): the name
// (SafeString +0x20: its text at +0x28), start +0x38, size +0x40, parent +0x48.
static void dump_heap_chain(const char *why, uint64_t h) {
    for (int i = 0; i < 8 && h >= GUEST_LO && h < GUEST_HI && !(h & 7); i++) {
        uint64_t vt = LD64(h);
        if (vt < MAIN_BASE || vt >= SUBSDK0_BASE) break;  // (a vtable in main: else not a heap)
        LOG("[diag]   %s heap %llx \"%s\" start %llx size %#llx flags %#x\n", i ? "  parent" : why, (unsigned long long)h,
            gstr(LD64(h + 0x28)), (unsigned long long)LD64(h + 0x38), (unsigned long long)LD64(h + 0x40),
            LD16(h + 0xd8));  // (bit 0: the heap locks its CriticalSection at +0x98)
        h = LD64(h + 0x48);
    }
}

// HaltWithDetail(file, line, format, ...)
void hle__ZN4sead6system14HaltWithDetailEPKciS2_z(Cpu *c) {
    report(c, "sead halt", 0, 0, c->x[0], (int)c->x[1], 0, c->x[2], 3);
}
// The ExpHeap's own view of itself (its functions, called on this thread): full, or its block lists broken?
static void dump_exp_heap(Cpu *c, uint64_t h) {
    static const char *const fns[] = {"_ZNK4sead7ExpHeap11getFreeSizeEv", "_ZNK4sead7ExpHeap21getMaxAllocatableSizeEi",
                                      "_ZNK4sead7ExpHeap16tryCheckFreeListEv", "_ZNK4sead7ExpHeap15tryCheckUseListEv"};
    uint64_t r[4] = {0};
    for (int i = 0; i < 4; i++) {
        uint64_t f = loader_find_symbol(fns[i]);
        if (!f) return;
        c->x[0] = h, c->x[1] = 8;
        guest_call(c, f);
        r[i] = c->x[0];
    }
    LOG("[diag]   ExpHeap free %#llx, largest block %#llx, free list %s, use list %s\n", (unsigned long long)r[0],
        (unsigned long long)r[1], r[2] & 1 ? "ok" : "BROKEN", r[3] & 1 ? "ok" : "BROKEN");
}

// (ExpHeap::tryAlloc's failure callback halts: it keeps the heap in x21, the size in x20)
void hle__ZN4sead6system4HaltEv(Cpu *c) {
    uint64_t heap = c->x[21], size = c->x[20];
    LOG("\n[diag] sead halt (x20 %#llx)\n", (unsigned long long)size);
    dump_heap_chain("x21", heap);
    if (heap >= GUEST_LO && heap < GUEST_HI) dump_exp_heap(c, heap);
    report(c, "sead halt", 0, 0, 0, 0, 0, 0, 0);
}
// PrintStringImpl(text, length)
void hle__ZN4sead6system15PrintStringImplEPKci(Cpu *c) {
    int n = (int)c->x[1];
    const char *s = gstr(c->x[0]);
    if (n < 0) n = (int)strlen(s);
    LOG("[sead] %.*s%s", n, s, n && s[n - 1] == '\n' ? "" : "\n");
}
