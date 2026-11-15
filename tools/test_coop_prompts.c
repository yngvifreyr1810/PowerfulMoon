// Read the owner's font and exercise the actual atlas builder and game wrappers.
// No game files are written or included in this test's source/output.
#include <assert.h>
#include "../runtime/nvn/kbprompts.c"

Settings g_settings = {.prompts = PROMPTS_CONTROLLER};
const char *g_romfs_dir;
int16_t g_bind_pad[BIND_PAD_COUNT] = {1,0,3,2,7,8,9,10,26,27,6,4,13,11,14,12};
int16_t g_bind_key[BIND_KEY_COUNT];
static int primary_family = 2, secondary_family;
int input_pad_family(void) { return primary_family; }
int input_cappy_family(void) { return secondary_family; }
bool input_cappy_assigned(void) { return true; }
int bind_prompt_host(int slot) { return g_bind_pad[slot]; }
const char *bind_pad_label(int host, int family) { (void)host; return family == 2 ? "L1" : "LB"; }
bool rt_resource(const char *name, const char *dev, char *out, size_t n) { (void)name;(void)dev;(void)out;(void)n;return false; }
bool loader_main_is_v141(void) { return false; }
GuestFn dispatch_lookup(uint64_t addr) { (void)addr; return NULL; }
void *hle_alloc(uint64_t bytes) { return calloc(1, bytes); }
void odyssey_coop_find_glyph(Cpu *, GuestFn);
void odyssey_coop_set_tutorial(Cpu *, GuestFn);
void odyssey_coop_set_pane(Cpu *, GuestFn);


static unsigned cell_diff(const uint8_t *a, unsigned ga, const uint8_t *b, unsigned gb) {
    unsigned differences = 0, width = ref.w, height = ref.h;
    for (unsigned y = 0; y < ref.cellh; y++) for (unsigned x = 0; x < ref.cellw; x++) {
        unsigned ax = ga % ref.cols * (ref.cellw + 1) + x, ay = height - 1 - (ga / ref.cols * (ref.cellh + 1) + y);
        unsigned bx = gb % ref.cols * (ref.cellw + 1) + x, by = height - 1 - (gb / ref.cols * (ref.cellh + 1) + y);
        uint8_t pa[16], pb[16];
        bc4_decode(a + ((ay / 4) * (width / 4) + ax / 4) * 8, pa);
        bc4_decode(b + ((by / 4) * (width / 4) + bx / 4) * 8, pb);
        differences += pa[(ay % 4) * 4 + ax % 4] != pb[(by % 4) * 4 + bx % 4];
    }
    return differences;
}
static void fake_find(Cpu *c) { c->x[0] = c->x[0] == 1 && c->x[1] == 0xe0e1 ? 9 : 0xffff; }
static const uint16_t tagged[] = {0xe0e1,' ',0xe,0,1,2,0,0xe0e3,0};
static unsigned pane_calls;
static void fake_pane(Cpu *c) {
    const uint16_t *s = (const void *)(uintptr_t)c->x[2];
    bool right = !strcmp((const char *)(uintptr_t)c->x[1], "TxtGuideR");
    assert(s[0] == (right ? 0xe0b9 : 0xe0e1));
    assert(!memcmp(s + 2, tagged + 2, 5 * 2)); // tags retain embedded zero parameters
    assert(s[7] == (right ? 0xe0bb : 0xe0e3) && !s[8]);
    pane_calls++;
}
static void fake_tutorial(Cpu *c) {
    c->x[0] += 8; c->x[1] = (uintptr_t)"TxtGuideR"; c->x[2] = (uintptr_t)tagged;
    odyssey_coop_set_pane(c, fake_pane);
    c->x[1] = (uintptr_t)"TxtGuideL"; c->x[2] = (uintptr_t)tagged;
    odyssey_coop_set_pane(c, fake_pane);
}
static const uint16_t icon_tag[] = {0xe,6,0x1a,54,
    22,'D','u','a','l','B','u','t','t','o','n','Y',
    28,'F','u','l','l','K','e','y','B','u','t','t','o','n','Y',' ',0};
static void icon_pane(Cpu *c) {
    const uint16_t *s = (const void *)(uintptr_t)c->x[2];
    const uint16_t glyph[] = {0xe,9,7,4,0,0xe0bb,' ',0};
    assert(!memcmp(s,glyph,sizeof glyph));
    assert(icon_tag[4] == 22 && icon_tag[5] == 'D');
}
static void icon_tutorial(Cpu *c) {
    c->x[0] += 8; c->x[1] = (uintptr_t)"TxtGuideR"; c->x[2] = (uintptr_t)icon_tag;
    odyssey_coop_set_pane(c, icon_pane);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[2048]; snprintf(path,sizeof path,"%s/romfs",argv[1]); g_romfs_dir = path;
    load_ref(); assert(ref.state == 1 && ref.glyphs == 39 && ref.cols * ref.rows == 72);
    size_t bytes = (size_t)ref.w * ref.h / 2;
    uint8_t *original = malloc(bytes), *patched = malloc(bytes);
    // Controlled source coverage in B's cell; use the owner's actual font map
    // and dimensions, but no proprietary glyph image in the fixture.
    for (unsigned by = 0; by < ref.h / 4; by++) for (unsigned bx = 0; bx < ref.w / 4; bx++) {
        uint8_t pixels[16];
        for (unsigned p = 0; p < 16; p++) {
            int x = bx * 4 + p % 4 - 3 * 81, y = ref.h - 1 - (by * 4 + p / 4) - 85;
            pixels[p] = x >= 10 && x < 65 && y >= 8 && y < 75 &&
                (x < 20 || y < 18 || (y >= 36 && y < 46) || y >= 65 || x >= 55) ? 255 : 0;
        }
        bc4_encode(pixels, original + (by * (ref.w / 4) + bx) * 8);
    }
    memcpy(patched,original,bytes); kbprompts_apply(0,patched,ref.w / 4,ref.h / 4);
    assert(cell_diff(patched,9,original,9) > 100); // Mario's B -> PlayStation Cross
    assert(cell_diff(patched,48,original,9) == 0); // Cappy's B remains the Switch B
    primary_family = 0; secondary_family = 2;
    memcpy(patched,original,bytes); kbprompts_apply(0,patched,ref.w / 4,ref.h / 4);
    assert(cell_diff(patched,9,original,9) == 0);
    assert(cell_diff(patched,48,original,9) > 100); // hot-switch reverses the two families
    assert(kbprompts_cappy_code(0xe0e1) == 0xe0b9 && kbprompts_cappy_code('B') == 'B');
    Cpu c = {0}; c.x[0] = 1; c.x[1] = 0xe0b9;
    odyssey_coop_find_glyph(&c,fake_find); assert(c.x[0] == 48);
    c.x[0] = 2; c.x[1] = 0xe0b9;
    odyssey_coop_find_glyph(&c,fake_find); assert(c.x[0] == 0xffff); // regular fonts keep fallback lookup
    c.x[0] = 0x1000;
    odyssey_coop_set_tutorial(&c, fake_tutorial); assert(pane_calls == 2);
    assert(tagged[0] == 0xe0e1 && tagged[7] == 0xe0e3); // source text is unchanged
    c.x[0] = 0x1000; odyssey_coop_set_tutorial(&c, icon_tutorial);
    free(original); free(patched);
    puts("PASS: mixed atlas cells, hot family changes, font fallback, pane scope and tagged text");
    return 0;
}
