#!/usr/bin/env python3
"""
Regenerate the Korean fonts in components/vault_core/fonts/.

Why this is different from a subset generator
---------------------------------------------
The board this project forked from could subset its fonts down to seventy
glyphs, because every string it drew was a literal in its own source. This board
draws note titles, tag names, agent names and inbox items that arrive from the
network at runtime. There is no symbol list that can be derived ahead of time,
and the failure mode of guessing is a tofu box on somebody's note title — on the
glass, after a two-second refresh, where nobody is watching.

So both faces carry the whole 완성형 set: the 2350 Hangul syllables of
KS X 1001, plus ASCII, plus the punctuation the UI composes at runtime.

The 2350 are not a hardcoded table. They are exactly the syllables reachable
through the EUC-KR encoding's Hangul rows (0xB0A1-0xC8FE), so Python's own codec
generates them — 25 lead bytes x 94 trail bytes = 2350, no data file to rot.

At 1 bpp this costs roughly 100 KB of flash per face against an 8 MB app
partition. 1 bpp and not 4: the panel binarizes everything anyway, so
anti-aliasing would cost four times the flash to produce pixels that are then
thresholded straight back to black and white.

Usage
-----
    python3 tools/gen_fonts.py --download          # fetch Noto Sans KR itself
    python3 tools/gen_fonts.py --font /path/NotoSansKR-Regular.otf \\
                               --font-medium /path/NotoSansKR-Medium.otf

Needs node/npx (it shells out to lv_font_conv). The generated .c files are
committed, so a normal build never runs this.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "components", "vault_core")
FONTDIR = os.path.join(CORE, "fonts")
STRINGS_H = os.path.join(CORE, "include", "ui_strings.h")

# Noto Sans KR — SIL Open Font License 1.1, so the generated bitmaps are
# redistributable with this repo. A sans face on purpose: this panel is a
# dashboard, and at 16 px after binarization a serif's thin strokes drop out.
FONT_URL_BASE = "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/KR/"
FONT_URLS = {
    "regular": FONT_URL_BASE + "NotoSansKR-Regular.otf",
    "medium":  FONT_URL_BASE + "NotoSansKR-Medium.otf",
}
LICENSE_URL = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE"


def wansung_syllables():
    """The 2350 KS X 1001 완성형 Hangul syllables.

    Derived from the EUC-KR codec rather than tabulated: the encoding's Hangul
    block is lead 0xB0..0xC8 x trail 0xA1..0xFE, and every one of those pairs
    decodes to exactly one syllable. If this ever returns something other than
    2350 the assumption has broken and the caller says so loudly.
    """
    out = []
    for lead in range(0xB0, 0xC9):
        for trail in range(0xA1, 0xFF):
            try:
                out.append(bytes([lead, trail]).decode("euc-kr"))
            except UnicodeDecodeError:
                pass
    return out


def ui_string_chars():
    """Every character in a #define'd string literal in ui_strings.h.

    The 완성형 set covers the Hangul, but not the typography the UI composes at
    runtime — the interpunct between footer hints, the ↔ after a link count, the
    percent sign. Those live in ui_strings.h (S_COMPOSED_CHARS exists precisely
    to hold the ones no other literal contains), so they are collected from
    there instead of being remembered here.
    """
    with open(STRINGS_H, encoding="utf-8") as f:
        src = f.read()
    # Comments first: the header explains itself in prose that contains Hangul
    # and typography which must NOT end up in the font.
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)

    chars = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
        lit = lit.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')
        chars |= {c for c in lit if c.isprintable()}
    return chars


def symbol_set():
    syll = wansung_syllables()
    if len(syll) != 2350:
        sys.exit(f"expected 2350 완성형 syllables, generated {len(syll)} — "
                 "Python's euc-kr codec is not what this script assumes")

    chars = set(syll)
    chars |= {chr(c) for c in range(0x20, 0x7F)}        # printable ASCII
    chars |= ui_string_chars()
    chars.discard("\n")
    chars.discard("\t")
    return chars


# name -> (size, weight). Both faces are full; see the module docstring.
FACES = {
    "ui_font_kr_16": (16, "regular"),
    "ui_font_kr_20": (20, "medium"),
}


def run_conv(font, name, size, chars):
    symbols = "".join(sorted(chars))
    out = os.path.join(FONTDIR, name + ".c")
    cmd = [
        "npx", "-y", "lv_font_conv@latest",
        "--font", font,
        "--size", str(size),
        "--bpp", "1",
        "--format", "lvgl",
        "--symbols", symbols,
        "--no-compress",
        "--lv-font-name", name,
        "-o", out,
    ]
    print(f"  {name}: {len(chars)} glyphs @ {size}px ...", flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="path to the Regular .otf/.ttf")
    ap.add_argument("--font-medium", help="path to the Medium .otf/.ttf")
    ap.add_argument("--download", action="store_true",
                    help="fetch the Noto Sans KR weights into a temp dir")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the symbol set and stop")
    args = ap.parse_args()

    chars = symbol_set()
    if args.dry_run:
        extra = sorted(c for c in chars if not ("가" <= c <= "힣") and ord(c) > 0x7E)
        print(f"{len(chars)} symbols "
              f"(2350 완성형 + {len(chars) - 2350} ASCII/punctuation)")
        print("non-ASCII, non-Hangul:", "".join(extra))
        return

    fonts = {"regular": args.font, "medium": args.font_medium}
    missing = [w for w, p in fonts.items() if not p]
    if missing:
        if not args.download:
            sys.exit("give --font and --font-medium, or --download")
        tmp = tempfile.mkdtemp()
        for w in missing:
            fonts[w] = os.path.join(tmp, os.path.basename(FONT_URLS[w]))
            print(f"downloading {FONT_URLS[w]}")
            urllib.request.urlretrieve(FONT_URLS[w], fonts[w])
        urllib.request.urlretrieve(LICENSE_URL, os.path.join(FONTDIR, "OFL.txt"))

    os.makedirs(FONTDIR, exist_ok=True)
    total = 0
    for name, (size, weight) in FACES.items():
        path = run_conv(fonts[weight], name, size, chars)
        total += os.path.getsize(path)
    print(f"generated {len(FACES)} faces, {total // 1024} KiB of C source")


if __name__ == "__main__":
    main()
