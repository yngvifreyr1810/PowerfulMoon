// The menu's own text in other languages (runtime/ui/i18n.mm): tr("English") is the text in the menu's language, or
// the English itself when there's no translation (or the menu is in English). Translations: lang/<code>.txt next to the
// app (runtime/ui/lang/ in a checkout), one per line, "English<TAB>translation", "\n" for line breaks.
// ODYSSEY_I18N_DUMP=<file>: every text asked for is appended there once (to see what a translation needs).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *tr(const char *en);
// the menu's language code ("en", "ko"), from the setting (Auto: the game's language when there's a translation)
const char *i18n_lang(void);
// every translated text, one after another (the font's glyphs for it are made from these); NULL: English
const char *i18n_all_text(void);
#ifdef __cplusplus
}
#endif
