#!/usr/bin/env python3
"""
Tests for tools/vault_server.py, against a vault built in a temp directory.

    python3 tools/test_vault_server.py

No framework and no network: it writes a small vault whose right answers are
countable by hand, scans it, and checks the numbers. The point is that the
*definitions* in the module docstring — what counts as a link, what makes an
orphan, what is a tag — are pinned by something other than prose.

The output is also checked against the wire contract the device parses, so a
payload this produces and a payload `mock_vault_server.py` produces are the same
shape.
"""

import datetime
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vault_server import (  # noqa: E402
    CAPTURE_MAX_BYTES, CaptureError, Vault, build_titles, capture, check_glyphs,
    load_agents, parse_note, slugify, strip_code,
)

FAILURES = []
CHECKS = [0]


def check(cond, label, got=None, want=None):
    CHECKS[0] += 1
    if not cond:
        detail = "" if got is None and want is None else f"  (got {got!r}, want {want!r})"
        FAILURES.append(label + detail)
        print(f"  FAIL {label}{detail}")


def eq(got, want, label):
    check(got == want, label, got, want)


# ---------------------------------------------------------------------------
# A vault whose answers are countable by hand
# ---------------------------------------------------------------------------

NOTES = {
    # Hub: links to three notes that exist and one that does not.
    "MOC.md": """---
tags: [moc, 프로젝트]
created: 2026-08-01
---
# Map of content
See [[Research]], [[Daily/2026-08-10]] and [[Ideas|my ideas]].
Also [[Nonexistent Note]] which has not been written yet.
An embed: ![[Research]]
""",
    "Research.md": """---
created: 2026-08-02
---
Back to [[MOC]]. Tagged #research and #프로젝트.
A second link to the same place: [[MOC]].

```python
# This #hashtag and this [[FakeLink]] are inside a fence.
```
Inline `#alsonotatag` and `[[AlsoNotALink]]` too.
""",
    "Ideas.md": """---
created: 2026-08-03
---
Nothing links out of here except [[Research]].
Numeric #2026 is not a tag. A URL fragment http://x/y#frag is not a tag either.
Nor is a table-of-contents anchor: [Features](#-features) and
[Docs](https://example.com/g#%E2%80%8D-integrating). See also [Spec](Research.md).
<a href="#-html-anchor">Nor an HTML one</a>.
""",
    "Daily/2026-08-10.md": """---
created: 2026-08-10
---
Today. Links to [[MOC]].
""",
    # An orphan: no links in, no links out.
    "Orphan.md": """---
created: 2026-07-01
---
Alone, with a #solo tag.
""",
    # Inbox items, by folder.
    "Inbox/Sort me.md": """---
created: 2026-08-05
---
- [ ] an open task
""",
    "Inbox/Old thing.md": """---
created: 2026-06-01
---
Been here a while.
""",
    # Inbox item, by tag, from outside the inbox folder.
    "Projects/Tagged todo.md": """---
created: 2026-08-08
---
#todo write this up
""",
    # Must be ignored entirely.
    ".obsidian/workspace.md": "not vault content [[MOC]]",
    "Attachments/notes.txt": "not markdown [[MOC]]",
}

TODAY = datetime.date(2026, 8, 10)
NOW = datetime.datetime(2026, 8, 10, 21, 4)


def build_vault(root):
    for rel, text in NOTES.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_strip_code():
    text = "keep [[A]]\n```\ndrop [[B]]\n```\nand `drop [[C]]` here"
    out = strip_code(text)
    check("[[A]]" in out, "strip_code keeps prose links")
    check("[[B]]" not in out, "strip_code drops a fenced link")
    check("[[C]]" not in out, "strip_code drops an inline-code link")


def test_parse_note():
    parsed = parse_note("x.md", NOTES["Research.md"])
    # Two [[MOC]] links collapse to one target: the graph draws one line.
    eq(parsed["targets"], {"MOC"}, "duplicate links to one note are one target")
    eq(parsed["tags"], {"research", "프로젝트"}, "tags come from prose, not from fences")
    eq(parsed["created_fm"], datetime.date(2026, 8, 2), "frontmatter created date")

    ideas = parse_note("x.md", NOTES["Ideas.md"])
    check("2026" not in ideas["tags"], "a numeric #2026 is not a tag")
    check("frag" not in ideas["tags"], "a URL fragment is not a tag")
    # Found by pointing the scanner at real documentation: a table of contents —
    # markdown links in one file, raw HTML anchors in another — turned every
    # anchor into a tag and took over the panel's top-tags list with
    # URL-encoded rubbish.
    eq(ideas["tags"], set(), "no tags from link destinations or HTML attributes")
    # The markdown link to Research.md is still a link, though.
    check("Research" in ideas["targets"], "a relative .md link is still a link")

    inbox = parse_note("x.md", NOTES["Inbox/Sort me.md"])
    eq(inbox["open_tasks"], 1, "counts an open task")


def test_snapshot(vault):
    snap = vault.snapshot(now=NOW)
    s = snap["stats"]

    # Eight .md files; the .obsidian one and the .txt are not notes.
    eq(s["notes"], 8, "note count excludes dotted dirs and non-markdown")

    # Directed, deduped, both ends existing:
    #   MOC->Research, MOC->Daily/2026-08-10, MOC->Ideas   (Nonexistent drops)
    #   Research->MOC        (the two [[MOC]]s are one)
    #   Ideas->Research
    #   Daily/2026-08-10->MOC
    eq(s["links"], 6, "links are distinct existing directed pairs")

    # Orphan.md, Inbox/Sort me.md, Inbox/Old thing.md, Projects/Tagged todo.md.
    eq(s["orphans"], 4, "orphans have no links in or out")

    # moc, 프로젝트, research, solo, todo — '프로젝트' appears twice but is one tag.
    eq(s["tags"], 5, "distinct tag count")
    eq(snap["tags"][0], {"name": "프로젝트", "count": 2}, "top tag is the most used")

    eq(s["added_today"], 1, "one note created today")
    eq(len(s["daily"]), 7, "daily has seven days")
    eq(s["daily"][-1], 1, "daily's last entry is today")
    # Created 08-05 and 08-08 are inside the window; 08-01/02/03 are not (the
    # window is 08-04..08-10), and 06-01/07-01 are far outside.
    eq(s["added_7d"], sum(s["daily"]), "added_7d is the sum of daily")
    eq(s["added_7d"], 3, "three notes created in the last seven days")


def test_graph(vault):
    snap = vault.snapshot(now=NOW)
    g = snap["graph"]
    eq(len(g["nodes"]), 8, "every note fits under the fourteen-node cap")
    eq(g["nodes"][0]["title"], "MOC", "the hub is drawn first")
    # MOC: 3 out + 2 in = 5.
    eq(g["nodes"][0]["deg"], 5, "degree counts both directions")
    eq([n["id"] for n in g["nodes"]], list(range(8)), "node ids are 0..n-1")

    for a, b in g["edges"]:
        check(0 <= a < len(g["nodes"]) and 0 <= b < len(g["nodes"]),
              f"edge [{a},{b}] points at real nodes")
        check(a < b, f"edge [{a},{b}] is normalised low-to-high")
    eq(len(g["edges"]), len({tuple(e) for e in g["edges"]}), "no duplicate edges")
    # The six directed links are four lines: MOC<->Research and MOC<->Daily are
    # each reciprocal, and a reciprocal pair is one line on the panel.
    eq(len(g["edges"]), 4, "a reciprocal pair of links is one edge")


def test_graph_is_deterministic(vault):
    a = json.dumps(vault.snapshot(now=NOW)["graph"], ensure_ascii=False, sort_keys=True)
    fresh = Vault(vault.root, inbox_tag="#todo")
    b = json.dumps(fresh.snapshot(now=NOW)["graph"], ensure_ascii=False, sort_keys=True)
    # A graph that reshuffles between polls costs a full refresh every time and
    # tells the user nothing, so this is a product requirement, not tidiness.
    eq(a, b, "the same vault draws the same graph on a cold scan")


def test_inbox(vault):
    snap = vault.snapshot(now=NOW)
    titles = [i["title"] for i in snap["inbox"]]
    eq(snap["inbox_total"], 3, "two in the inbox folder plus one tagged")
    eq(titles[0], "Old thing", "oldest first — that is what an inbox is for")
    check("Sort me" in titles, "inbox picks up the folder")
    check("Tagged todo" in titles, "inbox picks up the tag from outside the folder")
    for item in snap["inbox"]:
        check(item["age_days"] >= 0, "no negative ages")


def test_titles_disambiguate_on_collision():
    """Found by pointing the scanner at a real tree: three rows saying README.

    A real vault has many notes with the same basename — README, index, a daily
    note per folder. Three identical rows on the panel are three rows that say
    nothing, so a colliding name gets its parent folder and a unique one does
    not (the column is narrow).
    """
    titles = build_titles(["README.md", "docs/README.md", "Ideas.md", "a/b/Ideas2.md"])
    eq(titles["README.md"], "README", "a collision at the root takes no prefix it cannot form")
    eq(titles["docs/README.md"], "docs/README", "a colliding name gets its folder")
    eq(titles["Ideas.md"], "Ideas", "a unique name stays short")
    eq(titles["a/b/Ideas2.md"], "Ideas2", "a unique name keeps no path either")


def test_graph_titles_disambiguate_only_against_the_drawn_nodes():
    """A graph label is ~10 characters wide, so a path truncates to its folder.

    `provisioning/README` renders as `provisioni…` — the folder kept, the name
    thrown away, which is the opposite of the point. So the graph disambiguates
    against the fourteen nodes it actually draws, not against the whole vault:
    a note whose basename is unique among those keeps the short, informative
    form even if some note elsewhere in the vault shares it.
    """
    root = tempfile.mkdtemp()
    try:
        # A hub linking to Sub/Note and thirteen fillers fills the fourteen-node
        # cap exactly, so Archive/Note (degree 0) is scanned but never drawn.
        # The fillers are named Z* so Sub/Note wins the degree-1 tie-break.
        files = {"Hub.md": "[[Sub/Note]]\n" + "".join(f"[[Z{i:02d}]]\n" for i in range(13))}
        files["Sub/Note.md"] = "nothing\n"
        for i in range(13):
            files[f"Z{i:02d}.md"] = "nothing\n"
        files["Archive/Note.md"] = "nothing\n"
        for rel, text in files.items():
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)

        snap = Vault(root).snapshot(now=NOW)
        drawn = {n["title"] for n in snap["graph"]["nodes"]}
        eq(len(snap["graph"]["nodes"]), 14, "the graph is at its cap")
        # Both Note.md files exist, so a vault-wide list would disambiguate both
        # — but only one of them is drawn, so the graph keeps the short name.
        check("Note" in drawn, f"the drawn node keeps its short name (got {sorted(drawn)})")
        check("Sub/Note" not in drawn, "it is not disambiguated against a note nobody drew")
        check("Archive/Note" not in drawn, "the degree-0 collider is not drawn at all")
        # A vault-wide list, which has a wider column, still disambiguates.
        vault_wide = build_titles(["Sub/Note.md", "Archive/Note.md"])
        eq(vault_wide["Sub/Note.md"], "Sub/Note", "the vault-wide title is still qualified")
    finally:
        shutil.rmtree(root)


def test_recent(vault):
    snap = vault.snapshot(now=NOW)
    eq(len(snap["recent"]), 8, "recent is capped at eight and the vault has eight")
    for r in snap["recent"]:
        check(len(r["time"]) == 5 and r["time"][2] == ":", "recent time is HH:MM", r["time"])
        check(r["links"] >= 0, "recent link count is a count")


def test_incremental_rescan(vault):
    """A second scan must not re-read unchanged files, and must see changed ones."""
    first = vault.snapshot(now=NOW)
    cached = dict(vault.cache)
    second = vault.snapshot(now=NOW)
    eq(second["stats"], first["stats"], "an unchanged vault gives an identical answer")
    check(all(vault.cache[p][0] == cached[p][0] for p in cached),
          "unchanged files keep their cache entry")

    # Add a note and confirm it lands.
    path = os.path.join(vault.root, "Later.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("A new note linking [[MOC]].\n")
    third = vault.snapshot(now=NOW)
    eq(third["stats"]["notes"], first["stats"]["notes"] + 1, "a new note is picked up")
    eq(third["stats"]["links"], first["stats"]["links"] + 1, "its link is counted")

    os.remove(path)
    fourth = vault.snapshot(now=NOW)
    eq(fourth["stats"], first["stats"], "deleting it puts everything back")
    check("Later.md" not in [os.path.basename(p) for p in vault.cache],
          "a deleted note is dropped from the cache, not kept forever")


def test_empty_vault():
    """An empty directory must produce a payload the device can parse, not a crash.

    It will be REJECTED by vault_parse() as having no content — which is correct,
    and leaves whatever is already on the panel alone. What matters here is that
    nothing divides by zero on the way.
    """
    root = tempfile.mkdtemp()
    try:
        snap = Vault(root).snapshot(now=NOW)
        eq(snap["stats"]["notes"], 0, "empty vault has no notes")
        eq(snap["stats"]["links"], 0, "empty vault has no links")
        eq(snap["graph"]["nodes"], [], "empty vault has no graph")
        eq(snap["inbox_total"], 0, "empty vault has no inbox")
        eq(len(snap["stats"]["daily"]), 7, "daily is still seven days long")
    finally:
        shutil.rmtree(root)


def test_agents(tmpdir):
    eq(load_agents(None), [], "no agents file means no agents")
    eq(load_agents(os.path.join(tmpdir, "nope.json")), [], "a missing file means no agents")

    path = os.path.join(tmpdir, "agents.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump([
            {"name": "indexer", "state": "running", "last_run": "20:55",
             "processed": 1428, "queued": 3, "progress": 78, "note": "embedding"},
            {"name": "bogus", "state": "levitating"},
            "not an object",
        ], f)
    agents = load_agents(path)
    eq(len(agents), 2, "the string entry is dropped, the two objects are kept")
    eq(agents[0]["name"], "indexer", "name survives")
    eq(agents[1]["state"], "idle", "an unknown state falls back to idle")
    eq(agents[1]["progress"], -1, "a missing progress is -1, not 0")

    with open(path, "w", encoding="utf-8") as f:
        f.write("{ this is not json")
    eq(load_agents(path), [], "unparseable agents file degrades to none")


def test_slugify():
    eq(slugify("ring the dentist"), "ring the dentist", "a plain memo is its own slug")
    eq(slugify("first line\nsecond line"), "first line", "only the first line names the file")
    eq(slugify("   "), "memo", "a blank memo still gets a name")
    eq(slugify("a" * 100), "a" * 40, "the slug is capped")
    # The filename is built from text a stranger on the LAN supplied. It must
    # not be able to name a directory, escape the folder, or hide the note.
    eq(slugify("../../etc/passwd"), "etc passwd", "a slug cannot contain a path")
    eq(slugify("..\\..\\windows"), "windows", "nor a Windows one")
    eq(slugify(".hidden"), "hidden", "nor start with a dot")
    eq(slugify("a/b:c*d?e\"f<g>h|i"), "a b c d e f g h i", "reserved characters go")
    check("\n" not in slugify("x\ny"), "no newline survives into a filename")
    eq(slugify("메모 정리하기"), "메모 정리하기", "Korean is fine in a filename")


def test_capture(tmpdir):
    root = os.path.join(tmpdir, "capture-vault")
    os.makedirs(root)
    now = datetime.datetime(2026, 8, 10, 21, 4)

    path = capture(root, "Inbox", "ring the dentist", tag="#todo", now=now)
    # The filename IS the title the board shows, so it is the memo and nothing
    # else — no timestamp prefix eating a narrow column for a date the panel
    # already renders as an age.
    eq(os.path.basename(path), "ring the dentist.md", "filename is the memo")
    check(os.path.dirname(path).endswith("Inbox"), "it lands in the capture folder")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("created: 2026-08-10" in text, "the memo carries its creation date")
    check("tags: [todo]" in text, "and the inbox tag, so it reaches the board's queue")
    check(text.rstrip().endswith("ring the dentist"), "the body is the memo")

    # A second memo with the same first line must not overwrite the first.
    second = capture(root, "Inbox", "ring the dentist", now=now)
    check(second != path, "a duplicate gets its own file")
    eq(os.path.basename(second), "ring the dentist (2).md", "suffixed")
    check(os.path.exists(path), "the original still exists")

    # A memo whose text is a path must still land inside the folder.
    escape = capture(root, "Inbox", "../../../etc/passwd", now=now)
    check(os.path.realpath(escape).startswith(os.path.realpath(root)),
          f"capture cannot escape the vault (wrote {escape})")
    check(os.path.dirname(escape).endswith("Inbox"), "and cannot escape the folder")

    try:
        capture(root, "Inbox", "   ", now=now)
        check(False, "an empty memo is rejected")
    except CaptureError as e:
        eq(e.code, "empty", "an empty memo is rejected as 'empty'")

    try:
        capture(root, "Inbox", "x" * (CAPTURE_MAX_BYTES + 1), now=now)
        check(False, "an oversized memo is rejected")
    except CaptureError as e:
        eq(e.code, "too_large", "an oversized memo is rejected as 'too_large'")

    # And the scanner must then see what capture wrote — the whole point.
    v = Vault(root, inbox_tag="#todo")
    snap = v.snapshot(now=now)
    titles = [i["title"] for i in snap["inbox"]]
    check("ring the dentist" in titles,
          f"a captured memo shows up in the board's inbox, as itself (got {titles})")


def test_agent_status_cli(tmpdir):
    """tools/agent_status.py is what makes the agents page non-empty."""
    import agent_status

    path = os.path.join(tmpdir, "agents-cli.json")

    eq(agent_status.main(["--file", path, "set", "indexer", "running",
                          "--note", "embedding", "--progress", "78",
                          "--processed", "1428", "--queued", "3"]), 0, "set returns 0")
    agents = load_agents(path)
    eq(len(agents), 1, "one agent")
    eq(agents[0]["name"], "indexer", "name")
    eq(agents[0]["state"], "running", "state")
    eq(agents[0]["progress"], 78, "progress")
    eq(agents[0]["note"], "embedding", "note")
    check(len(agents[0]["last_run"]) == 5, "last_run defaults to now as HH:MM")

    # Updating must keep the agent where it was: the board draws the first six,
    # so an agent that moved to the end on every status change would fall off
    # the panel the moment a seventh existed.
    agent_status.main(["--file", path, "set", "linker", "idle"])
    agent_status.main(["--file", path, "set", "indexer", "done", "--progress", "100"])
    agents = load_agents(path)
    eq([a["name"] for a in agents], ["indexer", "linker"], "order is preserved across updates")
    eq(agents[0]["state"], "done", "state was updated in place")
    eq(agents[0]["note"], "embedding", "an unspecified field is left alone")

    # Out-of-range progress is clamped, and -1 stays -1 (no measurable progress).
    agent_status.main(["--file", path, "set", "linker", "running", "--progress", "500"])
    eq(load_agents(path)[1]["progress"], 100, "progress is clamped")
    agent_status.main(["--file", path, "set", "linker", "running", "--progress", "-1"])
    eq(load_agents(path)[1]["progress"], -1, "-1 survives as 'no progress bar'")

    eq(agent_status.main(["--file", path, "clear", "indexer"]), 0, "clear returns 0")
    eq([a["name"] for a in load_agents(path)], ["linker"], "the agent is gone")
    eq(agent_status.main(["--file", path, "clear", "nobody"]), 1, "clearing nothing is an error")

    # A corrupt file must not stop an agent reporting: refusing to rewrite a file
    # because it is damaged has the failure mode exactly backwards.
    with open(path, "w", encoding="utf-8") as f:
        f.write("{ not json")
    eq(agent_status.main(["--file", path, "set", "indexer", "running"]), 0,
       "a damaged file is rewritten, not fatal")
    eq([a["name"] for a in load_agents(path)], ["indexer"], "and the status lands")

    # Written atomically — no temp file left behind for the server to trip on.
    leftovers = [f for f in os.listdir(tmpdir) if f.startswith(".") and f.endswith(".tmp")]
    eq(leftovers, [], "no temp file survives the write")


def test_glyph_check(vault):
    snap = vault.snapshot(now=NOW)
    charset = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -:/")
    missing = check_glyphs(snap, charset, warn=False)
    # The synthetic vault has Korean in it, which this tiny charset lacks.
    check(missing, "the glyph check notices characters outside the charset")
    eq(check_glyphs(snap, None, warn=False), set(), "no charset means no complaint")


def test_payload_matches_the_wire_contract(vault):
    """Every key the device's parser reads must be present and the right type."""
    snap = vault.snapshot(now=NOW, agents=[])
    for key in ("schema", "vault", "generated_at", "stats", "tags", "agents",
                "graph", "recent", "inbox", "inbox_total"):
        check(key in snap, f"payload has '{key}'")
    for key in ("notes", "links", "orphans", "tags", "added_today", "added_7d", "daily"):
        check(key in snap["stats"], f"stats has '{key}'")
        check(isinstance(snap["stats"][key], (int, list)), f"stats.{key} is a number or list")
    check(isinstance(snap["graph"]["nodes"], list), "graph.nodes is a list")
    check(isinstance(snap["graph"]["edges"], list), "graph.edges is a list")
    for r in snap["recent"]:
        eq(sorted(r), ["links", "time", "title"], "a recent entry has exactly its three fields")
    for i in snap["inbox"]:
        eq(sorted(i), ["age_days", "title"], "an inbox entry has exactly its two fields")
    # It must survive a round trip as UTF-8 JSON, which is what actually goes out.
    json.loads(json.dumps(snap, ensure_ascii=False).encode("utf-8").decode("utf-8"))
    check(True, "the payload round-trips as UTF-8 JSON")


def main():
    tmpdir = tempfile.mkdtemp()
    try:
        root = os.path.join(tmpdir, "second-brain")
        os.makedirs(root)
        build_vault(root)
        vault = Vault(root, inbox_tag="#todo")

        test_strip_code()
        test_parse_note()
        test_snapshot(vault)
        test_graph(vault)
        test_graph_is_deterministic(vault)
        test_inbox(vault)
        test_titles_disambiguate_on_collision()
        test_graph_titles_disambiguate_only_against_the_drawn_nodes()
        test_recent(vault)
        test_incremental_rescan(vault)
        test_empty_vault()
        test_agents(tmpdir)
        test_slugify()
        test_capture(tmpdir)
        test_agent_status_cli(tmpdir)
        test_glyph_check(vault)
        test_payload_matches_the_wire_contract(vault)
    finally:
        shutil.rmtree(tmpdir)

    print(f"\nvault_server: {CHECKS[0]} checks, {len(FAILURES)} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
