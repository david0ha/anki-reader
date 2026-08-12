# Note Link Reader HTML Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a static 648×480 HTML composition in which ten underlined phrases from a real Obsidian MOC paragraph connect one-to-one to surrounding destination-note labels.

**Architecture:** Keep the prototype as one offline HTML file under `docs/prototypes/`, using semantic HTML and an inline SVG whose fixed coordinates connect the central prose to peripheral note labels. Add a small Python standard-library structural test so the exact note mappings, static-only constraint, element counts, and fixed canvas contract can be verified without browser dependencies.

**Tech Stack:** HTML5, CSS3, inline SVG, Python 3 `unittest`/`html.parser`, local static HTTP server for visual verification.

## Global Constraints

- The approved design is `docs/superpowers/specs/2026-08-12-note-link-reader-design.md`.
- The inner `.screen` is exactly `648px × 480px`; browser responsiveness scales the whole screen without reflowing its composition.
- Use exactly ten real link mappings from the approved spec and invent no destination notes.
- The page is static: no anchors, scripts, clicks, hover states, focus states, animation, tooltips, previews, navigation, switching, or remote requests.
- Use annotated `.linked-phrase[data-note]` spans, ten `.note-label[data-note]` labels, and ten SVG `.connector[data-note]` paths with one-to-one matching destination values.
- Use offline system font stacks and inline CSS/SVG only; no external images, fonts, stylesheets, or JavaScript.
- Connectors are one-pixel strokes behind all text; labels use opaque paper-colored backing where needed; linked phrases alone are underlined and end in a small ink dot.
- Keep all visible composition content within the 648×480 screen and at least 12 pixels from its outer edge.
- Do not modify firmware, app, simulator, provisioning HTML, or vault data-contract files.

---

### Task 1: Build and verify the static note-link composition

**Files:**
- Create: `docs/prototypes/note-link-reader.html`
- Create: `tools/test_note_link_reader.py`

**Interfaces:**
- Consumes: the ten visible-phrase/destination mappings and rendering constraints in `docs/superpowers/specs/2026-08-12-note-link-reader-design.md`.
- Produces: a dependency-free HTML document whose structural contract is addressable through `.screen`, `.linked-phrase[data-note]`, `.note-label[data-note]`, and `.connector[data-note]`.

- [ ] **Step 1: Write the failing structural test**

Create `tools/test_note_link_reader.py` with a real `HTMLParser` subclass that reads the intended HTML output and records start tags and attributes. Use this literal mapping as the independent expectation:

```python
EXPECTED_LINKS = {
    "LED bit-shift 실습": "1주차 코드",
    "Mailbox": "실습 4. Mailbox",
    "Event flag·세마포어": "실습 5. Event flag and semaphore",
    "종합 실습": "실습 6. 종합 실습",
    "2주차 과제": "2주차 과제",
    "5주차(Mailbox·MsgQueue)": "5주차 과제",
    "6주차(세마포어·이벤트 플래그)": "6주차 과제",
    "uC/OS-II 구현 소스": "6주차 코드",
    "실습 보드 2": "실습 보드 실습 2",
    "실습 보드 3": "실습 보드 실습 3",
}
```

Test these user-visible behaviors:

```python
def test_has_exact_real_vault_link_mappings(self):
    self.assertEqual(self.parsed.linked_phrases, EXPECTED_LINKS)
    self.assertEqual(set(self.parsed.note_labels), set(EXPECTED_LINKS.values()))
    self.assertEqual(set(self.parsed.connectors), set(EXPECTED_LINKS.values()))

def test_is_static_and_offline(self):
    self.assertEqual(self.parsed.counts["a"], 0)
    self.assertEqual(self.parsed.counts["script"], 0)
    self.assertEqual(self.parsed.remote_urls, [])

def test_declares_fixed_device_canvas_and_layer_contract(self):
    self.assertEqual(self.parsed.screen_style, "width: 648px; height: 480px;")
    self.assertEqual(self.parsed.connector_svg_viewbox, "0 0 648 480")
    self.assertEqual(self.parsed.counts["path.connector"], 10)
```

The parser must accumulate text inside each linked phrase before comparing the mapping, so the test exercises the generated artifact rather than grepping its source.

- [ ] **Step 2: Run the test and verify the expected failure**

Run:

```bash
python3 -m unittest tools/test_note_link_reader.py -v
```

Expected: `ERROR` from `FileNotFoundError` for `docs/prototypes/note-link-reader.html`; this proves the test is red because the artifact does not exist.

- [ ] **Step 3: Implement the single-file composition**

Create `docs/prototypes/note-link-reader.html` with this document structure:

```html
<main class="stage" aria-label="옵시디언 링크 문장 프로토타입">
  <article class="screen" style="width: 648px; height: 480px;">
    <svg class="connections" viewBox="0 0 648 480" aria-hidden="true">
      <!-- exactly ten path.connector elements; each has data-note -->
    </svg>
    <header class="masthead">…</header>
    <section class="reading" aria-labelledby="reader-title">…</section>
    <aside class="note-map" aria-label="연결된 노트">
      <!-- exactly ten .note-label elements; each has matching data-note -->
    </aside>
    <footer class="source">…</footer>
  </article>
</main>
```

Use the approved source paragraph, split into three short prose blocks so it fits the center. Every visible phrase in `EXPECTED_LINKS` must appear once as:

```html
<span class="linked-phrase" data-note="실습 4. Mailbox">Mailbox</span>
```

Place ten peripheral note labels at explicit safe coordinates around the central reading block. Draw ten gentle orthogonal or curved SVG paths behind the text, one for each matching `data-note`, using `vector-effect="non-scaling-stroke"`. Keep the central reading region visually quiet and use paper-colored backgrounds on labels.

Implement the palette and system type stacks from the spec as CSS custom properties. Use pseudo-elements for the subtle linked-phrase terminal dot and CSS gradients for restrained paper grain. The only responsive rule scales `.screen` through a stage-level transform/custom property; it must not change the 648×480 internal layout.

- [ ] **Step 4: Run the structural test and verify it passes**

Run:

```bash
python3 -m unittest tools/test_note_link_reader.py -v
```

Expected: three tests pass with `OK` and no warnings.

- [ ] **Step 5: Serve and inspect the rendered page**

Run a local server from the repository root:

```bash
python3 -m http.server 4173
```

Open `http://127.0.0.1:4173/docs/prototypes/note-link-reader.html` in a browser. Verify the page has meaningful visible content, `.screen` reports exactly 648×480, no visible element overflows the screen, the browser console has no errors, and the screenshot shows central prose with ten readable one-to-one connectors to surrounding labels. Save verification evidence outside source control unless explicitly asked to commit it.

- [ ] **Step 6: Run repository-scope regression checks**

Run:

```bash
git diff --check
python3 -m unittest tools/test_note_link_reader.py -v
git status --short
```

Expected: no whitespace errors; three tests pass; only the intended prototype and test are changed before commit.

- [ ] **Step 7: Commit the implementation**

```bash
git add docs/prototypes/note-link-reader.html tools/test_note_link_reader.py
git commit -m "feat: prototype linked prose as a note map" \
  -m "Render ten real Obsidian wiki-link mappings as underlined prose connected to surrounding note labels on a static 648x480 canvas.\n\nConstraint: Keep the prototype offline and non-interactive\nRejected: Hover previews | user requested a static screen\nConfidence: high\nScope-risk: narrow"
```
