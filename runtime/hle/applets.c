// System library applets Odyssey can start: on the Switch these calls launch an applet and wait for it to close;
// our am service has no applets, so they would wait forever. Answered here as if the player closed them at once.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

void menu_notice_run(const char *title, const char *text);  // runtime/ui/menu.mm

// nn::web::ShowOfflineHtmlPage(OfflineHtmlPageReturnValue *out, const ShowOfflineHtmlPageArg &arg) -> nn::Result
// (an offline HTML page; StageScene polls rs::isExistRequestShowHtmlViewer). Odyssey's Action Guide (pause menu) is
// one: a page of the game's HTML manual, shown by the console's browser. That manual is a part of the game of its own,
// not in the exefs / romfs a player dumps - the player is told so. The return value starts with the exit reason: 1 =
// closed with the back button.
HLE_FUNC("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_show_offline) {
    // the page asked for, for the log: the argument is a header (8 bytes) and type / size / data entries; the first
    // string among them
    const uint8_t *arg = (const uint8_t *)(uintptr_t)c->x[1];
    char page[256] = "";
    for (size_t o = 8; arg && o + 8 <= 0x2000 && !page[0];) {
        uint16_t size;
        memcpy(&size, arg + o + 2, 2);
        if (!size || o + 8 + size > 0x2000) break;
        const char *s = (const char *)arg + o + 8;
        size_t n = strnlen(s, size);
        bool text = n >= 3 && n < size;
        for (size_t i = 0; text && i < n; i++) text = s[i] >= 0x20 && s[i] < 0x7f;
        if (text) snprintf(page, sizeof page, "%s", s);
        o += 8 + size;
    }
    LOG("[applet] web: offline HTML page %s requested (lr %llx) - not available, the player is told\n",
        page[0] ? page : "(?)", (unsigned long long)c->x[30]);
    menu_notice_run("Not available on PC",
                    "This is a page of the game's built-in manual (like the Action Guide). On the Switch the console's "
                    "web browser shows it, from a part of the game that isn't in the files you dumped (exefs and romfs) "
                    "- so Power Moon Recomp can't show it.");
    if (c->x[0]) *(uint64_t *)(uintptr_t)c->x[0] = 1;
    c->x[0] = 0;
}

// nn::err::ShowError(nn::Result): the error applet (a message box on the Switch). Logged; the game goes on.
HLE_FUNC("_ZN2nn3err9ShowErrorENS_6ResultE", err_show_error) {
    uint32_t r = (uint32_t)c->x[0];
    LOG("[applet] error applet: result %#x (module %u, description %u, lr %llx)\n", r, r & 0x1ff, (r >> 9) & 0x1fff,
        (unsigned long long)c->x[30]);
}
