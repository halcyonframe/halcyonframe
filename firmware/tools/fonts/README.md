# Status-screen typefaces

`tools/ttf2c.py` rasterizes these typefaces into `main/typefaces.c`, so the frame can draw them without a
font engine. All are licensed under the SIL Open Font License 1.1. The license texts are in this folder.

| Style in Settings | Typeface | Source |
|---|---|---|
| Clean | Inter | github.com/google/fonts/tree/main/ofl/inter |
| Warm | Fraunces | github.com/google/fonts/tree/main/ofl/fraunces |
| Dramatic | DM Serif Display | github.com/google/fonts/tree/main/ofl/dmserifdisplay |
| Classic | EB Garamond | github.com/google/fonts/tree/main/ofl/ebgaramond |
| Friendly | Outfit | github.com/google/fonts/tree/main/ofl/outfit |
| Modern | Figtree | github.com/google/fonts/tree/main/ofl/figtree |
| Technical | JetBrains Mono | github.com/google/fonts/tree/main/ofl/jetbrainsmono |

Labels and tags use Inter SemiBold in every style.

To regenerate, download the `.ttf` files from those folders into one directory, then run:

    tools/ttf2c.py <that-directory> > main/typefaces.c

The files must be saved under the names `ttf2c.py` expects: `Inter.ttf`, `Fraunces.ttf`, `DMSerifDisplay.ttf`,
`EBGaramond.ttf`, `Outfit.ttf`, `Figtree.ttf` and `JetBrainsMono.ttf`.
