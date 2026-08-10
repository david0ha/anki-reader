#!/usr/bin/env python3
"""
Report an agent's status to the board, in one line.

`vault_server.py --agents FILE` serves whatever is in that file and invents
nothing, which is right — but it leaves the board's agent page empty until
something writes it. This is that something: a tiny CLI any script, cron job or
hook can call.

    tools/agent_status.py set indexer running --note "embedding 6 new notes" \\
                          --progress 78 --processed 1428 --queued 3
    tools/agent_status.py set indexer done --progress 100
    tools/agent_status.py clear indexer
    tools/agent_status.py list

Wrapped around a job, that is the whole integration:

    A=tools/agent_status.py
    $A set indexer running --note "reindexing"
    ./do-the-work && $A set indexer done --progress 100 || $A set indexer error --note "failed"

The file is written atomically (temp file + rename), so the server can never
read a half-written one no matter how often this is called.

Which file
----------
`--file`, else $OBSIDIAN_BOARD_AGENTS, else ./agents.json. Point vault_server.py
at the same path.

The board draws the first six agents in the file, in order, and this preserves
insertion order — so the one you add first is the one that stays visible.
"""

import argparse
import datetime
import json
import os
import sys

STATES = ("running", "idle", "error", "done")
BOARD_SHOWS = 6          # VAULT_AGENTS_MAX
NAME_MAX = 64
NOTE_MAX = 64

DEFAULT_FILE = os.environ.get("OBSIDIAN_BOARD_AGENTS", "agents.json")


def load(path):
    """The file's agents, or [] — a missing or damaged file is not an error here.

    A status reporter that refuses to run because the file it is about to
    rewrite is corrupt has the failure mode exactly backwards.
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return []
    if isinstance(data, dict):
        data = data.get("agents", [])
    return [a for a in data if isinstance(a, dict) and a.get("name")] if isinstance(data, list) else []


def save(path, agents):
    """Atomic: the server may read this file at any moment, including now."""
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    tmp = os.path.join(directory, f".{os.path.basename(path)}.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(agents, f, ensure_ascii=False, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def set_agent(agents, name, state, **fields):
    """Update in place, keeping position; append if new.

    Position is preserved deliberately: the board draws the first six, so an
    agent that moved to the end of the file on every status change would
    disappear off the panel the moment a seventh existed.
    """
    for a in agents:
        if a.get("name") == name:
            entry = a
            break
    else:
        entry = {"name": name}
        agents.append(entry)

    entry["state"] = state
    for key, value in fields.items():
        if value is not None:
            entry[key] = value
    entry.setdefault("processed", 0)
    entry.setdefault("queued", 0)
    entry.setdefault("progress", -1)
    entry.setdefault("note", "")
    return agents


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.split("Which file")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--file", default=DEFAULT_FILE,
                    help=f"agents JSON file (default: {DEFAULT_FILE})")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("set", help="create or update one agent")
    s.add_argument("name")
    s.add_argument("state", choices=STATES)
    s.add_argument("--note", help="what it is doing right now (shown on the panel)")
    s.add_argument("--progress", type=int, help="0..100, or -1 for a task with no measurable progress")
    s.add_argument("--processed", type=int)
    s.add_argument("--queued", type=int)
    s.add_argument("--last-run", help="HH:MM (default: now)")

    c = sub.add_parser("clear", help="remove one agent")
    c.add_argument("name")

    sub.add_parser("list", help="print what the board would show")

    args = ap.parse_args(argv)
    agents = load(args.file)

    if args.cmd == "list":
        for i, a in enumerate(agents):
            shown = "  " if i < BOARD_SHOWS else "· "   # · = past what the panel draws
            pct = a.get("progress", -1)
            # -1 means "no measurable progress", which the panel draws as no bar
            # at all. Printing it as "-1%" would read as a broken percentage.
            bar = f"{pct:>3}%" if isinstance(pct, int) and pct >= 0 else "  —"
            print(f"{shown}{a.get('name',''):<16} {a.get('state',''):<8} "
                  f"{bar}  {a.get('note','')}")
        if len(agents) > BOARD_SHOWS:
            print(f"\n{len(agents) - BOARD_SHOWS} past the {BOARD_SHOWS} the board draws (·)",
                  file=sys.stderr)
        return 0

    if args.cmd == "clear":
        remaining = [a for a in agents if a.get("name") != args.name]
        if len(remaining) == len(agents):
            print(f"no agent named {args.name!r}", file=sys.stderr)
            return 1
        save(args.file, remaining)
        return 0

    name = args.name[:NAME_MAX]
    progress = args.progress
    if progress is not None and progress != -1:
        progress = max(0, min(100, progress))
    set_agent(
        agents, name, args.state,
        note=args.note[:NOTE_MAX] if args.note is not None else None,
        progress=progress,
        processed=args.processed,
        queued=args.queued,
        last_run=args.last_run or datetime.datetime.now().strftime("%H:%M"),
    )
    save(args.file, agents)
    return 0


if __name__ == "__main__":
    sys.exit(main())
