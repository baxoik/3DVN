# import_narcissu.py: Narcissu 10th Anniversary Anthology Project to 3DVN

Converts the Steam release of the Narcissu anthology (Majiro engine) to 3DVN, one story
folder at a time. Only convert games you own.

    python3 tools/import_narcissu.py "<…/Narcissu 10th Anniversary Anthology Project>/narci_onetwo" games/narcissu-onetwo

| Folder | Game | Stories on its title menu |
|---|---|---|
| `narci_onetwo` | Narcissu 1st & 2nd | Narcissu, Narcissu -Side 2nd- (with its movie) |
| `narci_zero` | Narcissu Zero | Narcissu Zero, Afterword |
| `narci_iris` | Narcissu Iris | Iris, Afterword |
| `narci_sumire` | Narcissu Sumire | Sumire (with its epilogue), Akari, Character introductions |
| `narci_himeko` | Narcissu Himeko | Himeko, Afterword |

Options: `--no-movies`, `--movie-size WxH` (default 320x240), `--chapters a,b,c` (convert
these scripts as one story instead of the built-in list), and any import_vnds.py option
(for example `--no-hd`).

## What it does

- Reads the archives (`MajiroArcV3`) in place. The scripts (`scenario.arc`, `*.mjo`) are
  compiled Majiro bytecode, XORed with the CRC-32 table; they are decrypted, disassembled
  and translated to VNDS commands, then converted by import_vnds.py with NVL display
  (the original shows a full page of text): `p` waits for a tap, `w` starts a new page,
  `n` continues without a tap; a name followed by a `「「` line is that line's speaker.
- Calls are recognised by their hashes: Majiro system calls are CRC-32 of
  `$name@MAJIRO_INTER` (`$wait`, `$sound`, `$music_fadeout`, `$movie_play` …); the
  anthology's own library functions (picture, voice, music, wait) by what they are given.
- Pictures are the original 800x600 screens (the art is the wide strip in the middle).
  `b` / `w` / `black` / `white` are plain screens. Voices, music and sounds are Ogg or WAV
  (converted to Ogg Vorbis).
- A sound or picture a story's scripts name but only another story ships is taken from
  that story's folder (the original would play nothing there); the few that no folder
  has are reported and left out.
- Left out: the Japanese original scripts, the one-script "novel" versions (the same text
  again), the older Sumire translation (`narsumi*`; `3narsumi*` is used), menus and tests.

Sizes: about 160 MB (1st & 2nd), 90 MB (Zero), 60 MB (Iris), 160 MB (Sumire),
50 MB (Himeko), with the HD pictures (`--no-hd` makes them smaller for the 3DS).
