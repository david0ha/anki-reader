# The four pages

648 × 480, one bit deep. Every page shares a 44 px header and a 34 px footer, leaving a
648 × 398 content area at y = 46. `KEY0` cycles forward, `BOOT` back.

The grid is fixed and absolute, and nothing on any page reflows. That is a display constraint, not
a style: a row that moves when a number gains a digit is unreadable at a glance, and on e-Paper a
reflow means the whole panel changed and has to be fully refreshed.

## Chrome

```
 14        122          244        336            478      578  606
┌──────────────────────────────────────────────────────────────────┐
│ OBSIDIAN  second-brain  [DEMO]  2026-08-10 (월)  11:23   ((•   ▉ │ 44
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│                        content, 648 × 398                        │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ 1/4 볼트 통계   ● ○ ○ ○   KEY0 페이지 · KEY1 새로고침 · KEY2 길게 Wi-Fi │ 34
└──────────────────────────────────────────────────────────────────┘
```

Fixed slots, not a flex row, for two reasons. The clock is the only thing that gets a targeted
partial refresh, and a targeted refresh needs a rectangle that does not move when the vault name
gets longer. And every slot here has a neighbour it must not touch — which a layout engine would
let it do silently.

The vault name gets its own slot rather than being concatenated onto the brand: a combined
`OBSIDIAN · <name>` label ellipsizes the brand away first, which reads as a device that has
forgotten what it is.

**One badge slot, ranked.** `DEMO` (no URL configured) beats `오프라인` (Wi-Fi down or the last
fetch failed at the transport) beats `오래됨` (the snapshot is older than two poll intervals). Three
indicators competing for 88 px would either overlap or need a layout pass; ranking them means the
header always shows the most important thing wrong, which is all a glance from across a room can
carry anyway.

## 0 · 볼트 통계

```
┌──────────────┬──────────────┬──────────────┬──────────────┐
│    1,428     │    3,910     │      37      │     212      │
│     노트     │     링크     │     고아     │     태그     │
├──────────────┴──────────────┴──────────────┴──────────────┤
│ 최근 7일 활동                        오늘 +6 · 이번 주 +41 │
│      ▁      ▅      █      ▂      ·      ▄      ▃          │
├───────────────────────────┬───────────────────────────────┤
│ 상위 태그                 │ 볼트 상태                     │
│ 프로젝트 ███████████  186 │ 링크 밀도      2.74 개 / 노트 │
│ 데일리   ████████     141 │ 고아 비율             2.6 %   │
│ 영역     █████         88 │ 에이전트       2 / 5 실행 중  │
│ ...                       │ 마지막 동기화         21:04   │
└───────────────────────────┴───────────────────────────────┘
```

Counters are grouped (`1,428`) — the four headline numbers are the first thing read from across a
room, and an ungrouped five-digit number is measurably slower to parse.

The activity bars scale against **the week's own peak**, not a fixed maximum, so a quiet week still
shows its shape rather than seven stubs. A zero day keeps a 2 px stub: a chart that divides by the
value rather than the peak renders it as nothing at all, and "no bar" and "no data" then look
identical. The demo snapshot contains a zero day precisely so the simulator asserts on this.

## 1 · 링크 그래프

The vault's highest-degree notes and the links between them. Deliberately **not** a force-directed
layout — see `ui_graph.h` for the argument, but briefly: physics is iterative (this runs on the task
that owns a panel), seeded (the simulator and the device would draw different pictures from
identical data, which makes the simulator useless as a test), and unbounded (a node ends up
off-canvas and nobody notices until it is on the glass).

Instead: concentric rings, biggest hub at the centre, everything else spread evenly outwards in
degree order. Integer arithmetic throughout, including a 91-entry sine table rather than libm —
`sin()` is only guaranteed to agree between an x86 host and an Xtensa target to within an ulp, which
is exactly enough to move a node one pixel and fail a screenshot test for a reason that has nothing
to do with the layout.

Node radius scales with link degree, against the local range rather than an absolute one, so a vault
of uniformly-linked notes still shows some variation instead of fourteen identical dots.

Draw order is the only subtle thing: **edges, then a white disc per node, then the node's outline.**
The white disc is what stops six edges converging on a hub from turning it into an unreadable black
star. Node titles get an opaque white background for the same reason — a line running through the
middle of a word is unreadable on a panel with no greys to soften it.

`test_graph_layout.c` asserts that every node's circle *and its label box* lie inside the canvas,
for every node count from 1 to 14, on four differently-shaped canvases.

## 2 · 에이전트

```
● indexer      [실행 중]  20:55   처리 1,428   대기 3   [███████░░░]
  새 노트 6건 임베딩 중
───────────────────────────────────────────────────────────────────
○ summarizer   [대기]     18:30   처리 412     대기 0
```

Two lines per agent: the machine-readable row, and what it is actually doing. The second line is the
one a human reads, so it gets the full width.

`progress: -1` (or absent) means the agent has no meaningful completion figure and **no bar is
drawn**. An empty bar there would read as "stuck at 0%", which is a different and much more alarming
thing than "not applicable".

The state word is Korean on the panel and English on the wire, which is why
`vault_agent_state_name()` (for `/api/state`) and the page's own `state_label()` are separate
functions. One is an identifier; the other is prose.

## 3 · 최근 노트

Recent notes on the left, the inbox queue on the right, with a full-height divider — two
ragged-bottomed lists with no rule between them read as one list.

The inbox header shows `inbox_total`, not the number of visible rows. A count that silently equalled
what fits would turn a backlog of forty into a comfortable eight.

This is the page where a subset font would have failed: every title on it arrives over the network.
See [fonts](#fonts-and-why-both-faces-are-full) below.

The inbox is also the one list that can be *added to* rather than only watched, from the companion
app's memo box or a `curl`. That write never touches the board — it goes to the machine serving the
snapshot, which creates the note, and the board sees it on its next poll like any other change. See
[vault-contract.md](vault-contract.md#capture).

## Fonts, and why both faces are full

`ui_font_kr_16` and `ui_font_kr_20` each carry the whole 완성형 set — all 2350 KS X 1001 Hangul
syllables, plus ASCII, plus the typographic punctuation note titles actually contain. About 100 KB
of flash each at 1 bpp, against an 8 MB app partition.

The board this project forked from subset its fonts down to seventy glyphs, because every string it
drew was a literal in its own source. Here, half the strings arrive at runtime. There is no symbol
list that can be derived ahead of time, and the failure mode of guessing is a tofu box on somebody's
note title — visible only after a four-second refresh, on a board on a shelf.

1 bpp because the panel binarizes anyway: anti-aliasing would cost four times the flash to produce
pixels that are then thresholded straight back to black and white.

The 2350 are not a hardcoded table. `tools/gen_fonts.py` derives them from Python's own EUC-KR
codec — the encoding's Hangul block is lead `0xB0..0xC8` × trail `0xA1..0xFE`, which is exactly
25 × 94 = 2350 — so there is no data file to rot.

The remaining gap is a syllable outside 완성형 (old Hangul, or a rare modern combination) or a
symbol outside `S_DATA_PUNCT`. Those would tofu. The simulator checks every string in the snapshot
against both faces, so if it ever happens it fails on a laptop with the offending codepoint printed.

## Where the layout is asserted

`sim/main_sim.c` renders all four pages at 648 × 480 and checks the pixels: every list row inked,
every graph node and label inside the canvas, exactly one filled page dot, the header and footer
rules intact, the overlay opaque, and every string covered by its font. It exits non-zero on any
failure, so `./sim.sh` is a test that happens to leave screenshots behind.

It earns its keep. On its first run it caught two real bugs: a missing em dash in a note title, and
labels wrapping instead of ellipsizing because only their width was set, which put a second line on
top of the row below.
