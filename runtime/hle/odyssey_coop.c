// Keep co-op camera arbitration and tutorial icons separate for each player.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../rt.h"
#include "../hle.h"
#include "../input_players.h"
#include "../nvn/kbprompts.h"

static GuestFn near_zero_original;
static _Thread_local uint64_t tutorial_layout;
// Both controllers are exposed to the game as FullKey. Resolve only known
// controller icons in Cappy's tutorial; other tags keep the game's processor.
static uint16_t cappy_tag_icon(const uint16_t *tag, size_t words, uint16_t *type) {
    static const struct { const char *name; uint16_t code, type; } icons[] = {
        {"FullKeyButtonA",0xe0e0,4}, {"FullKeyButtonB",0xe0e1,5},
        {"FullKeyButtonX",0xe0e2,6}, {"FullKeyButtonY",0xe0e3,7},
        {"FullKeyButtonL",0xe0e4,8}, {"FullKeyButtonR",0xe0e5,9},
        {"FullKeyButtonZL",0xe0e6,10}, {"FullKeyButtonZR",0xe0e7,11},
        {"FullKeyStickL",0xe101,24}, {"FullKeyStickR",0xe102,25},
    };
    for (size_t k = 4; k < words;) {
        size_t bytes = tag[k++], len = bytes / 2;
        if ((bytes & 1) || k + len > words) return 0;
        for (unsigned a = 0; a < sizeof icons / sizeof *icons; a++) {
            size_t j = 0;
            if (strlen(icons[a].name) != len) continue;
            while (j < len && tag[k+j] == (uint8_t)icons[a].name[j]) j++;
            if (j == len) { *type = icons[a].type; return kbprompts_cappy_code(icons[a].code); }
        }
        k += len;
    }
    return 0;
}

void odyssey_coop_near_zero(Cpu *c) {
    // The game's co-op branch tested Mario against 0.001, but discarded camera
    // vectors below 0.03 afterwards. Drift in that gap prevented trying Cappy.
    // Change only that call: capture restrictions and Mario's priority remain
    // in the original game code. Movement and unrelated near-zero tests stay.
    if ((input_cappy_assigned() && !getenv("ODYSSEY_COOP_CAMERA_FIX_OFF")) || !near_zero_original) {
        // (also when the game's own helper wasn't found at boot: the same test, rather than calling nothing)
        const float *v = (const float *)(uintptr_t)c->x[0];
        float length2 = v[0] * v[0] + v[1] * v[1];
        c->x[0] = isfinite(length2) && length2 < 0.03f * 0.03f;
        static unsigned traced;
        if (getenv("ODYSSEY_LOG_CAMERA") && length2 > 0.001f * 0.001f && c->x[0] && traced++ < 8)
            LOG("[coop-camera] Mario drift %.5f: checking Cappy\n", sqrtf(length2));
        return;
    }
    near_zero_original(c);
}

void odyssey_coop_find_glyph(Cpu *c, GuestFn original_function) {
    uint32_t original;
    uint16_t source, index;
    if (kbprompts_cappy_glyph((uint32_t)c->x[1], &original, &source, &index)) {
        Cpu probe = *c;
        probe.x[1] = original;
        original_function(&probe);
        static unsigned traces;
        if (getenv("ODYSSEY_CAPPY_TRACE") && traces++ < 12)
            LOG("[coop-font] code %x original %x index %llx target %u\n", (unsigned)c->x[1], original, (unsigned long long)probe.x[0], index);
        // Only the DeviceFont maps this code to the known source cell. Let
        // ordinary fonts fail normally so PairFont selects the right fallback.
        if (probe.x[0] == source) { c->x[0] = index; return; }
    }
    original_function(c);
}

void odyssey_coop_set_pane(Cpu *c, GuestFn original_function) {
    const char *name = (const char *)(uintptr_t)c->x[1];
    if (tutorial_layout && c->x[0] == tutorial_layout + 8 && name &&
        (!strcmp(name, "TxtGuideR") || !strcmp(name, "TxtGuideRSh"))) {
        const uint16_t *source = (const uint16_t *)(uintptr_t)c->x[2];
        static _Thread_local uint16_t copy[4096];
        size_t i = 0, out = 0;
        unsigned changed = 0;
        while (source && out + 1 < sizeof copy / sizeof *copy) {
            uint16_t ch = source[i];
            if (ch == 0xe || ch == 0xf) {
                size_t words = ch == 0xe ? 4 + (source[i + 3] + 1u) / 2 : 3;
                if (out + words >= sizeof copy / sizeof *copy) break;
                uint16_t type = 0;
                uint16_t icon = ch == 0xe && source[i+1] == 6 &&
                    (source[i+2] == 0x1a || source[i+2] == 0x1b) ? cappy_tag_icon(source+i,words,&type) : 0;
                if (icon) {
                    // Preserve the device-font tag: a bare code would be
                    // rendered through the ordinary text font as '?'.
                    uint16_t glyph_tag[] = {0xe,9,type,4,0,icon};
                    memcpy(copy+out,glyph_tag,sizeof glyph_tag); out += 6; changed++;
                }
                else { memcpy(copy + out, source + i, words * 2); out += words; }
                i += words;
                continue;
            }
            copy[out] = kbprompts_cappy_code(ch);
            changed += copy[out++] != ch;
            i++;
            if (!ch) {
                if (changed) c->x[2] = (uintptr_t)copy;
                break;
            }
        }
        if (getenv("ODYSSEY_CAPPY_TRACE")) LOG("[coop-prompts] %s: %u separate icons\n", name, changed);
    }
    original_function(c);
}

void odyssey_coop_set_tutorial(Cpu *c, GuestFn original_function) {
    uint64_t previous = tutorial_layout;
    tutorial_layout = input_cappy_assigned() ? c->x[0] : 0;
    original_function(c);
    tutorial_layout = previous;
}

void odyssey_coop_boot(void) {
    bool v141 = loader_main_is_v141();
    uint64_t near = v141 ? 0x700068fe30ull : 0x700091ddecull;
    near_zero_original = dispatch_lookup(near);
    LOG("[coop] compiled prompt wrappers; camera helper %s\n", near_zero_original ? "ready" : "unavailable");
}
