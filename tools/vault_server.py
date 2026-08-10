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

Read-only, always: it opens `.md` files and nothing else, and never writes into
the vault. The one file it will write is the agents file, and only if you ask
another program to write it.

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

Usage
-----
    python3 tools/vault_server.py VAULT                  # serve on :8123
    python3 tools/vault_server.py VAULT --port 9000
    python3 tools/vault_server.py VAULT --dump           # print the payload
    python3 tools/vault_server.py VAULT --inbox Inbox --inbox-tag '#todo'
    python3 tools/vault_server.py VAULT --agents ~/agents.json
"""

import argparse
import datetime
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

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

# Directory names that are never vault content. Anything else dotted is skipped
# too — `.obsidian` is only the most obvious one.
SKIP_DIRS = {".obsidian", ".trash", ".git", ".github", "node_modules"}

DEFAULT_INBOX_NAMES = ["inbox", "0-inbox", "00-inbox", "_inbox", "인박스", "받은편지함"]

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

    def __init__(self, root, excludes=(), inbox_names=None, inbox_tag=None):
        self.root = os.path.abspath(os.path.expanduser(root))
        self.excludes = set(excludes)
        self.inbox_names = [n.lower() for n in (inbox_names or DEFAULT_INBOX_NAMES)]
        self.inbox_tag = (inbox_tag or "").lstrip("#").lower() or None
        self.cache = {}          # path -> (mtime, size, parsed)

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
        now = now or datetime.datetime.now()
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

        return {
            "schema": 1,
            "vault": name or os.path.basename(self.root),
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
            "graph": self._graph(notes, titles, out_links, degree),
            "recent": self._recent(notes, titles, out_links),
            # The device shows at most INBOX_MAX rows but prints the true total,
            # so "8 shown, 23 waiting" is a fact the panel can state.
            "inbox": inbox[:INBOX_MAX],
            "inbox_total": len(inbox),
        }

    @staticmethod
    def _top_tags(tag_counts):
        ranked = sorted(tag_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        return [{"name": t[:TITLE_MAX], "count": c} for t, c in ranked[:TAGS_MAX]]

    def _graph(self, notes, titles, out_links, degree):
        """The most-connected notes, and the links among just those.

        The panel has room for fourteen nodes. Which fourteen is decided by
        degree, with the title as the tie-break so the same vault always draws
        the same picture — a graph that reshuffles between polls would cost a
        full refresh every time and tell the user nothing.
        """
        ranked = sorted(notes, key=lambda rel: (-degree[rel], rel))[:NODES_MAX]
        index = {rel: i for i, rel in enumerate(ranked)}
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
    ap.add_argument("--dump", action="store_true", help="print the payload and exit")
    ap.add_argument("--no-glyph-check", action="store_true",
                    help="skip warning about characters the board cannot draw")
    args = ap.parse_args()

    if not os.path.isdir(os.path.expanduser(args.vault)):
        sys.exit(f"not a directory: {args.vault}")

    vault = Vault(args.vault, excludes=args.exclude,
                  inbox_names=args.inbox or None, inbox_tag=args.inbox_tag)
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
    srv = HTTPServer((args.host, args.port), Handler)
    print(f"serving it on http://{args.host}:{args.port}/vault.json")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
