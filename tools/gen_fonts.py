#!/usr/bin/env python3
"""
Regenerate the subset CJK fonts in components/fortune_core/fonts/.

Why this exists
---------------
The panel is 1-bit and 122x250. A full Korean font is ~11k glyphs and megabytes;
we need about seventy. Subsetting is therefore mandatory — but a hand-maintained
`--symbols` list rots the moment someone edits a message, and the failure mode is
a tofu box (U+FFFD-ish blank) that only shows up once the firmware is on the
glass.

So the symbol list is not maintained. It is *derived*, here, from the same
sources the firmware compiles:

    components/fortune_core/include/omikuji_messages.h   fortune text + labels
    components/fortune_core/saju.c                       the 60갑자 name tables

Change a message, re-run this, and the fonts follow. Nothing else to remember.

Usage
-----
    python3 tools/gen_fonts.py --font /path/to/NotoSerifKR-Regular.otf

    # or let it fetch the OFL source itself into a temp dir:
    python3 tools/gen_fonts.py --download

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
CORE = os.path.join(ROOT, "components", "fortune_core")
FONTDIR = os.path.join(CORE, "fonts")

MESSAGES_H = os.path.join(CORE, "include", "omikuji_messages.h")
SAJU_C = os.path.join(CORE, "saju.c")

# Noto Serif KR — SIL Open Font License 1.1, so the generated bitmaps are
# redistributable with this repo. A serif/명조 face is also the right look: real
# omikuji slips are printed in a brush-derived serif, not a UI sans. The 만세력
# page additionally uses the Bold weight where the mockup asks for weight 700
# (the grade Hanja, the seal, the fortune-table headers).
FONT_URL_BASE = "https://github.com/notofonts/noto-cjk/raw/main/Serif/SubsetOTF/KR/"
FONT_URLS = {
    "regular": FONT_URL_BASE + "NotoSerifKR-Regular.otf",
    "bold":    FONT_URL_BASE + "NotoSerifKR-Bold.otf",
}
LICENSE_URL = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Serif/LICENSE"

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
# The rank table: X(id, hanja, hangul, message, haeseok, joeon, jae, sa, dae,
# geon). Rows are long enough to spill across '\'-continued lines now, so the
# gap between arguments has to admit the continuation backslash too — plain
# \s* stops dead at it.
GAP = r'[\s\\]*'
XROW_RE = re.compile(r'X\(' + GAP + r'(\w+)' + (GAP + ',' + GAP + r'"([^"]*)"') * 9 + GAP + r'\)')
# The five-element 흐름 table: X(id, text). One string, so it can never swallow a
# rank row — a rank row has a comma after its first string where this needs ')'.
FLOWROW_RE = re.compile(r'X\(' + GAP + r'(\w+)' + GAP + ',' + GAP + r'"([^"]*)"' + GAP + r'\)')

# One alternation pass over the source: string/char literals are matched (and
# kept) before the comment patterns get a chance, so a "//" inside a literal is
# safe. Comments must go — saju.c's header quotes its sources verbatim, and
# without this every accented letter and Hanja in a code comment would silently
# get a glyph baked into the firmware.
TOKEN_RE = re.compile(
    r'"(?:\\.|[^"\\])*"'      # string literal
    r"|'(?:\\.|[^'\\])*'"     # char literal
    r"|/\*.*?\*/"             # block comment
    r"|//[^\n]*",             # line comment
    re.S,
)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def unescape(raw):
    return raw.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")


def strip_noise(src):
    """Remove comments and #include lines.

    Includes matter because `#include "saju.h"` is a string literal as far as a
    regex is concerned, and it would otherwise put s/a/j/u/./h into a Korean
    font that never draws a Latin letter.
    """
    src = TOKEN_RE.sub(lambda m: "" if m.group(0)[0] == "/" else m.group(0), src)
    return re.sub(r'^\s*#\s*include\s+.*$', "", src, flags=re.M)


def strings_in(path):
    """Every string literal in a C source (comments/includes excluded)."""
    return [unescape(raw) for raw in STRING_RE.findall(strip_noise(read(path)))]


def renderable(s):
    """The characters of a literal that actually reach the glass.

    ASCII is included — the first version of this script kept only non-ASCII
    and every space in a Korean message came out as a tofu box, because the
    space is drawn from the *label's* font, not a fallback. Control characters
    are excluded ('\\n' is a layout instruction), and printf-style literals are
    dropped whole by the caller.
    """
    return {c for c in s if c.isprintable()}


# The fortune-table headers (財運/事業/對人/健康) and the masthead (今日運勢)
# get their own Bold faces, so they are matched by name rather than fished out
# of the general string soup — by Unicode range they would be
# indistinguishable from the 60갑자 Hanja.
CAT_RE = re.compile(r'#\s*define\s+MANSE_CAT_\w+\s+"([^"]*)"')
TITLE_RE = re.compile(r'#\s*define\s+MANSE_TITLE\s+"([^"]*)"')

HAN_LO, HAN_HI = 0x4E00, 0x9FFF   # CJK Unified Ideographs


def is_han(c):
    return HAN_LO <= ord(c) <= HAN_HI


def collect():
    """Return {font_key: (size, weight, set_of_chars)}."""
    msg_src = strip_noise(read(MESSAGES_H))
    rows = XROW_RE.findall(msg_src)
    if len(rows) != 7:
        sys.exit(f"expected 7 ranks in {MESSAGES_H}, found {len(rows)} — "
                 "the X-macro shape changed; fix this script too")
    if len(FLOWROW_RE.findall(msg_src)) != 5:
        sys.exit(f"expected 5 flow rows in {MESSAGES_H} — "
                 "the OMIKUJI_FLOW_TABLE shape changed; fix this script too")

    rank_hanja = set()
    rank_hangul = set()
    body = set()
    for row in rows:
        rank_hanja |= renderable(unescape(row[1]))
        rank_hangul |= renderable(unescape(row[2]))

    # Everything else — messages, verses, table values, fixed labels, the 흐름
    # table, and the 60갑자 name tables from saju.c. The X-macro columns land
    # here too via the generic literal scan, which is fine: `body` is the
    # superset the 16 px face carries.
    #
    # Format strings are skipped: "%s %s" would otherwise bake a '%' and an 's'
    # into the font, and neither is ever drawn.
    for path in (MESSAGES_H, SAJU_C):
        for s in strings_in(path):
            if "%" in s:
                continue
            body |= renderable(s)

    # Characters that exist only in runtime-composed strings, never in a source
    # literal (the class of bug that once turned every space into a tofu box):
    # the space in "<hanja> <hangul>", and the date line "2026. 8. 8 (토)",
    # whose digits and punctuation come from snprintf.
    body.add(" ")
    body |= set("0123456789().")

    cat_han = set()
    for s in CAT_RE.findall(msg_src):
        cat_han |= renderable(unescape(s))

    title = set()
    for s in TITLE_RE.findall(msg_src):
        title |= renderable(unescape(s))

    # The seal stamps 吉 on auspicious ranks and 凶 on the rest — always a
    # character the rank table already contains.
    seal = {c for c in rank_hanja if c in "吉凶"}

    # The 12 px workhorse draws Hangul and ASCII only (verse, meta line, side
    # pillars, table values, foot); its Han characters all render from the
    # dedicated Bold faces, so they are excluded here.
    kr12 = {c for c in body | rank_hangul if not is_han(c)}

    return {
        # The grade (大吉 … 大凶) — single size for every rank, like the mockup.
        "ui_font_kr_hanja_34": (34, "bold", rank_hanja),
        # The seal glyph and the masthead 今日運勢.
        "ui_font_kr_hanja_16": (16, "bold", seal | title),
        # Two-char fortune-table headers (財運 …) at table-column width.
        "ui_font_kr_hanja_12": (12, "bold", cat_han),
        # Body of the 만세력 page.
        "ui_font_kr_12": (12, "regular", kr12),
        # Head band, page-1 labels, the composed 일진 line, and the overlay.
        "ui_font_kr_16": (16, "regular", body | rank_hangul | rank_hanja),
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
    print(f"  {name}: {len(chars)} glyph(s) @ {size}px")
    subprocess.run(cmd, check=True, cwd=ROOT)
    return out, len(chars)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="path to the Regular .otf/.ttf")
    ap.add_argument("--font-bold", help="path to the Bold .otf/.ttf")
    ap.add_argument("--download", action="store_true",
                    help="fetch missing Noto Serif KR weights into a temp dir")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the derived symbol sets and stop")
    args = ap.parse_args()

    sets = collect()

    if args.dry_run:
        for name, (size, weight, chars) in sets.items():
            print(f"{name} ({size}px {weight}, {len(chars)}): {''.join(sorted(chars))}")
        return

    fonts = {"regular": args.font, "bold": args.font_bold}
    missing = [w for w, p in fonts.items() if not p]
    if missing:
        if not args.download:
            sys.exit("give --font and --font-bold, or --download")
        tmp = tempfile.mkdtemp()
        for w in missing:
            fonts[w] = os.path.join(tmp, os.path.basename(FONT_URLS[w]))
            print(f"downloading {FONT_URLS[w]}")
            urllib.request.urlretrieve(FONT_URLS[w], fonts[w])
        urllib.request.urlretrieve(LICENSE_URL, os.path.join(FONTDIR, "OFL.txt"))

    os.makedirs(FONTDIR, exist_ok=True)
    total = 0
    for name, (size, weight, chars) in sets.items():
        path, _ = run_conv(fonts[weight], name, size, chars)
        total += os.path.getsize(path)
    print(f"generated {len(sets)} fonts, {total // 1024} KiB of C source")


if __name__ == "__main__":
    main()
