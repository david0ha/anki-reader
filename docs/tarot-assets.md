# Tarot card assets

The firmware ships 78 pre-generated monochrome Rider–Waite–Smith card images.
They are exact-size LVGL 9 `LV_COLOR_FORMAT_I1` descriptors; the firmware does
not download, decode, resize, rotate, or dither cards at runtime.

## Provenance and license

- Source repository: [yunruse/tarot](https://github.com/yunruse/tarot)
- Source branch: `develop`
- Pinned commit: `de7fac547e15f6b210f73f30e58df0d93c212727`
- Source directory: `cards/bw`
- Declared project license: [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)

Only the 78 JPEGs in `cards/bw` are used. The repository's
`interpretations.json` is expressly excluded because its README warns that the
interpretation text is not public domain. No source JPEG is checked into this
repository.

The upstream README describes the historical card scans themselves as
"assumed to be public domain." The CC0 declaration and pinned hashes make this
source auditable, but that wording is weaker provenance than a file-by-file
Public Domain Mark. Before commercial redistribution, replace or independently
clear the masters if that distinction matters to the product.

[`tools/tarot_assets/manifest.json`](../tools/tarot_assets/manifest.json)
records the pinned Git-LFS media URL, source JPEG SHA-256, generated payload SHA-256, English and
Korean names, and conversion parameters for every card.

## Runtime format

Each descriptor is:

- 272 × 464 pixels
- 34 bytes per row, MSB first
- 8-byte BGRA8888 palette followed by 15,776 packed pixel bytes
- palette index 0: opaque white (`ff ff ff ff`)
- palette index 1: opaque black (`00 00 00 ff`)
- 15,784 bytes total per card

All 78 image payloads occupy 1,231,152 bytes before compiler/linker alignment.
The width is byte-aligned, so there are no unused tail bits. UI code must render
the descriptor at its native 272 × 464 size. LVGL cannot transform indexed I1
images, and scaling would also damage the ordered-dither pattern.

The public API is in
[`components/vault_core/include/tarot_cards.h`](../components/vault_core/include/tarot_cards.h).
It exposes card count/dimensions and lookup by stable ID. IDs are
`major-00`…`major-21` and `cups|pentacles|swords|wands-01`…`14`.
Unknown or null IDs return `NULL`.

## Regeneration

Regeneration is a maintainer-only operation. A normal firmware or simulator
build uses the checked-in `major.c`, `cups.c`, `pentacles.c`, `swords.c`,
`wands.c`, and `catalog.c`; it needs neither network access nor ImageMagick.

The generator deliberately accepts a local checkout instead of downloading
sources itself, preventing an upstream branch move from silently changing
generated firmware assets.

```sh
git clone --branch develop https://github.com/yunruse/tarot.git /tmp/yunruse-tarot
git -C /tmp/yunruse-tarot checkout de7fac547e15f6b210f73f30e58df0d93c212727
python3 tools/tarot_assets/generate.py \
  --source-root /tmp/yunruse-tarot \
  --output-root .
python3 tools/test_tarot_assets.py
```

The generator rejects a different Git commit, a dirty `cards/bw` directory, or
an ImageMagick release other than `ImageMagick 7.1.2-3`. Its fixed transform is:

```text
magick {source} -auto-orient -colorspace Gray -filter Lanczos \
  -resize 272x464! -ordered-dither o4x4 -depth 8 gray:-
```

The raw output must contain only byte values 0 and 255. The generator then packs
black pixels as index-1 bits in MSB-first order and prepends the fixed palette.
Tests compile all six generated C translation units against the public API and
also validate every descriptor, byte count, palette, manifest entry, payload
hash, and missing-ID behavior.
