# Note Link Reader HTML Prototype — Design

**Date:** 2026-08-12
**Status:** approved direction; implementation pending
**Reference:** `/Users/ggrrm/Downloads/C60DAF44-9C11-4B21-947A-49F855F1E740 2.PNG`
**Source vault:** `/Users/ggrrm/Desktop/ObsidianBrain`

## Purpose

Build one standalone HTML prototype that demonstrates a sentence as an Obsidian
graph: linked words in the sentence are visibly underlined, and thin connector
lines lead from those words to the titles of their destination notes around the
page. The prototype is visual evidence for a later ESP32/e-paper implementation;
it does not change the firmware, companion app, simulator, or vault data contract.

## Deliverable

Create `docs/prototypes/note-link-reader.html` as a dependency-free, offline,
single-file document. Its inner screen is exactly 648×480 pixels, matching the
target e-paper panel, and scales down as a single unit when the browser viewport
is smaller.

No other production file changes. If a preview image is committed later, it
belongs at `docs/images/note-link-reader.png`, following the repository's visual
evidence convention.

## Content

The central copy is adapted only for line breaks from the `실습 & 과제` paragraph
in:

`/Users/ggrrm/Desktop/ObsidianBrain/임베디드 소프트웨어/_임베디드-소프트웨어-moc.md`

It preserves these ten real wiki-link mappings:

| Visible linked phrase | Destination note |
| --- | --- |
| LED bit-shift 실습 | 1주차 코드 |
| Mailbox | 실습 4. Mailbox |
| Event flag·세마포어 | 실습 5. Event flag and semaphore |
| 종합 실습 | 실습 6. 종합 실습 |
| 2주차 과제 | 2주차 과제 |
| 5주차(Mailbox·MsgQueue) | 5주차 과제 |
| 6주차(세마포어·이벤트 플래그) | 6주차 과제 |
| uC/OS-II 구현 소스 | 6주차 코드 |
| 실습 보드 2 | 실습 보드 실습 2 |
| 실습 보드 3 | 실습 보드 실습 3 |

The page may add a short title and small source label, but may not invent extra
note destinations.

## Visual Direction

The design follows the reference image's quiet editorial character without
imitating the photographed frame or desk:

- **Paper:** warm e-paper white `#f1eee4`, with an extremely subtle monochrome
  grain made from CSS gradients; no imported image assets.
- **Ink:** primary `#171713`, secondary `#59564d`, connector `#817d70`, and faint
  rule `#b7b1a2`. Every mark remains legible when interpreted as monochrome.
- **Type:** a Korean-capable system serif stack for the central prose; a
  handwritten-like system fallback stack for peripheral note labels; a compact
  sans-serif stack for source metadata. No network fonts.
- **Layout:** the paragraph occupies the calm center of the screen. Destination
  note labels sit around its top, sides, and bottom. Connectors are drawn behind
  all text, and labels receive opaque paper-colored backing where a connector
  could cross them.
- **Signature:** each underlined phrase ends in a small ink dot. A unique thin
  connector leaves that dot and terminates at the matching note label, making the
  prose itself read as the graph's hub.

The visual hierarchy is prose first, underlined linked phrases second, peripheral
note titles third, and metadata last. Decoration that does not clarify a link is
excluded.

## Static Behavior

The result is entirely static:

- no click behavior;
- no hover or focus state;
- no animation;
- no tooltips, preview cards, navigation, or content switching;
- no remote requests.

Linked phrases are represented as annotated text spans rather than live anchors,
so opening the prototype cannot mutate or navigate the source vault. Each span
stores its destination in `data-note` for inspectability and for a later data
model, but JavaScript is not required for the visual.

## Rendering Rules

1. Draw grain and connector lines first.
2. Draw endpoint dots and peripheral note label backgrounds next.
3. Draw central prose, underlines, labels, and metadata last.
4. Keep every visible element within the 648×480 screen.
5. Underline only linked words or linked phrases, never the surrounding prose.
6. Use one-pixel connector strokes and avoid crossings through central glyphs.
7. Preserve a minimum 12-pixel safe inset around the device canvas.
8. Respect `prefers-reduced-motion`, although the approved design contains no
   motion in any mode.

## Verification

Verification is visual and structural:

- serve the prototype from a local static HTTP server and open it in a browser;
- confirm the inner `.screen` is 648×480 CSS pixels;
- confirm ten `.linked-phrase[data-note]` elements, ten destination labels, and
  ten connector paths are present;
- confirm there are no live anchors, scripts, remote assets, console errors, or
  elements overflowing the screen;
- capture a browser screenshot and compare it with the approved composition:
  central link-rich prose, surrounding note titles, and readable one-to-one
  connectors.

## Deferred Work

This prototype does not define how note body spans reach the ESP32. The current
vault snapshot payload exposes graph aggregates and recent titles, not body text
with per-span destinations. A later product implementation therefore requires a
separate data-contract design before the screen can use live vault content.
