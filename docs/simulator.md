# The simulator

```bash
cd sim && ./sim.sh
```

Builds the real UI against desktop LVGL, renders every page at the real 648 × 480, binarizes with
the device's exact rule, writes a bitmap per page into `sim/shots/`, and **asserts on the pixels**.
It exits non-zero when anything fails, so this is a test that happens to leave screenshots behind.

```bash
VAULT_URL=http://localhost:8123/vault.json ./sim.sh   # against the reference server
```

With `VAULT_URL` set it runs the device's own fetch-and-parse path — the same `vault_service_fetch`,
the same `vault_parse`, the same bytes — so a change to the wire contract is caught here rather than
on the glass.

## What "the real UI" means

`sim/CMakeLists.txt` compiles `components/vault_core/` directly: the same `ui_vault.c`, the same
four page files, the same `ui_graph.c`, the same generated fonts. The only file that differs between
the simulator and the firmware is the HTTP port (`http_port_curl.c` instead of `http_port_esp.c`).

The binarization is the same threshold the device applies in `main.cpp`:

```c
epd_set_pixel(x, y, (*buffer < 0x7fff) ? EPD_BLACK : EPD_WHITE);
```

That is the whole reason LVGL renders RGB565 on the device rather than its native 1-bit format
(see [graphics.md](graphics.md)): one colour format across host and target means a screenshot is
evidence about the device, not an approximation of it.

The simulator includes `ui_internal.h`, which is otherwise private to the UI files, so its
assertions use the same layout grid the pages do. Duplicating that grid here is the one thing that
would let the assertions lie.

## What it checks

**Glyph coverage.** Every fixed string in `ui_strings.h`, and every string in the loaded snapshot —
note titles, tag names, agent names and notes, graph node titles — against *both* Korean faces. Not
by looking for tofu boxes in the bitmap, which is unreliable, but by asking the font whether it has
each codepoint. This is the check that matters most on this board, because half the strings arrive
over the network and cannot be subset ahead of time.

**Every row of every list inked.** For each agent row, each note row, each inbox row, each tag row,
each activity bar, each graph node and each graph label: is there any ink where it should be? This
catches the failure mode LVGL is silent about — a label positioned past its parent gets clipped away
entirely, so the page looks fine, just with one fewer row than the data had.

**The chrome.** The header and footer rules black across their full width, exactly one filled page
dot and it is this page's, the legend inside its slot, the badge present when it should be.

**Structural blanks.** Agent rows beyond the agent count must be empty, not left showing the
previous snapshot. An agent with no progress figure must have no bar — an empty bar reads as "stuck
at 0%", which is a different and more alarming thing.

**The overlay is opaque.** On e-Paper a hidden page is still physically on the glass until something
covers it, so the provisioning overlay is checked to actually cover the footer.

**Sparse data.** Every page is rendered a second time from a deliberately small snapshot — two
agents, two notes, one tag, an empty inbox, a single graph node. The demo snapshot fills every list
to its cap, which is the widest the pages can get but never exercises hiding the rows that have no
data. A real vault is far more likely to look like the sparse one. The same page checks run
unchanged, because they are data-driven: ink for the rows that exist, blankness for the rows that do
not, and a placeholder for a list that is entirely empty.

**The empty state.** Every page is rendered again with no data at all. A blank e-Paper panel is
indistinguishable from a dead one, so each page must still draw its chrome and say something.

**Header slot collisions.** The header is seven fixed slots in 620 px with 4–8 px between them, all
holding text whose length depends on data, and nothing enforces the arithmetic. The six gaps are
checked for ink directly.

## Output

```
sim/shots/0_stats.png     볼트 통계
sim/shots/1_graph.png     링크 그래프
sim/shots/2_agents.png    에이전트
sim/shots/3_notes.png     최근 노트
sim/shots/4_offline.png   the offline/stale header
sim/shots/5_setup.png     the provisioning overlay
sim/shots/6..9_sparse_*   the same four pages from a nearly-empty vault
```

Each line of output carries an ink percentage. A page that renders nothing comes out near 0%; one
that has gone solid black comes out near 100%. Both are bugs a human skimming filenames would miss.
The four content pages normally sit between 6% and 13%.

`.bmp` is what the simulator writes; `sim.sh` converts to `.png` with `sips` on macOS.

## It earns its keep

On its first run against the finished UI it caught two real bugs, neither of which any host test
could have found:

1. **A missing em dash.** A demo inbox title contained `—`, which was not in the generated font. On
   hardware that is a tofu box, visible only after a four-second refresh.
2. **Labels wrapping instead of ellipsizing.** `ui_lab_w` set only the width, so LVGL auto-sized the
   height and wrapped — putting a second line of the header's vault name on top of the page heading,
   and a second line of a long note title on top of the next row.

Both were one-line fixes. Both would have cost an hour each to find on the board.
