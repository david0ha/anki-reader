#!/bin/sh
# provision_worktree.sh — make a freshly created git worktree buildable.
#
# The build reads the study catalog out of a SQLite file that lives *outside* this
# repository, and main/CMakeLists.txt names it by a path relative to the project root:
#
#     ${CMAKE_SOURCE_DIR}/../kanjis-backend/data/kanjis-backend.sqlite3
#
# In the main checkout that lands on the sibling directory, which is where the backend
# actually is. In a worktree under .claude/worktrees/<name> it lands on
# .claude/worktrees/kanjis-backend, which does not exist — so `idf.py flash` dies in
# ninja with "missing and no known rule to make it", pointing at a path nobody created.
# The build itself succeeds, because only the flash target depends on the catalog; the
# failure therefore shows up at the moment you are trying to put firmware on a board.
#
# The fix is one symlink beside the worktree, not a copy: the file is ~72 MB, and every
# worktree under the same parent resolves `../kanjis-backend` to the same place, so one
# link serves all of them, now and later.
#
# Deliberately NOT provisioned here:
#   - sdkconfig. It is per-developer, gitignored, and generated once and never re-derived,
#     so copying a neighbour's in is how you get a build that is silently missing whatever
#     sdkconfig.defaults gained since that file was made. Each worktree must generate its
#     own. See CLAUDE.md, "Quick start".
#   - build/. Same reasoning, less subtly.
#
# Usage: provision_worktree.sh [worktree-path]
#   With no argument it reads the Claude Code hook payload on stdin and takes the path
#   from there. Falls back to the current directory. Always exits 0 — a worktree that
#   could not be provisioned is worth a warning, never a failed worktree creation.
#
# Wire this to SessionStart, which fires once the session is running inside the new
# worktree — early enough, since the link is only needed at build time. Do NOT wire it
# to WorktreeCreate: that event is not a notification, it is a provider contract asking
# a hook to CREATE the worktree and print its path to stdout, so a hook that merely
# reports something there fails worktree creation outright.

set -u

warn() { printf 'provision-worktree: %s\n' "$1" >&2; }

# --- 1. which worktree ------------------------------------------------------------
worktree="${1:-}"

if [ -z "$worktree" ] && [ ! -t 0 ]; then
    payload=$(cat 2>/dev/null || true)
    if [ -n "$payload" ] && command -v jq >/dev/null 2>&1; then
        # The payload key has moved between Claude Code versions, so try the plausible
        # spellings rather than pinning one and silently doing nothing on a rename.
        worktree=$(printf '%s' "$payload" | jq -r '
            .worktree_path // .worktreePath // .worktree.path //
            .path // .cwd // .tool_input.path // empty' 2>/dev/null || true)
    fi
fi

[ -z "$worktree" ] && worktree=$(pwd)
[ -d "$worktree" ] || { warn "no such directory: $worktree"; exit 0; }

# --- 2. is this the repo we know how to provision? --------------------------------
# Guard by repo identity, not by path, so this is inert if it ever runs elsewhere.
common_git=$(git -C "$worktree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
[ -n "$common_git" ] || exit 0
main_repo=$(dirname "$common_git")

[ -f "$main_repo/main/CMakeLists.txt" ] || exit 0
grep -q 'kanjis-backend' "$main_repo/main/CMakeLists.txt" 2>/dev/null || exit 0

# A worktree whose parent IS the main repo is the main checkout; nothing to do.
[ "$worktree" = "$main_repo" ] && exit 0

# --- 3. where the backend really lives --------------------------------------------
# Env var wins so a machine with a different layout needs no edit here.
for candidate in \
    "${KANJIS_BACKEND_DIR:-}" \
    "$main_repo/../kanjis-backend" \
    "$HOME/Documents/kanjis-backend"
do
    [ -n "$candidate" ] || continue
    if [ -f "$candidate/data/kanjis-backend.sqlite3" ]; then
        backend=$(cd "$candidate" && pwd -P)
        break
    fi
done

if [ -z "${backend:-}" ]; then
    warn "kanjis-backend not found — set KANJIS_BACKEND_DIR, or 'idf.py flash' will fail"
    warn "on a missing data/kanjis-backend.sqlite3. Plain 'idf.py build' still works."
    exit 0
fi

# --- 4. link it beside the worktree -----------------------------------------------
link="$(dirname "$worktree")/kanjis-backend"

if [ -e "$link" ] && [ ! -L "$link" ]; then
    warn "$link exists and is not a symlink — leaving it alone"
    exit 0
fi

ln -sfn "$backend" "$link" || { warn "could not create $link"; exit 0; }
# stderr, not stdout. Some Claude Code hook events read a value off stdout — the
# WorktreeCreate event, for one, treats it as the path of a worktree the hook was
# supposed to create — so a chatty line here does not just look untidy, it can be
# parsed as an answer and break the thing it was reporting on. Nothing this script
# has to say is a return value.
warn "$link -> $backend"
exit 0
