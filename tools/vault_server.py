#!/usr/bin/env python3
"""
Serve a REAL Obsidian vault to the board.

`mock_vault_server.py` is the contract as a fixed payload — it pins the wire
format and produces the test fixture. This is the other half: it walks an actual
vault on disk, works out the same numbers from the notes that are really there,
and serves them at the same URL shape. Point the board at this and the panel
stops showing a demo and starts showing your vault.

    python3 tools/vault_server.py ~/Documents/MyVault
    curl -X POST http://obsidianboard.local/api/vault \\
         -d '{"url":"http://mymac.local:8123/vault.json"}'

Read-only by default: it opens `.md` files and nothing else. `--allow-capture`
adds one write path, `POST /capture`, which appends a memo to the vault's inbox
so the board's queue is somewhere you can actually put things — see "Capture"
below. It stays off unless you ask for it.

What the numbers mean
---------------------
The definitions are Obsidian's where Obsidian has one, and stated here where it
does not — the board has no room to explain itself, so this is where the meaning
lives.

- **notes**     every `.md` file under the vault, excluding dotted directories
                (`.obsidian`, `.trash`, `.git`) and anything `--exclude`d.
- **links**     distinct **directed** pairs (A links to B) where both ends are
                notes that exist. Two `[[B]]`s in A count once; a link to a note
                that has not been created yet counts zero, because the graph on
                the panel has nowhere to draw it. `![[embeds]]` count.
- **orphans**   notes with no links in *or* out — Obsidian's own definition.
- **tags**      distinct tags, from frontmatter `tags:` and from inline `#tag`.
                Code fences and inline code are stripped first, so a `#include`
                in a C snippet is not a tag.
- **added**     by creation date: a `created:` frontmatter field if there is one
                and it parses, else the filesystem's birth time (macOS), else
                mtime. `daily` is the last seven days, oldest first.
- **recent**    the eight most recently *modified* notes.
- **inbox**     notes under an inbox folder (`--inbox`, auto-detected by default)
                or carrying `--inbox-tag`. Oldest first: the point of the list is
                what has been sitting there longest.
- **agents**    read from a JSON file that something else writes; see --agents.
                No file, no agents — this server does not invent them.

Rescanning is incremental: a file whose mtime and size are unchanged is not
reopened. A 3,000-note vault costs a second the first time and milliseconds
after that, which matters because the board polls forever.

Capture
-------
A wall display showing an inbox you cannot add to is half a loop. With
`--allow-capture`, one endpoint closes it:

    curl -X POST http://localhost:8123/capture -d 'ring the dentist'

That writes `Inbox/ring the dentist.md` into the vault, and the board shows it
on its next poll. Bind it to a keyboard shortcut and the board becomes somewhere
to put things, not just somewhere to look.

It is off by default and has to be asked for, because it writes into somebody's
notes and this is an unauthenticated service on a LAN. When it is on, it can do
exactly one thing: create a new `.md` file inside the capture folder. The
filename is built here from a sanitised slug, so a request cannot name a path;
an existing file is never overwritten; and the body is capped.

Usage
-----
    python3 tools/vault_server.py VAULT                  # serve on :8123
    python3 tools/vault_server.py VAULT --port 9000
    python3 tools/vault_server.py VAULT --dump           # print the payload
    python3 tools/vault_server.py VAULT --inbox Inbox --inbox-tag '#todo'
    python3 tools/vault_server.py VAULT --agents ~/agents.json
    python3 tools/vault_server.py VAULT --allow-capture  # enable POST /capture
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import unicodedata
from http.server import BaseHTTPRequestHandler, HTTPServer
from zoneinfo import ZoneInfo

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Device-side caps (components/vault_core/include/vault_model.h). The parser
# clamps to these anyway; matching them here means what is served is what is
# shown, so a title that vanished off the panel is not a mystery.
TAGS_MAX = 6
AGENTS_MAX = 6
NODES_MAX = 14
EDGES_MAX = 32
RECENT_MAX = 8
INBOX_MAX = 8
DAILY_DAYS = 7
TITLE_MAX = 64

ARTWORK_HEADLINE_MAX = 2
ARTWORK_DEFINITION_MAX = 2
ARTWORK_BACKLINKS_MAX = 3
ARTWORK_NODES_MAX = 6
ARTWORK_EDGES_MAX = 8

ARTWORK_HEADLINE_CELLS = 24
ARTWORK_HEADWORD_CELLS = 20
ARTWORK_DEFINITION_CELLS = 36
ARTWORK_NOTE_TITLE_CELLS = 24
ARTWORK_NOTE_PATH_CELLS = 30
ARTWORK_BACKLINK_CELLS = 28
ARTWORK_GRAPH_TITLE_CELLS = 14

TAROT_TIMEZONE = "Asia/Seoul"
TAROT_HEADLINE_CELLS = 22
TAROT_BODY_CELLS = 32
TAROT_LINE_UTF8_BYTES = 127
DEFAULT_TAROT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "tarot_readings_ko.json")

# Directory names that are never vault content. Anything else dotted is skipped
# too — `.obsidian` is only the most obvious one.
SKIP_DIRS = {".obsidian", ".trash", ".git", ".github", "node_modules"}

DEFAULT_INBOX_NAMES = ["inbox", "0-inbox", "00-inbox", "_inbox", "인박스", "받은편지함"]


def visual_cells(text):
    """Deterministic display width for producer-side fixed-row fitting."""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def tarot_display_cells(text):
    """Conservative cells for the real tarot fonts: space=1, every inked glyph=2.

    Noto Sans KR's Latin W is almost as wide as a Hangul syllable. Treating
    ASCII as one East-Asian-width cell would accept rows that LVGL ellipsizes.
    """
    return sum(1 if ch.isspace() else 2 for ch in text)


def fit_visual(text, max_cells):
    """Clip at a display-cell boundary, reserving one cell for an ellipsis."""
    if visual_cells(text) <= max_cells:
        return text
    out = []
    used = 0
    limit = max(0, max_cells - 1)
    for char in text:
        width = 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
        if used + width > limit:
            break
        out.append(char)
        used += width
    return "".join(out).rstrip() + "…"


def fit_wire(text, max_cells, max_utf8_bytes):
    """Fit one fixed LVGL row by both approximate width and C buffer bytes.

    The wire carries UTF-8 while the ESP32 stores fixed byte arrays. Characters
    such as the Euro sign are one display cell but three bytes, so either limit
    alone is insufficient. Reserve the ellipsis in both budgets when clipping.
    """
    text = str(text or "")
    if visual_cells(text) <= max_cells and len(text.encode("utf-8")) <= max_utf8_bytes:
        return text
    ellipsis = "…"
    cell_limit = max(0, max_cells - visual_cells(ellipsis))
    byte_limit = max(0, max_utf8_bytes - len(ellipsis.encode("utf-8")))
    out = []
    cells = 0
    byte_count = 0
    for char in text:
        width = 2 if unicodedata.east_asian_width(char) in ("W", "F") else 1
        encoded = len(char.encode("utf-8"))
        if cells + width > cell_limit or byte_count + encoded > byte_limit:
            break
        out.append(char)
        cells += width
        byte_count += encoded
    return "".join(out).rstrip() + ellipsis


def fit_focus_label(text):
    """Break the selected graph title into at most two rows inside its disc."""
    compact = " ".join(str(text or "").split())
    if visual_cells(compact) <= 8:
        return fit_wire(compact, 8, 63)
    words = compact.split(" ")
    if len(words) > 1:
        best = None
        for split in range(1, len(words)):
            left = " ".join(words[:split])
            right = " ".join(words[split:])
            if visual_cells(left) <= 8 and visual_cells(right) <= 8:
                score = abs(visual_cells(left) - visual_cells(right))
                if best is None or score < best[0]:
                    best = (score, left, right)
        if best:
            return best[1] + "\n" + best[2]

    first = fit_wire(compact, 8, 31)
    consumed = first[:-1] if first.endswith("…") else first
    rest = compact[len(consumed):].lstrip()
    if not rest:
        return first
    return consumed + "\n" + fit_wire(rest, 8, 31)


def expected_tarot_card_ids():
    """The stable 78-card ids shared by assets, copy and the wire contract."""
    majors = [f"major-{number:02d}" for number in range(22)]
    minors = [
        f"{suit}-{number:02d}"
        for suit in ("cups", "pentacles", "swords", "wands")
        for number in range(1, 15)
    ]
    return tuple(majors + minors)


def _fallback_tarot_readings():
    """Complete deterministic copy for source checkouts missing the data file."""
    return {
        card_id: {
            "copy_version": 1,
            "headline": ["오늘의 카드를", "천천히 살피다"],
            "flow": ["작은 신호에 주의를 기울인다"],
            "caution": ["확신을 서둘러 결론내지 않는다"],
            "action": ["한 번 더 확인하고 움직인다"],
        }
        for card_id in expected_tarot_card_ids()
    }


def load_tarot_readings(path=DEFAULT_TAROT_PATH):
    """Load and strictly validate the authored 78-card Korean copy mapping."""
    if not path or not os.path.exists(path):
        if path and os.path.abspath(path) != os.path.abspath(DEFAULT_TAROT_PATH):
            raise ValueError(f"tarot readings not found: {path}")
        print("warning: packaged tarot readings missing; using generic fallback", file=sys.stderr)
        return _fallback_tarot_readings()
    try:
        with open(path, "r", encoding="utf-8") as source:
            data = json.load(source)
    except (OSError, ValueError) as exc:
        raise ValueError(f"tarot readings unreadable: {exc}") from exc
    if not isinstance(data, dict) or set(data) != set(expected_tarot_card_ids()):
        raise ValueError("tarot readings must contain exactly the 78 stable card ids")

    readings = {}
    for card_id in expected_tarot_card_ids():
        entry = data[card_id]
        if not isinstance(entry, dict):
            raise ValueError(f"{card_id}: reading must be an object")
        copy_version = entry.get("copy_version", 1)
        if isinstance(copy_version, bool) or not isinstance(copy_version, int) or copy_version <= 0:
            raise ValueError(f"{card_id}: copy_version must be a positive integer")
        normalized = {"copy_version": copy_version}
        for key in ("headline", "flow", "caution", "action"):
            lines = entry.get(key)
            if not isinstance(lines, list) or not 1 <= len(lines) <= 2:
                raise ValueError(f"{card_id}.{key}: expected one or two lines")
            max_cells = TAROT_HEADLINE_CELLS if key == "headline" else TAROT_BODY_CELLS
            for line in lines:
                if (not isinstance(line, str) or not line or
                        any(ord(char) < 0x20 for char in line) or
                        tarot_display_cells(line) > max_cells or
                        len(line.encode("utf-8")) > TAROT_LINE_UTF8_BYTES):
                    raise ValueError(f"{card_id}.{key}: line exceeds its display budget")
            normalized[key] = list(lines)
        readings[card_id] = normalized
    return readings

# ---------------------------------------------------------------------------
# Parsing one note
# ---------------------------------------------------------------------------

FENCE_RE = re.compile(r"^(?:```|~~~).*?^(?:```|~~~)", re.S | re.M)
INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
FRONTMATTER_RE = re.compile(r"\A---\r?\n(.*?)\r?\n---\r?\n", re.S)

# [[Target]], [[Target|alias]], [[Target#heading]], [[Target^block]], ![[embed]]
WIKILINK_RE = re.compile(r"!?\[\[([^\]\|#\^\r\n]+)(?:[#\^][^\]\|\r\n]*)?(?:\|[^\]\r\n]*)?\]\]")
# [text](Some/Note.md) — a relative markdown link to another note.
MDLINK_RE = re.compile(r"\[[^\]\r\n]*\]\(([^)\s]+\.md)\)")
# The destination half of ANY markdown link, and bare/auto-linked URLs. Blanked
# before tags are extracted: `[TOC](#-features)` and `https://x/y#frag` are not
# tags, and left in they inflate the tag count and take over the panel's
# top-tags list with URL-encoded rubbish. Found by pointing this at a directory
# of real documentation, which is exactly the shape that has TOC links in it.
MDLINK_DEST_RE = re.compile(r"\]\([^)\r\n]*\)")
URL_RE = re.compile(r"<?\bhttps?://[^\s>)\]]+>?")
# Raw HTML, which markdown allows and people paste into notes. `<a href="#x">`
# is an anchor, not a tag — and an HTML table of contents produces a dozen of
# them, which is how this was found.
HTML_TAG_RE = re.compile(r"<[^>\r\n]{1,300}>")
# #tag — not preceded by a word character (so `foo#bar` and URL fragments are
# out) and not a heading (`# ` has a space). Obsidian allows nesting with '/'.
TAG_RE = re.compile(r"(?<![\w/&#])#([^\s#,.;:!?'\"()\[\]{}<>/\\]{1,60}(?:/[^\s#,.;:!?'\"()\[\]{}<>/\\]{1,60})*)")
TASK_RE = re.compile(r"^\s*[-*+]\s+\[ \]\s+", re.M)

DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})")


def strip_code(text):
    """Remove fenced blocks and inline code before looking for links or tags.

    Obsidian does not index a `[[link]]` inside a code fence, and neither does
    this. Without it, any note documenting this project's own syntax would
    invent links and tags that do not exist.
    """
    return INLINE_CODE_RE.sub(" ", FENCE_RE.sub("\n", text))


def parse_frontmatter(text):
    """A deliberately small YAML subset: `key: value` and `- item` lists.

    Importing PyYAML would mean this script cannot be run with a bare Python,
    and the two fields that matter here (`tags`, `created`) are exactly the ones
    people write in the simple form.
    """
    m = FRONTMATTER_RE.match(text)
    if not m:
        return {}, text
    out, key = {}, None
    for raw in m.group(1).splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        if line.lstrip().startswith("- ") and key:
            out.setdefault(key, [])
            if isinstance(out[key], list):
                out[key].append(line.lstrip()[2:].strip().strip("\"'"))
            continue
        if ":" in line and not line.startswith(" "):
            key, _, value = line.partition(":")
            key = key.strip().lower()
            value = value.strip()
            out[key] = value.strip("\"'") if value else []
    return out, text[m.end():]


def frontmatter_tags(fm):
    raw = fm.get("tags", fm.get("tag", []))
    if isinstance(raw, str):
        raw = [t for t in re.split(r"[,\s]+", raw.strip("[]")) if t]
    return {t.lstrip("#").strip().strip("\"'") for t in raw if str(t).strip()}


def parse_note(path, text):
    """Everything one note contributes, independent of the rest of the vault."""
    fm, body = parse_frontmatter(text)
    clean = strip_code(body)

    targets = set()
    for m in WIKILINK_RE.finditer(clean):
        target = m.group(1).strip()
        if target:
            targets.add(target)
    for m in MDLINK_RE.finditer(clean):
        target = m.group(1).strip()
        if target and "://" not in target:
            targets.add(target[:-3] if target.endswith(".md") else target)

    # Tags come from prose only — never from a link destination, a URL, or an
    # HTML attribute.
    prose = HTML_TAG_RE.sub(" ", URL_RE.sub(" ", MDLINK_DEST_RE.sub("]( )", clean)))
    tags = frontmatter_tags(fm)
    tags |= {m.group(1) for m in TAG_RE.finditer(prose)}
    # A purely numeric "#2026" is a heading anchor or an issue reference, not a
    # tag — Obsidian rejects those too.
    tags = {t for t in tags if not t.replace("/", "").isdigit()}

    return {
        "targets": targets,
        "tags": tags,
        "open_tasks": len(TASK_RE.findall(clean)),
        "created_fm": parse_fm_date(fm),
    }


def parse_fm_date(fm):
    for key in ("created", "created_at", "date", "생성일", "날짜"):
        value = fm.get(key)
        if isinstance(value, str):
            m = DATE_RE.search(value)
            if m:
                try:
                    return datetime.date(*(int(g) for g in m.groups()))
                except ValueError:
                    pass
    return None


# ---------------------------------------------------------------------------
# Walking the vault
# ---------------------------------------------------------------------------

def iter_notes(root, excludes):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [
            d for d in dirnames
            if not d.startswith(".") and d not in SKIP_DIRS and d not in excludes
        ]
        for name in filenames:
            if name.endswith(".md"):
                yield os.path.join(dirpath, name)


def created_date(path, fm_date, stat):
    if fm_date:
        return fm_date
    # st_birthtime is real on macOS; on Linux it is usually absent and mtime is
    # the only honest answer available.
    ts = getattr(stat, "st_birthtime", None) or stat.st_mtime
    return datetime.date.fromtimestamp(ts)


class Vault:
    """A scanned vault, with a per-file cache so repeat polls are cheap."""

    def __init__(self, root, excludes=(), inbox_names=None, inbox_tag=None,
                 tarot_path=DEFAULT_TAROT_PATH, tarot_seed=None):
        self.root = os.path.abspath(os.path.expanduser(root))
        self.excludes = set(excludes)
        self.inbox_names = [n.lower() for n in (inbox_names or DEFAULT_INBOX_NAMES)]
        self.inbox_tag = (inbox_tag or "").lstrip("#").lower() or None
        self.cache = {}          # path -> (mtime, size, parsed)
        self.tarot_readings = load_tarot_readings(tarot_path)
        self.tarot_seed = str(tarot_seed) if tarot_seed is not None else self.root

    def scan(self):
        """Re-read what changed, then recompute everything from the whole set."""
        notes = {}
        seen = set()
        for path in iter_notes(self.root, self.excludes):
            try:
                st = os.stat(path)
            except OSError:
                continue
            seen.add(path)
            key = (st.st_mtime, st.st_size)
            cached = self.cache.get(path)
            if cached and cached[0] == key:
                parsed = cached[1]
            else:
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        parsed = parse_note(path, f.read())
                except OSError:
                    continue
                self.cache[path] = (key, parsed)
            rel = os.path.relpath(path, self.root)
            notes[rel] = {
                "parsed": parsed,
                "mtime": st.st_mtime,
                "created": created_date(path, parsed["created_fm"], st),
            }
        for gone in set(self.cache) - seen:
            del self.cache[gone]
        return notes

    # -- link resolution ---------------------------------------------------

    @staticmethod
    def build_index(notes):
        """basename -> rel path, and rel-path-without-extension -> rel path.

        Obsidian resolves `[[Note]]` by basename across the vault. When two
        notes share a basename it applies its own tie-break; this picks the
        shallowest path, then the lexicographically first, so the graph is the
        same picture on every run rather than depending on directory order.
        """
        by_base, by_path = {}, {}
        for rel in sorted(notes):
            stem = os.path.splitext(rel)[0]
            by_path[stem.lower()] = rel
            by_path[stem.replace(os.sep, "/").lower()] = rel
            base = os.path.basename(stem).lower()
            prev = by_base.get(base)
            if prev is None or rel.count(os.sep) < prev.count(os.sep):
                by_base[base] = rel
        return by_base, by_path

    @staticmethod
    def resolve(target, by_base, by_path):
        t = target.strip().replace("\\", "/").lower()
        if t.endswith(".md"):
            t = t[:-3]
        return by_path.get(t) or by_base.get(os.path.basename(t))

    # -- the payload -------------------------------------------------------

    def snapshot(self, name=None, agents=None, now=None):
        seoul = ZoneInfo(TAROT_TIMEZONE)
        if now is None:
            now = datetime.datetime.now(seoul)
        elif now.tzinfo is not None:
            now = now.astimezone(seoul)
        notes = self.scan()
        by_base, by_path = self.build_index(notes)

        # Directed, deduped edges between notes that both exist.
        out_links = {rel: set() for rel in notes}
        for rel, info in notes.items():
            for target in info["parsed"]["targets"]:
                dst = self.resolve(target, by_base, by_path)
                if dst and dst != rel:
                    out_links[rel].add(dst)

        degree = {rel: len(out_links[rel]) for rel in notes}
        for rel in notes:
            for dst in out_links[rel]:
                degree[dst] += 1

        link_count = sum(len(v) for v in out_links.values())
        orphans = sum(1 for rel in notes if degree[rel] == 0)

        tag_counts = {}
        for info in notes.values():
            for tag in info["parsed"]["tags"]:
                tag_counts[tag] = tag_counts.get(tag, 0) + 1

        today = now.date()
        daily = [0] * DAILY_DAYS
        added_today = 0
        for info in notes.values():
            delta = (today - info["created"]).days
            if delta == 0:
                added_today += 1
            if 0 <= delta < DAILY_DAYS:
                daily[DAILY_DAYS - 1 - delta] += 1

        titles = build_titles(notes)
        inbox = self._inbox(notes, titles, today)

        vault_name = name or os.path.basename(self.root)
        return {
            "schema": 3,
            "vault": vault_name,
            "generated_at": now.strftime("%H:%M"),
            "stats": {
                "notes": len(notes),
                "links": link_count,
                "orphans": orphans,
                "tags": len(tag_counts),
                "added_today": added_today,
                "added_7d": sum(daily),
                "daily": daily,
            },
            "tags": self._top_tags(tag_counts),
            "agents": agents or [],
            "graph": self._graph(notes, out_links, degree),
            "recent": self._recent(notes, titles, out_links),
            # The device shows at most INBOX_MAX rows but prints the true total,
            # so "8 shown, 23 waiting" is a fact the panel can state.
            "inbox": inbox[:INBOX_MAX],
            "inbox_total": len(inbox),
            "artwork": self._artwork(vault_name, notes, out_links, degree),
            "daily_tarot": self._daily_tarot(vault_name, today),
        }

    def _daily_tarot(self, vault_name, today):
        date = today.isoformat()
        material = f"{date}\n{vault_name}\n{self.tarot_seed}".encode("utf-8")
        digest = hashlib.sha256(material).digest()
        card_ids = sorted(self.tarot_readings)
        card_id = card_ids[int.from_bytes(digest, "big") % len(card_ids)]
        reading = self.tarot_readings[card_id]
        return {
            "date": date,
            "timezone": TAROT_TIMEZONE,
            "card_id": card_id,
            "orientation": "upright",
            "copy_version": reading["copy_version"],
            "headline": list(reading["headline"]),
            "flow": list(reading["flow"]),
            "caution": list(reading["caution"]),
            "action": list(reading["action"]),
        }

    @staticmethod
    def _top_tags(tag_counts):
        ranked = sorted(tag_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        return [{"name": t[:TITLE_MAX], "count": c} for t, c in ranked[:TAGS_MAX]]

    def _graph(self, notes, out_links, degree):
        """The most-connected notes, and the links among just those.

        The panel has room for fourteen nodes. Which fourteen is decided by
        degree, with the title as the tie-break so the same vault always draws
        the same picture — a graph that reshuffles between polls would cost a
        full refresh every time and tell the user nothing.
        """
        ranked = sorted(notes, key=lambda rel: (-degree[rel], rel))[:NODES_MAX]
        index = {rel: i for i, rel in enumerate(ranked)}
        # Titles are disambiguated against the fourteen nodes actually drawn, not
        # against the whole vault. A graph label is about ten characters wide, so
        # `provisioning/README` truncates to `provisioni…` — which keeps the
        # folder and throws away the name, the exact opposite of the point.
        # Collisions among the drawn nodes are rare; when they happen they still
        # get their folder, and when they do not the label stays short and says
        # what the note is.
        titles = build_titles(ranked)
        nodes = [
            {"id": i, "title": titles[rel], "deg": degree[rel]}
            for i, rel in enumerate(ranked)
        ]
        edges = []
        for rel in ranked:
            for dst in sorted(out_links[rel]):
                if dst in index and index[rel] < index[dst]:
                    edges.append([index[rel], index[dst]])
                elif dst in index and index[dst] < index[rel]:
                    pair = [index[dst], index[rel]]
                    if pair not in edges:
                        edges.append(pair)
        # Deduplicate while keeping the order (a reciprocal pair of links is one
        # line on the panel).
        seen, unique = set(), []
        for e in edges:
            key = tuple(e)
            if key not in seen:
                seen.add(key)
                unique.append(e)
        return {"nodes": nodes, "edges": unique[:EDGES_MAX]}

    def _artwork(self, vault_name, notes, out_links, degree):
        """Build the fixed landscape composition without device-side inference.

        Focus and related-note choices are stable functions of degree, mtime,
        and path.  Every string is already fitted to its fixed display row and
        graph ids/slots are normalized to the six emitted positions.
        """
        def rank(rel):
            return (-degree.get(rel, 0), -notes[rel].get("mtime", 0),
                    rel.casefold(), rel)

        focus = min(notes, key=rank) if notes else None
        vault_titles = build_titles(notes)

        incoming = []
        related = set()
        if focus is not None:
            incoming = sorted(
                (rel for rel in notes if focus in out_links.get(rel, set())),
                key=rank,
            )
            related = set(out_links.get(focus, set()))
            related.update(incoming)
            related.discard(focus)
            selected = [focus] + sorted(related, key=rank)[:ARTWORK_NODES_MAX - 1]
        else:
            selected = []

        fallback = fit_wire(vault_name or "여백", ARTWORK_NOTE_TITLE_CELLS, 63)
        note_title = fit_wire(
            vault_titles[focus] if focus is not None else fallback,
            ARTWORK_NOTE_TITLE_CELLS, 63,
        )
        note_path = "" if focus is None else fit_wire(
            focus.replace("\\", "/").replace(os.sep, "/"),
            ARTWORK_NOTE_PATH_CELLS, 127,
        )
        backlinks = [
            fit_wire(vault_titles[rel], ARTWORK_BACKLINK_CELLS, 63)
            for rel in incoming[:ARTWORK_BACKLINKS_MAX]
        ]

        if selected:
            graph_titles = build_titles(selected)
            nodes = [
                {
                    "id": i,
                    "title": (fit_focus_label(graph_titles[rel]) if i == 0 else
                              fit_wire(graph_titles[rel], ARTWORK_GRAPH_TITLE_CELLS, 63)),
                    "slot": i,
                }
                for i, rel in enumerate(selected)
            ]
            selected_index = {rel: i for i, rel in enumerate(selected)}
            pairs = set()
            for src in selected:
                for dst in out_links.get(src, set()):
                    if dst not in selected_index or dst == src:
                        continue
                    a, b = sorted((selected_index[src], selected_index[dst]))
                    pairs.add((a, b))
            edges = [list(pair) for pair in sorted(pairs)[:ARTWORK_EDGES_MAX]]
        else:
            nodes = [{
                "id": 0,
                "title": fit_focus_label(fallback),
                "slot": 0,
            }]
            edges = []

        headline = [
            fit_wire("기억한 것은", ARTWORK_HEADLINE_CELLS, 127),
            fit_wire("남아 있다.", ARTWORK_HEADLINE_CELLS, 127),
        ][:ARTWORK_HEADLINE_MAX]
        definition_lines = [
            fit_wire("서로 멀리 있던 생각이 만나", ARTWORK_DEFINITION_CELLS, 127),
            fit_wire("새로운 방향을 만드는 순간.", ARTWORK_DEFINITION_CELLS, 127),
        ][:ARTWORK_DEFINITION_MAX]
        return {
            "headline": headline,
            "definition": {
                "headword": fit_wire(note_title, ARTWORK_HEADWORD_CELLS, 63),
                "meta": "노트",
                "lines": definition_lines,
            },
            "note": {
                "title": note_title,
                "path": note_path,
                "backlink_total": len(incoming),
                "backlinks": backlinks,
            },
            "graph": {"nodes": nodes, "edges": edges},
        }

    @staticmethod
    def _recent(notes, titles, out_links):
        ranked = sorted(notes, key=lambda rel: (-notes[rel]["mtime"], rel))[:RECENT_MAX]
        return [
            {
                "time": datetime.datetime.fromtimestamp(notes[rel]["mtime"]).strftime("%H:%M"),
                "title": titles[rel],
                "links": len(out_links[rel]),
            }
            for rel in ranked
        ]

    def _inbox(self, notes, titles, today):
        items = []
        for rel, info in notes.items():
            first = rel.replace(os.sep, "/").split("/")[0].lower()
            in_folder = first in self.inbox_names and "/" in rel.replace(os.sep, "/")
            tagged = self.inbox_tag and any(
                t.lower() == self.inbox_tag or t.lower().startswith(self.inbox_tag + "/")
                for t in info["parsed"]["tags"]
            )
            if in_folder or tagged:
                items.append(
                    {"title": titles[rel], "age_days": max(0, (today - info["created"]).days)}
                )
        # Oldest first: the whole point of showing an inbox on a wall display is
        # what has been sitting in it longest.
        items.sort(key=lambda i: (-i["age_days"], i["title"]))
        return items


def title_of(rel):
    """The note's display name: its basename, as Obsidian shows it."""
    return os.path.splitext(os.path.basename(rel))[0][:TITLE_MAX]


def build_titles(notes):
    """Display titles for a whole vault, disambiguated where they collide.

    A basename is the right title right up until the vault has three notes
    called `README` — and real vaults do, along with `index`, `notes`, and a
    daily note per folder. Three identical rows on the panel are three rows that
    say nothing, so a colliding name is prefixed with its parent folder
    (`docs/README`). Obsidian's own file switcher does the same thing for the
    same reason.

    Only collisions pay the cost: a unique name stays short, which matters on a
    122-pixel-wide column.
    """
    counts = {}
    for rel in notes:
        counts[title_of(rel)] = counts.get(title_of(rel), 0) + 1

    titles = {}
    for rel in notes:
        base = title_of(rel)
        if counts[base] < 2:
            titles[rel] = base
            continue
        parent = os.path.basename(os.path.dirname(rel))
        titles[rel] = f"{parent}/{base}"[:TITLE_MAX] if parent else base
    return titles


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

CAPTURE_MAX_BYTES = 8192
SLUG_MAX = 40
# Characters no filesystem, sync client or Obsidian link should have to deal
# with. '/' and '\' are in here for the obvious reason: the filename is built
# from user text, and it must not be able to name a directory.
SLUG_BAD_RE = re.compile(r'[\\/:*?"<>|#\[\]^\x00-\x1f]')


def slugify(text):
    """A filename from the memo's first line. Never a path, never empty."""
    first = text.strip().splitlines()[0] if text.strip() else ""
    slug = SLUG_BAD_RE.sub(" ", first)
    slug = re.sub(r"\s+", " ", slug).strip(" .")
    # Leading dots would hide the note from the scanner's own walk.
    slug = slug.lstrip(".")
    return slug[:SLUG_MAX].strip() or "memo"


class CaptureError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def capture(root, folder, text, tag=None, now=None):
    """Write one memo into the vault's capture folder. Returns its path.

    Raises CaptureError('empty') for a blank memo and ('too_large') past the
    cap. Every other failure is an OSError, which the caller reports as a 500 —
    a disk that is full or read-only is not the client's fault and there is
    nothing it can do differently.
    """
    now = now or datetime.datetime.now()
    text = (text or "").strip()
    if not text:
        raise CaptureError("empty")
    if len(text.encode("utf-8")) > CAPTURE_MAX_BYTES:
        raise CaptureError("too_large")

    target_dir = os.path.join(root, folder)
    os.makedirs(target_dir, exist_ok=True)

    # The memo's own first line is the filename, and therefore the title the
    # board shows. A `2026-08-10 2104 ` prefix would be the first sixteen
    # characters of a narrow column spent on something the panel already
    # displays as an age in days; the date lives in the frontmatter instead.
    stem = slugify(text)
    path = os.path.join(target_dir, stem + ".md")
    # Never overwrite. Two memos with the same first line is a duplicate-submit
    # or a genuine repeat, and losing one of them silently would be the worst
    # possible behaviour for a capture box.
    n = 2
    while os.path.exists(path):
        path = os.path.join(target_dir, f"{stem} ({n}).md")
        n += 1

    front = [f"created: {now.date().isoformat()}"]
    if tag:
        # The tag as well as the folder, so the memo still reaches the board's
        # inbox if the folder is not one the scanner recognises.
        front.append(f"tags: [{tag.lstrip('#')}]")
    body = "---\n" + "\n".join(front) + "\n---\n\n" + text + "\n"

    # 'x' rather than 'w': if something created the file between the check above
    # and here, fail loudly instead of clobbering it.
    with open(path, "x", encoding="utf-8") as f:
        f.write(body)
    return path


# ---------------------------------------------------------------------------
# Agents
# ---------------------------------------------------------------------------

AGENTS_HELP = """\
A JSON file some other program writes, shaped like:

  [ { "name": "indexer", "state": "running", "last_run": "20:55",
      "processed": 1428, "queued": 3, "progress": 78,
      "note": "embedding 6 new notes" } ]

`state` is running | idle | error | done; `progress` is 0..100 or -1 for none.
Missing file means no agents, which the board shows as an empty board rather
than pretending."""


def load_agents(path):
    if not path or not os.path.exists(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print(f"agents file unreadable ({e}) — serving none", file=sys.stderr)
        return []
    if isinstance(data, dict):
        data = data.get("agents", [])
    if not isinstance(data, list):
        return []
    out = []
    for a in data[:AGENTS_MAX]:
        if not isinstance(a, dict):
            continue
        state = str(a.get("state", "idle"))
        out.append({
            "name": str(a.get("name", ""))[:TITLE_MAX],
            "state": state if state in ("running", "idle", "error", "done") else "idle",
            "last_run": str(a.get("last_run", ""))[:8],
            "processed": int(a.get("processed", 0) or 0),
            "queued": int(a.get("queued", 0) or 0),
            "progress": int(a.get("progress", -1) if a.get("progress") is not None else -1),
            "note": str(a.get("note", ""))[:TITLE_MAX],
        })
    return out


# ---------------------------------------------------------------------------
# Glyph coverage
# ---------------------------------------------------------------------------

def load_device_charset():
    """What the shipped fonts can actually draw, from the generator itself.

    Not a second list to keep in step: `gen_fonts.symbol_set()` IS the set the
    faces were built from. Anything outside it reaches the panel as a tofu box,
    and a tofu box is only visible once the firmware is on the glass — which is
    the worst possible place to discover that a note title has an emoji in it.
    """
    try:
        from gen_fonts import symbol_set
        return symbol_set()
    except Exception as e:                       # noqa: BLE001 - advisory only
        print(f"glyph check unavailable ({e})", file=sys.stderr)
        return None


def check_glyphs(payload, charset, warn=True):
    """Report characters the board cannot draw. Returns the offending set."""
    if not charset:
        return set()
    missing = set()

    def walk(node):
        if isinstance(node, str):
            missing.update(c for c in node if c not in charset and c.isprintable())
        elif isinstance(node, dict):
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(payload)
    if missing and warn:
        shown = " ".join(sorted(missing))
        print(f"warning: {len(missing)} character(s) the board has no glyph for "
              f"and will draw as boxes: {shown}", file=sys.stderr)
    return missing


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    vault = None
    agents_path = None
    vault_name = None
    charset = None
    capture_folder = None      # None = capture disabled

    def _json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.split("?")[0] != "/capture":
            self.send_error(404)
            return
        if not self.capture_folder:
            # 403 rather than 404: the endpoint exists, it is switched off. A
            # client can tell the difference between "wrong address" and "turn
            # it on", and those need different fixes.
            self._json(403, {"ok": False, "error": "capture_disabled"})
            return

        length = int(self.headers.get("Content-Length") or 0)
        if length > CAPTURE_MAX_BYTES * 2:
            self._json(413, {"ok": False, "error": "too_large"})
            return
        raw = self.rfile.read(length).decode("utf-8", errors="replace") if length else ""

        # JSON {"text": "..."} or a bare body, so `curl -d 'buy milk'` works.
        text = raw
        if raw.lstrip().startswith("{"):
            try:
                text = str(json.loads(raw).get("text", ""))
            except ValueError:
                self._json(400, {"ok": False, "error": "bad_json"})
                return

        try:
            path = capture(self.vault.root, self.capture_folder, text,
                           tag=self.vault.inbox_tag)
        except CaptureError as e:
            self._json(400, {"ok": False, "error": e.code})
            return
        except OSError as e:
            print(f"capture failed: {e}", file=sys.stderr)
            self._json(500, {"ok": False, "error": "write_failed"})
            return

        rel = os.path.relpath(path, self.vault.root)
        print(f"captured {rel}", file=sys.stderr)
        self._json(201, {"ok": True, "path": rel})

    def do_GET(self):
        if self.path.split("?")[0] not in ("/vault.json", "/"):
            self.send_error(404)
            return
        payload = self.vault.snapshot(
            name=self.vault_name, agents=load_agents(self.agents_path)
        )
        check_glyphs(payload, self.charset)
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        # The board polls one host forever; keeping the socket saves a connect
        # per poll and, on an https:// URL, a whole TLS handshake.
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("Usage")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=AGENTS_HELP,
    )
    ap.add_argument("vault", help="path to the Obsidian vault directory")
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--name", help="vault name shown on the panel (default: folder name)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="directory name to skip (repeatable)")
    ap.add_argument("--inbox", action="append", default=[],
                    help=f"inbox folder name (repeatable; default: {', '.join(DEFAULT_INBOX_NAMES[:3])}, …)")
    ap.add_argument("--inbox-tag", default="#todo",
                    help="tag that also puts a note in the inbox (default: #todo)")
    ap.add_argument("--agents", help="JSON file of agent statuses — see below")
    ap.add_argument("--tarot-readings", default=DEFAULT_TAROT_PATH,
                    help="validated 78-card Korean tarot copy JSON")
    ap.add_argument("--tarot-seed",
                    help="stable per-device selection seed (default: vault path)")
    ap.add_argument("--dump", action="store_true", help="print the payload and exit")
    ap.add_argument("--no-glyph-check", action="store_true",
                    help="skip warning about characters the board cannot draw")
    ap.add_argument("--allow-capture", action="store_true",
                    help="enable POST /capture, which WRITES a memo into the vault")
    ap.add_argument("--capture-folder",
                    help="folder captured memos land in (default: the first --inbox, or Inbox)")
    args = ap.parse_args()

    if not os.path.isdir(os.path.expanduser(args.vault)):
        sys.exit(f"not a directory: {args.vault}")

    vault = Vault(args.vault, excludes=args.exclude,
                  inbox_names=args.inbox or None, inbox_tag=args.inbox_tag,
                  tarot_path=args.tarot_readings, tarot_seed=args.tarot_seed)
    charset = None if args.no_glyph_check else load_device_charset()

    if args.dump:
        payload = vault.snapshot(name=args.name, agents=load_agents(args.agents))
        check_glyphs(payload, charset)
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return

    # Scan once up front so a slow first walk happens at startup rather than
    # inside the board's first poll, which would look like an unreachable server.
    payload = vault.snapshot(name=args.name, agents=load_agents(args.agents))
    check_glyphs(payload, charset)
    s = payload["stats"]
    print(f"{payload['vault']}: {s['notes']} notes, {s['links']} links, "
          f"{s['orphans']} orphans, {s['tags']} tags, {payload['inbox_total']} in the inbox")

    Handler.vault = vault
    Handler.agents_path = args.agents
    Handler.vault_name = args.name
    Handler.charset = charset
    if args.allow_capture:
        Handler.capture_folder = args.capture_folder or (args.inbox[0] if args.inbox else "Inbox")
    srv = HTTPServer((args.host, args.port), Handler)
    print(f"serving it on http://{args.host}:{args.port}/vault.json")
    if Handler.capture_folder:
        # Said plainly and every time: this is an unauthenticated service that
        # can now create files in somebody's notes.
        print(f"capture ENABLED — POST /capture writes into {Handler.capture_folder}/ "
              f"(anyone on this network can)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
