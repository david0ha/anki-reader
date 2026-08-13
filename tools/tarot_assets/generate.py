#!/usr/bin/env python3
"""Generate checked-in LVGL 9 I1 tarot assets from yunruse/tarot.

This script is intentionally a maintainer tool, not part of the firmware build.
It requires the pinned yunruse/tarot checkout and the pinned ImageMagick release.
The generated C files need only LVGL and contain all 78 runtime images.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


SOURCE_REPOSITORY = "https://github.com/yunruse/tarot"
SOURCE_BRANCH = "develop"
SOURCE_COMMIT = "de7fac547e15f6b210f73f30e58df0d93c212727"
SOURCE_SUBDIR = Path("cards/bw")
SOURCE_LICENSE = "CC0-1.0"
EXPECTED_IMAGEMAGICK = "ImageMagick 7.1.2-3"

WIDTH = 272
HEIGHT = 464
STRIDE = WIDTH // 8
PALETTE = bytes((0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF))
DATA_SIZE = len(PALETTE) + STRIDE * HEIGHT

IMAGEMAGICK_ARGS = (
    "-auto-orient",
    "-colorspace", "Gray",
    "-filter", "Lanczos",
    "-resize", f"{WIDTH}x{HEIGHT}!",
    "-ordered-dither", "o4x4",
    "-depth", "8",
    "gray:-",
)
IMAGEMAGICK_COMMAND = (
    "magick {source} -auto-orient -colorspace Gray -filter Lanczos "
    f"-resize {WIDTH}x{HEIGHT}! -ordered-dither o4x4 -depth 8 gray:-"
)


@dataclass(frozen=True)
class Card:
    card_id: str
    source_file: str
    name_en: str
    name_ko: str
    arcana: str
    suit: str | None = None
    rank: str | None = None

    @property
    def group(self) -> str:
        return "major" if self.arcana == "major" else str(self.suit)

    @property
    def symbol(self) -> str:
        return "tarot_" + self.card_id.replace("-", "_")


MAJOR_NAMES = (
    ("The Fool", "바보"),
    ("The Magician", "마법사"),
    ("The High Priestess", "여사제"),
    ("The Empress", "여제"),
    ("The Emperor", "황제"),
    ("The Hierophant", "교황"),
    ("The Lovers", "연인"),
    ("The Chariot", "전차"),
    ("Strength", "힘"),
    ("The Hermit", "은둔자"),
    ("Wheel of Fortune", "운명의 수레바퀴"),
    ("Justice", "정의"),
    ("The Hanged Man", "매달린 사람"),
    ("Death", "죽음"),
    ("Temperance", "절제"),
    ("The Devil", "악마"),
    ("The Tower", "탑"),
    ("The Star", "별"),
    ("The Moon", "달"),
    ("The Sun", "태양"),
    ("Judgement", "심판"),
    ("The World", "세계"),
)

SUITS = (
    ("cups", "c", "Cups", "컵"),
    ("pentacles", "p", "Pentacles", "펜타클"),
    ("swords", "s", "Swords", "소드"),
    ("wands", "w", "Wands", "완드"),
)

RANKS = (
    ("Ace", "에이스"),
    ("Two", "2"),
    ("Three", "3"),
    ("Four", "4"),
    ("Five", "5"),
    ("Six", "6"),
    ("Seven", "7"),
    ("Eight", "8"),
    ("Nine", "9"),
    ("Ten", "10"),
    ("Page", "시종"),
    ("Knight", "기사"),
    ("Queen", "여왕"),
    ("King", "왕"),
)


def cards() -> tuple[Card, ...]:
    result = [
        Card(f"major-{number:02d}", f"{number}.jpg", name_en, name_ko, "major")
        for number, (name_en, name_ko) in enumerate(MAJOR_NAMES)
    ]
    for suit, prefix, suit_en, suit_ko in SUITS:
        for number, (rank_en, rank_ko) in enumerate(RANKS, start=1):
            result.append(Card(
                card_id=f"{suit}-{number:02d}",
                source_file=f"{prefix}{number}.jpg",
                name_en=f"{rank_en} of {suit_en}",
                name_ko=f"{suit_ko} {rank_ko}",
                arcana="minor",
                suit=suit,
                rank=rank_en.lower(),
            ))
    assert len(result) == 78
    return tuple(result)


def command_output(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}")
    return result.stdout.strip()


def validate_tools_and_checkout(source_root: Path) -> None:
    version_line = command_output(["magick", "-version"]).splitlines()[0]
    if not version_line.startswith("Version: " + EXPECTED_IMAGEMAGICK + " "):
        raise RuntimeError(
            f"expected {EXPECTED_IMAGEMAGICK}, got {version_line.removeprefix('Version: ')}"
        )

    commit = command_output(["git", "-C", str(source_root), "rev-parse", "HEAD"])
    if commit != SOURCE_COMMIT:
        raise RuntimeError(f"expected source commit {SOURCE_COMMIT}, got {commit}")

    status = command_output(["git", "-C", str(source_root), "status", "--short", "--", str(SOURCE_SUBDIR)])
    if status:
        raise RuntimeError("source cards/bw checkout has local changes")


def render_bilevel(source_path: Path) -> bytes:
    result = subprocess.run(
        ["magick", str(source_path), *IMAGEMAGICK_ARGS],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"ImageMagick failed for {source_path}: {result.stderr.decode(errors='replace')}"
        )
    raw = result.stdout
    if len(raw) != WIDTH * HEIGHT:
        raise RuntimeError(f"unexpected raw size for {source_path}: {len(raw)}")
    unexpected = set(raw) - {0x00, 0xFF}
    if unexpected:
        raise RuntimeError(f"non-bilevel pixels for {source_path}: {sorted(unexpected)}")
    return raw


def pack_i1(raw: bytes) -> bytes:
    """Pack pixels MSB-first; palette index 0 is white and index 1 is black."""
    packed = bytearray(STRIDE * HEIGHT)
    for y in range(HEIGHT):
        source_row = y * WIDTH
        packed_row = y * STRIDE
        for x in range(WIDTH):
            if raw[source_row + x] == 0x00:
                packed[packed_row + x // 8] |= 0x80 >> (x & 7)
    return PALETTE + bytes(packed)


def c_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16])
        rows.append(f"    {values},")
    return "\n".join(rows)


def render_group(group: str, group_cards: list[Card], payloads: dict[str, bytes]) -> str:
    sections = [
        "/* Generated by tools/tarot_assets/generate.py. Do not edit. */",
        '#include "tarot_cards.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "#ifndef LV_ATTRIBUTE_LARGE_CONST",
        "#define LV_ATTRIBUTE_LARGE_CONST",
        "#endif",
        "",
    ]
    for card in group_cards:
        payload = payloads[card.card_id]
        sections.extend((
            "LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST",
            f"static const uint8_t {card.symbol}_map[] = {{",
            c_bytes(payload),
            "};",
            "",
            f"const lv_image_dsc_t {card.symbol} = {{",
            "    .header = {",
            "        .magic = LV_IMAGE_HEADER_MAGIC,",
            "        .cf = LV_COLOR_FORMAT_I1,",
            "        .flags = 0,",
            f"        .w = {WIDTH},",
            f"        .h = {HEIGHT},",
            f"        .stride = {STRIDE},",
            "        .reserved_2 = 0,",
            "    },",
            f"    .data_size = sizeof({card.symbol}_map),",
            f"    .data = {card.symbol}_map,",
            "    .reserved = NULL,",
            "    .reserved_2 = NULL,",
            "};",
            "",
        ))
    return "\n".join(sections)


def render_header() -> str:
    return f"""/* Generated by tools/tarot_assets/generate.py. Public tarot asset API. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

#define TAROT_CARD_COUNT 78u
#define TAROT_CARD_WIDTH {WIDTH}u
#define TAROT_CARD_HEIGHT {HEIGHT}u
#define TAROT_CARD_STRIDE {STRIDE}u
#define TAROT_CARD_DATA_SIZE {DATA_SIZE}u

typedef struct {{
    const char *id;
    const char *name_ko;
    const char *name_en;
    const char *arcana;
    const char *suit;
    const char *rank;
}} tarot_card_metadata_t;

/* Returned descriptors are exact-size I1 assets and must not be scaled. */
const lv_image_dsc_t *tarot_card_image(const char *id);
const tarot_card_metadata_t *tarot_card_metadata(const char *id);

#ifdef __cplusplus
}}
#endif
"""


def c_string(value: str | None) -> str:
    if value is None:
        return "NULL"
    return json.dumps(value, ensure_ascii=False)


def render_catalog(all_cards: tuple[Card, ...]) -> str:
    declarations = "\n".join(f"extern const lv_image_dsc_t {card.symbol};" for card in all_cards)
    rows = []
    for card in all_cards:
        rows.append(
            "    { "
            f"{{ {c_string(card.card_id)}, {c_string(card.name_ko)}, {c_string(card.name_en)}, "
            f"{c_string(card.arcana)}, {c_string(card.suit)}, {c_string(card.rank)} }}, "
            f"&{card.symbol} }},"
        )
    table = "\n".join(rows)
    return f"""/* Generated by tools/tarot_assets/generate.py. Do not edit. */
#include "tarot_cards.h"

#include <stddef.h>
#include <string.h>

{declarations}

typedef struct {{
    tarot_card_metadata_t metadata;
    const lv_image_dsc_t *image;
}} tarot_card_entry_t;

static const tarot_card_entry_t catalog[TAROT_CARD_COUNT] = {{
{table}
}};

static const tarot_card_entry_t *find_card(const char *id)
{{
    if(id == NULL) return NULL;
    for(size_t i = 0; i < TAROT_CARD_COUNT; ++i) {{
        if(strcmp(catalog[i].metadata.id, id) == 0) return &catalog[i];
    }}
    return NULL;
}}

const lv_image_dsc_t *tarot_card_image(const char *id)
{{
    const tarot_card_entry_t *entry = find_card(id);
    return entry ? entry->image : NULL;
}}

const tarot_card_metadata_t *tarot_card_metadata(const char *id)
{{
    const tarot_card_entry_t *entry = find_card(id);
    return entry ? &entry->metadata : NULL;
}}
"""


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def generate(source_root: Path, output_root: Path) -> None:
    validate_tools_and_checkout(source_root)
    all_cards = cards()
    source_dir = source_root / SOURCE_SUBDIR
    payloads: dict[str, bytes] = {}
    manifest_cards = []

    for index, card in enumerate(all_cards, start=1):
        source_path = source_dir / card.source_file
        if not source_path.is_file():
            raise RuntimeError(f"missing source card: {source_path}")
        source_data = source_path.read_bytes()
        payload = pack_i1(render_bilevel(source_path))
        if len(payload) != DATA_SIZE:
            raise RuntimeError(f"unexpected packed size for {card.card_id}: {len(payload)}")
        payloads[card.card_id] = payload
        manifest_cards.append({
            "id": card.card_id,
            "name_en": card.name_en,
            "name_ko": card.name_ko,
            "arcana": card.arcana,
            "suit": card.suit,
            "rank": card.rank,
            "source_file": card.source_file,
            "source_url": (
                f"https://media.githubusercontent.com/media/yunruse/tarot/{SOURCE_COMMIT}/"
                f"cards/bw/{card.source_file}"
            ),
            "source_sha256": hashlib.sha256(source_data).hexdigest(),
            "generated_file": f"{card.group}.c",
            "symbol": card.symbol,
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
        })
        print(f"[{index:02d}/78] {card.card_id}", file=sys.stderr)

    asset_dir = output_root / "components" / "vault_core" / "assets" / "tarot"
    for group in ("major", "cups", "pentacles", "swords", "wands"):
        group_cards = [card for card in all_cards if card.group == group]
        write_text(asset_dir / f"{group}.c", render_group(group, group_cards, payloads))
    write_text(asset_dir / "catalog.c", render_catalog(all_cards))
    write_text(output_root / "components" / "vault_core" / "include" / "tarot_cards.h", render_header())

    manifest = {
        "schema_version": 1,
        "source": {
            "repository": SOURCE_REPOSITORY,
            "branch": SOURCE_BRANCH,
            "commit": SOURCE_COMMIT,
            "path": SOURCE_SUBDIR.as_posix(),
            "license": SOURCE_LICENSE,
            "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
        },
        "transform": {
            "width": WIDTH,
            "height": HEIGHT,
            "stride": STRIDE,
            "palette_bytes": 8,
            "data_size": DATA_SIZE,
            "color_format": "LV_COLOR_FORMAT_I1",
            "palette_format": "BGRA8888",
            "palette_index_0": "white",
            "palette_index_1": "black",
            "bit_order": "msb-first",
            "resize_filter": "Lanczos",
            "dither": "ordered",
            "dither_map": "o4x4",
            "imagemagick_version": EXPECTED_IMAGEMAGICK,
            "command": IMAGEMAGICK_COMMAND,
        },
        "cards": manifest_cards,
    }
    write_text(
        output_root / "tools" / "tarot_assets" / "manifest.json",
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-root",
        required=True,
        type=Path,
        help="pinned yunruse/tarot checkout (the directory containing cards/bw)",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository worktree to receive generated files",
    )
    args = parser.parse_args()
    try:
        generate(args.source_root.resolve(), args.output_root.resolve())
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
