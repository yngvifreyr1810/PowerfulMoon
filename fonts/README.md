# System fonts

The game draws its text with the Switch's built-in system font, which isn't part of the game files.
Without it, Power Moon Recomp uses a Japanese gothic in the same style as the Switch's Morisawa UD Shin Go (Yu Gothic on
Windows, Hiragino Sans on macOS), then Arial.

For the exact look, put your own copies of the system fonts here:

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or spelled `nintendo_udsgr_std_003`, also works)
- `nintendo_ext_003.bfttf`: button glyphs (optional)

`ODYSSEY_FONT=<file>` overrides everything (.ttf / .otf / .ttc / .bfttf).
