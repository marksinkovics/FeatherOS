FeatherOS fonts

The .ffnt files in this folder are bitmap fonts in FeatherOS's own
FFNT format. tools/fonts/generate.py pre-rendered them from the
TrueType fonts below, which are licensed under the SIL Open Font
License 1.1. They are not the original fonts and are named mono-N
and sans-N after their family kind and line height in pixels.

mono-*.ffnt: derived from JetBrains Mono Regular 2.304
  Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)
  Licence: LICENSE-JetBrainsMono.txt

sans-*.ffnt: derived from Inter Regular 4.0
  Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
  Licence: LICENSE-Inter.txt

sample.txt is plain text for the FONTS smoke check, not a font:
characters the console font has, and some invalid UTF-8 bytes.

The font files are under the SIL Open Font License 1.1, not under
the licence of the rest of FeatherOS.
SHA256SUMS lists the SHA-256 of every other file in this folder.
