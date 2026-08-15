#!/usr/bin/env python3
"""
Regenerate the faces in components/vault_core/fonts/.

Why this is different from a subset generator
---------------------------------------------
The board this project forked from could subset its fonts down to seventy
glyphs, because every string it drew was a literal in its own source. This board
draws a Japanese headword, its かな reading, example sentences, Korean glosses
and comment bodies that all arrive from the network at runtime. There is no
symbol list that can be derived ahead of time, and the failure mode of guessing
is a tofu box in the middle of somebody's card — on the glass, after a
two-second refresh, where nobody is watching.

So the three body faces carry the whole of both scripts: the 2350 완성형 Hangul
syllables of KS X 1001, every kana, all 6355 kanji of JIS X 0208 (level 1 and
level 2), the JIS punctuation row, plus ASCII and the typography the UI composes
at runtime.

None of those tables is hardcoded. The 2350 are exactly the syllables reachable
through the EUC-KR encoding's Hangul rows, and the kana and kanji are exactly
the characters reachable through EUC-JP's rows 4-5 and 16-84 — so Python's own
codecs generate both, and there is no data file to rot. The counts are asserted
on every run: a codec that stops agreeing with this script is a loud failure,
not a face that quietly ships 289 tofu boxes.

What no table covers is the component forms a shape explanation is written in.
別 = 另 + 刂 cites two characters that are not JIS X 0208 kanji, because a
radical's combining form has its own codepoint outside the level 1/level 2
blocks. Those are curated in ui_strings.h's S_DATA_RADICALS, which this reads
along with the rest of that file — see the comment there for what is in it, what
is deliberately not, and why.

Why the source fonts are split
------------------------------
Neither Sans body source can do this alone. Noto Sans KR is missing 289 of the
2965 JIS level 1 kanji and 631 of the 3390 level 2 kanji — the 新字体 forms with
no Korean-hanja counterpart (唖, 圧, 営, 鴎 ...). Noto Sans JP has every one of
them and not a single Hangul syllable. So each face is converted from both, with
the symbol set split into two disjoint groups: Hangul, ASCII and Latin
typography come from Noto Sans KR, and everything that is Japanese script —
kana, kanji and the JIS punctuation row — comes from Noto Sans JP.

The split is a partition on purpose, and this is the part worth knowing about
lv_font_conv: when two `--font` groups claim the same codepoint, **the last one
wins**, not the first (`lib/ranger.js` just overwrites the entry), and if the
winning font turns out not to have that glyph, the character is dropped from the
output silently, with exit code 0. Overlap is therefore a way to lose glyphs
without being told. Nothing here overlaps, and `verify_face()` re-reads every
generated .c and fails if a single requested character did not make it.

The 26 characters the split moves from the Korean source to the Japanese one
(「」『』、。・〜 and friends) render byte-for-byte identically from either
face — they are the same Source Han Sans drawings — so the partition costs
nothing on the glass and buys Japanese metrics inside Japanese text.

Where the split is drawn by script it is also checked against reality:
`face_groups()` reads each source font's cmap and moves anything the Korean face
does not actually have to the Japanese one. 42 of S_DATA_RADICALS' component
forms (牜 畐 亼 复 ...) go over that way — they are not Japanese script by the
JIS tables, so the script rule would have left them with a font that cannot draw
them, which is precisely the silent drop above.

Sizes
-----
1 bpp everywhere, not 4: the panel binarizes anyway, so anti-aliasing would cost
four times the flash to produce pixels that are then thresholded straight back to
black and white.

The hero is why the build needs LV_FONT_FMT_TXT_LARGE: LVGL otherwise packs a
glyph's bitmap offset into 20 bits, which caps one face at 1 MB of bitmap, and
the hero's bitmap is over 2 MiB. Without the option the compiler stops on an
#error lv_font_conv writes into the file, so this is a loud requirement, not a
trap.

`ui_font_jp_56` is Japanese script and ASCII from Noto Serif JP SemiBold, no
Hangul: 완성형 at 56 px would be another 790 KB of flash for glyphs a Japanese
headword cannot contain, where the 95 ASCII it does carry cost 16 KiB and 133
real cards. The serif is reserved for this display face; the 16 px body stays
Sans because thin serif strokes disappear when the panel binarizes them.

Usage
-----
    python3 tools/gen_fonts.py --download        # fetch all five Noto sources
    python3 tools/gen_fonts.py --kr-regular /path/NotoSansKR-Regular.otf \\
                               --kr-medium  /path/NotoSansKR-Medium.otf \\
                               --jp-regular /path/NotoSansJP-Regular.otf \\
                               --jp-medium  /path/NotoSansJP-Medium.otf \\
                               --jp-serif-semibold /path/NotoSerifJP-SemiBold.otf
    python3 tools/gen_fonts.py --dry-run         # report the symbol set only

Needs node/npx (it shells out to lv_font_conv). The generated .c files are
committed, so a normal build never runs this.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "components", "vault_core")
FONTDIR = os.path.join(CORE, "fonts")
STRINGS_H = os.path.join(CORE, "include", "ui_strings.h")

# Noto Sans KR / JP and Noto Serif JP — SIL Open Font License 1.1, so the
# generated bitmaps are redistributable with this repo. Body faces stay Sans:
# this panel is 1-bit, and at 16 px after binarization a serif's thin strokes
# drop out. The hero alone uses the Serif SemiBold display face at 56 px.
FONT_URLS = {
    "kr-regular": "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/KR/NotoSansKR-Regular.otf",
    "kr-medium":  "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/KR/NotoSansKR-Medium.otf",
    "jp-regular": "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf",
    "jp-medium":  "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Medium.otf",
    "jp-serif-semibold": "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Serif/SubsetOTF/JP/NotoSerifJP-SemiBold.otf",
}
LICENSE_URL = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE"
LV_FONT_CONV_VERSION = "1.5.3"


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


def jis_rows(first, last):
    """Every JIS X 0208 character in 区 (rows) `first`..`last`, via EUC-JP.

    The same trick as wansung_syllables() one encoding over: EUC-JP's code set 1
    puts 区 k at lead byte 0xA0+k and 点 n at trail 0xA0+n, so a row is one lead
    byte and Python's codec is the table. Unassigned cells raise and are skipped,
    which is what makes the row counts below worth asserting.
    """
    out = []
    for lead in range(0xA0 + first, 0xA0 + last + 1):
        for trail in range(0xA1, 0xFF):
            try:
                out.append(bytes([lead, trail]).decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    return out


def japanese_groups():
    """Everything the Japanese source font owns, kept in its four named pieces.

    Row 1 is the JIS punctuation row and is taken whole rather than curated,
    because it is 94 glyphs — under 2 KB at body size — and because leaving any
    of it out is a tofu box in a sentence the catalog really does write:
    ー ends half the katakana words, 々 is in 人々, and 「」 wrap every quotation.

    Rows 4 and 5 are the kana. Rows 16-47 and 48-84 are the two kanji levels;
    level 2 is included because a headword is whatever the deck says it is, and
    the difference between the levels is 3390 glyphs the board would otherwise
    draw as boxes rather than a category a learner ever sees.

    The four counts are the point of doing it this way. They are exactly what
    JIS X 0208 defines, so a codec that stops agreeing with them has changed the
    face's contents, and this says so instead of shipping the difference.
    """
    groups = {
        "JIS X 0208 row 1 (punctuation)": (jis_rows(1, 1), 94),
        "kana (rows 4-5)":                (jis_rows(4, 5), 169),
        "JIS X 0208 level 1 kanji":       (jis_rows(16, 47), 2965),
        "JIS X 0208 level 2 kanji":       (jis_rows(48, 84), 3390),
    }
    for what, (chars, want) in groups.items():
        if len(chars) != want:
            sys.exit(f"expected {want} {what}, generated {len(chars)} — "
                     "Python's euc_jp codec is not what this script assumes")
    return {what: chars for what, (chars, _) in groups.items()}


def japanese_set():
    """The union of japanese_groups() — the Japanese source font's whole share."""
    return set().union(*(set(c) for c in japanese_groups().values()))


def hero_set():
    """What ui_font_jp_56 carries: Japanese script and printable ASCII.

    The hero draws exactly one string, the headword, and a headword is mostly
    kana and kanji — but not only. The catalog writes its bound forms with an
    ASCII tilde (~がる, ~ちゃん, ~ごと) and its optional okurigana with ASCII
    parentheses (表(わ)す), and kanji_hero_is_large() routes 9,799 of the 9,956
    cards here on character count alone, with no coverage test. That put a tofu
    box dead centre at 56 px on 133 cards: ~ in 106 of them, ( and ) in 25 each.

    So the whole printable ASCII range, not a curated handful — the next bound
    form the deck adds will use whatever punctuation its author typed, and the
    range costs 95 glyphs where the face already holds 6,618.

    Still nothing Korean: at 56 px the 완성형 set alone would be most of a
    megabyte of flash for glyphs a Japanese headword cannot contain.

    Exposed rather than inlined so the simulator and tools/kanji_server.py can
    check a `card.front` against the face that will actually render it — a
    character in symbol_set() is not necessarily a character the hero has.
    """
    return japanese_set() | {chr(c) for c in range(0x20, 0x7F)}


def ui_string_chars():
    """Every character in a #define'd string literal in ui_strings.h.

    The 완성형 set covers the Hangul, but not the typography the UI composes at
    runtime — the interpunct between footer hints, the % in the FSRS copy, the
    em dash that stands in for a number the scheduler does not have yet. Those
    live in ui_strings.h (S_COMPOSED_CHARS and S_DATA_PUNCT exist precisely to
    hold the ones no other literal contains), so they are collected from there
    instead of being remembered here.
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


def korean_set():
    """Everything the Korean source font owns: the complement of japanese_set().

    Subtracting rather than listing is what keeps the two groups a partition: a
    Japanese character that appears in ui_strings.h — a 「」 in a fixed label,
    say — belongs to the face that is certain to have it, and belongs to exactly
    one of the two groups no matter which file it was named in.
    """
    syll = wansung_syllables()
    if len(syll) != 2350:
        sys.exit(f"expected 2350 완성형 syllables, generated {len(syll)} — "
                 "Python's euc-kr codec is not what this script assumes")

    chars = set(syll)
    chars |= {chr(c) for c in range(0x20, 0x7F)}        # printable ASCII
    chars |= ui_string_chars()
    chars.discard("\n")
    chars.discard("\t")
    return chars - japanese_set()


def symbol_set():
    """Every character the three body faces can draw.

    tools/kanji_server.py imports this and checks each string it is about to
    send, so a catalog entry containing a simplified-Chinese component form is
    caught on a laptop with the codepoint printed rather than silently on the
    glass. It is the union of the two source groups, so it stays true by
    construction when either half moves.
    """
    return korean_set() | japanese_set()


# name -> (size, sources, symbols). The three body faces are merged from both
# Sans families; the hero is one Serif family, because everything it draws is
# Japanese.
#
# Weights follow the existing hierarchy — Regular for reading text, Medium for
# headings, and Serif SemiBold for the display-sized hero.
FACES = {
    "ui_font_kr_16": (16, ("kr-regular", "jp-regular"), symbol_set),
    "ui_font_kr_20": (20, ("kr-medium",  "jp-medium"),  symbol_set),
    "ui_font_kr_28": (28, ("kr-medium",  "jp-medium"),  symbol_set),
    "ui_font_jp_56": (56, ("jp-serif-semibold",),      hero_set),
}


_COVERAGE = {}


def otf_coverage(path):
    """The codepoints a source font actually has a glyph for, from its cmap.

    fontTools is not a dependency of this repo and this needs one question
    answered, so the sfnt tables are read directly. Formats 4 and 12 are what
    Noto CJK ships; the ttcf header is handled because the non-subset family is
    distributed as a collection and somebody will eventually point --kr-regular
    at one.

    This exists because "the Korean source owns everything that is not Japanese
    script" is an assumption, not a fact. It held while that remainder was
    Hangul and ASCII. It stops holding the moment S_DATA_RADICALS names 亻 or
    扌, which are neither Japanese script by the JIS tables nor present in Noto
    Sans KR — and lv_font_conv's answer to a group whose font lacks the glyph is
    to drop the character and exit 0.
    """
    if path in _COVERAGE:
        return _COVERAGE[path]

    with open(path, "rb") as f:
        buf = f.read()

    base = 0
    if buf[:4] == b"ttcf":
        base = struct.unpack_from(">I", buf, 12)[0]
    num = struct.unpack_from(">H", buf, base + 4)[0]
    cmap = None
    for i in range(num):
        tag, _sum, off, _len = struct.unpack_from(">4sIII", buf, base + 12 + i * 16)
        if tag == b"cmap":
            cmap = off
    if cmap is None:
        sys.exit(f"{path}: no cmap table")

    # Prefer a format 12 subtable: format 4 stops at the BMP, and the astral
    # planes have to be visible for a "this font does not have it either"
    # verdict to mean anything.
    best = None
    for i in range(struct.unpack_from(">H", buf, cmap + 2)[0]):
        pid, eid, sub = struct.unpack_from(">HHI", buf, cmap + 4 + i * 8)
        fmt = struct.unpack_from(">H", buf, cmap + sub)[0]
        if fmt == 12 and (pid, eid) in ((3, 10), (0, 4), (0, 6)):
            best = (cmap + sub, 12)
            break
        if fmt == 4 and (pid, eid) in ((3, 1), (0, 3)) and best is None:
            best = (cmap + sub, 4)
    if best is None:
        sys.exit(f"{path}: no format 4 or 12 unicode cmap subtable")

    at, fmt = best
    cps = set()
    if fmt == 12:
        for g in range(struct.unpack_from(">I", buf, at + 12)[0]):
            start, end, _gid = struct.unpack_from(">III", buf, at + 16 + g * 12)
            cps.update(range(start, end + 1))
    else:
        seg2 = struct.unpack_from(">H", buf, at + 6)[0]
        seg = seg2 // 2
        ends = struct.unpack_from(f">{seg}H", buf, at + 14)
        starts = struct.unpack_from(f">{seg}H", buf, at + 16 + seg2)
        deltas = struct.unpack_from(f">{seg}h", buf, at + 16 + 2 * seg2)
        ro_at = at + 16 + 3 * seg2
        ros = struct.unpack_from(f">{seg}H", buf, ro_at)
        for i in range(seg):
            for c in range(starts[i], min(ends[i], 0xFFFE) + 1):
                if ros[i]:
                    gid = struct.unpack_from(
                        ">H", buf, ro_at + i * 2 + ros[i] + (c - starts[i]) * 2)[0]
                    gid = (gid + deltas[i]) & 0xFFFF if gid else 0
                else:
                    gid = (c + deltas[i]) & 0xFFFF
                if gid:
                    cps.add(c)

    _COVERAGE[path] = cps
    return cps


def face_groups(chars, keys, paths):
    """Split `chars` into the (font path, symbols) groups lv_font_conv wants.

    A single-source face takes everything from that one font. A merged face
    gives the second source everything Japanese and the first source the rest,
    which is a partition and so says nothing about precedence — deliberately,
    because lv_font_conv's precedence is the opposite of what one would guess.

    "Everything Japanese" is the JIS tables plus one correction the JIS tables
    cannot express: the radical component forms in S_DATA_RADICALS. 亻 扌 氵 are
    not JIS X 0208 kanji, so the script partition hands them to the Korean
    source, and Noto Sans KR does not have them — 42 of them exist only in Noto
    Sans JP. So the group boundary is drawn against what each font can actually
    draw, read from its cmap, rather than against what its language name
    implies. Anything neither font has still lands here and is caught by
    verify_face(), which is the point of asking the question this way round.

    Order still matters for one thing that is not glyph selection: the merged
    face takes its OS/2 typo metrics and its underline position from the FIRST
    font on the command line, so the Korean source leads.

    What that does NOT pin is line_height, which lv_font_conv recomputes as the
    extremes of the glyphs the face actually ended up with. Adding kanji to a
    Hangul face raises it — 19->20 at 16 px, 22->24 at 20 px, 33->35 at 28 px —
    because a kanji's box is taller than a syllable's. Anything that stacks rows
    at a fixed pitch has to be re-checked against the rendered panel, not
    against the old numbers.
    """
    if len(keys) == 1:
        return [(paths[keys[0]], chars)]
    first, second = paths[keys[0]], paths[keys[1]]
    has_first = otf_coverage(first)
    jp = (chars & japanese_set()) | {c for c in chars if ord(c) not in has_first}
    return [(first, chars - jp), (second, jp)]


def run_conv(name, size, groups):
    out = os.path.join(FONTDIR, name + ".c")
    cmd = ["npx", "-y", f"lv_font_conv@{LV_FONT_CONV_VERSION}"]
    for font, chars in groups:
        cmd += ["--font", font, "--symbols", "".join(sorted(chars))]
    cmd += [
        "--size", str(size),
        "--bpp", "1",
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", name,
        "-o", out,
    ]
    sources = " + ".join(f"{len(c)} from {os.path.basename(f)}" for f, c in groups)
    print(f"  {name}: {size}px, {sources} ...", flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT)
    return out


def verify_face(path, chars):
    """Assert the generated face really carries every character asked for.

    This is not belt and braces. lv_font_conv drops a character whose assigned
    font has no such glyph without a word and without a non-zero exit, so the
    only evidence that a face is complete is the cmap in the file it wrote. The
    LVGL cmap is either a contiguous FORMAT0 block or a SPARSE unicode_list of
    offsets from range_start; both are read back here.
    """
    with open(path, encoding="utf-8") as f:
        src = f.read()

    table = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not table:
        sys.exit(f"{path}: no cmaps[] table — lv_font_conv output changed shape")

    got = set()
    for entry in re.findall(r"\{(.*?)\}", table.group(1), re.S):
        def field(n):
            m = re.search(r"\." + n + r"\s*=\s*([^,\n]+)", entry)
            return m.group(1).strip() if m else None

        start = int(field("range_start"), 0)
        if "SPARSE" in field("type"):
            listlen = int(field("list_length"), 0)
            body = re.search(r"\b" + re.escape(field("unicode_list")) +
                             r"\[\]\s*=\s*\{(.*?)\};", src, re.S)
            offsets = [int(t, 0) for t in
                       re.findall(r"0x[0-9a-fA-F]+|\d+", body.group(1))]
            if len(offsets) != listlen:
                sys.exit(f"{path}: unicode list is {len(offsets)} entries, "
                         f"cmap says {listlen}")
            got |= {start + o for o in offsets}
        else:
            got |= set(range(start, start + int(field("range_length"), 0)))

    lost = sorted(c for c in chars if ord(c) not in got)
    if lost:
        sys.exit(f"{os.path.basename(path)}: {len(lost)} requested characters "
                 f"are not in the generated face: {''.join(lost[:40])}")
    return len(got)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kr-regular", help="path to NotoSansKR-Regular.otf")
    ap.add_argument("--kr-medium", help="path to NotoSansKR-Medium.otf")
    ap.add_argument("--jp-regular", help="path to NotoSansJP-Regular.otf")
    ap.add_argument("--jp-medium", help="path to NotoSansJP-Medium.otf")
    ap.add_argument("--jp-serif-semibold",
                    help="path to NotoSerifJP-SemiBold.otf")
    ap.add_argument("--download", action="store_true",
                    help="fetch the five Noto Sans/Serif sources into a temp dir")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the symbol set and stop")
    args = ap.parse_args()

    chars = symbol_set()
    if args.dry_run:
        kr = korean_set()
        hangul = sum(1 for c in kr if "가" <= c <= "힣")
        print(f"{len(chars)} symbols in the body faces")
        print(f"  Noto Sans KR: {len(kr)} — {hangul} 완성형 + "
              f"{len(kr) - hangul} ASCII/typography")
        for what, group in japanese_groups().items():
            print(f"  Noto Sans JP: {len(group):5d} — {what}")
        print(f"{len(hero_set())} symbols in ui_font_jp_56 (the headword hero)")
        # Which of these the Korean font is actually asked for is not decided
        # here — face_groups() moves anything it does not have to the Japanese
        # source — so this is the script partition, not the final grouping.
        extra = sorted(c for c in kr if not ("가" <= c <= "힣") and ord(c) > 0x7E)
        print(f"{len(extra)} non-ASCII, non-Hangul by the script partition:",
              "".join(extra))
        return

    sources = {
        "kr-regular": args.kr_regular,
        "kr-medium": args.kr_medium,
        "jp-regular": args.jp_regular,
        "jp-medium": args.jp_medium,
        "jp-serif-semibold": args.jp_serif_semibold,
    }
    missing = [k for k, p in sources.items() if not p]
    if missing:
        if not args.download:
            sys.exit("give every --kr-*/--jp-* path, or --download")
        tmp = tempfile.mkdtemp()
        for k in missing:
            sources[k] = os.path.join(tmp, os.path.basename(FONT_URLS[k]))
            print(f"downloading {FONT_URLS[k]}")
            urllib.request.urlretrieve(FONT_URLS[k], sources[k])
        urllib.request.urlretrieve(LICENSE_URL, os.path.join(FONTDIR, "OFL.txt"))

    os.makedirs(FONTDIR, exist_ok=True)
    total = 0
    for name, (size, keys, symbols) in FACES.items():
        want = symbols()
        path = run_conv(name, size, face_groups(want, keys, sources))
        glyphs = verify_face(path, want)
        size_kib = os.path.getsize(path) // 1024
        print(f"    {glyphs} glyphs, {size_kib} KiB of C source", flush=True)
        total += os.path.getsize(path)
    print(f"generated {len(FACES)} faces, {total // 1024} KiB of C source")


if __name__ == "__main__":
    main()
