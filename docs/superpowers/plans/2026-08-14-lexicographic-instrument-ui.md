# Lexicographic Instrument UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Shorts-derived 648×480 LVGL interface with the approved paper-dominant Lexicographic Instrument UI, including reliable navigation, byte-exact partial refresh, production fonts, and simulator evidence.

**Architecture:** Keep the existing immutable `kanji_t` snapshot, pure `kanji_nav_t` state machine, draw/present separation, and RGB565-to-1-bit path. Put content semantics and geometry in host-testable C, let the LVGL layer only render those decisions, and make the native simulator prove every state and e-Paper-specific invariant.

**Tech Stack:** C11, LVGL 9.4+, ESP-IDF 5.4.3, ESP32-S3/UC8179, CMake host tests, `lv_font_conv` 1.5.3, Python 3 font tooling.

**Spec:** [`../specs/2026-08-14-lexicographic-instrument-ui-design.md`](../specs/2026-08-14-lexicographic-instrument-ui-design.md) and [`../../../DESIGN.md`](../../../DESIGN.md)

## Global Constraints

- The physical canvas is exactly 648×480 and uses only black and white semantic colors.
- Use a 16 px outer edge, 80 px index rail, 16 px gutter, and 520 px main column.
- Keep the grade dock exactly `{112, 344, 520, 80}` with four equal 130 px cells.
- Every refresh rectangle uses half-open `[x1, x2) × [y1, y2)` coordinates; dock bounds are exactly `(112, 344, 632, 424)`.
- Only grade-cursor movement uses a partial refresh. Every changed pixel must remain inside the dock.
- Do not introduce animation, grayscale tokens, opacity hierarchy, rounded cards, pills, shadows, flex layout, scrolling, or a default LVGL theme.
- Keep all user-visible fixed copy in `components/vault_core/include/ui_strings.h`.
- Never display `KEY0`, `KEY1`, `KEY2`, `BOOT`, GPIO names, or implementation vocabulary to the learner; visible keys are `1`, `2`, `3`, and `i`.
- Headwords are never ellipsized. Use the 56 px Japanese display face only when it fits and covers the string, otherwise the existing 28 px multilingual face.
- Preserve all model field limits, three senses, three examples, three parts, three comments, and all FSRS pages.
- The simulator and firmware continue to compile the same UI code, generated fonts, and RGB565 threshold path.
- Do not hand-edit `components/vault_core/fonts/*.c`; regenerate them with `tools/gen_fonts.py`.

---

### Task 1: Text normalization, semantic description pages, and control availability

**Files:**
- Modify: `components/vault_core/include/kanji_model.h`
- Modify: `components/vault_core/kanji_model.c`
- Modify: `components/vault_core/include/kanji_nav.h`
- Modify: `components/vault_core/kanji_nav.c`
- Modify: `components/vault_core/include/ui_strings.h`
- Test: `components/vault_core/test/host/test_kanji_model.c`
- Test: `components/vault_core/test/host/test_kanji_nav.c`

**Interfaces:**
- Produces: `bool kanji_text_has_content(const char *text)`.
- Produces: `size_t kanji_text_collapse_whitespace(char *dst, size_t dst_size, const char *src)`.
- Produces: `kanji_desc_page_t` and `kanji_desc_page_at(const kanji_t *, int)`.
- Produces: `bool kanji_nav_can_press(const kanji_nav_t *, kanji_button_t, const kanji_t *)`.
- Consumed later by: description rendering and context-sensitive footer visibility.

- [ ] **Step 1: Add failing model tests for whitespace normalization**

```c
static void test_display_prose_collapses_ascii_whitespace(void)
{
    char out[64];
    CHECK_INT(kanji_text_collapse_whitespace(
                  out, sizeof out, "  글자 \n\t 유래\r\n 입니다  "),
              strlen("글자 유래 입니다"));
    CHECK_STR(out, "글자 유래 입니다");
    CHECK(kanji_text_has_content(out));
    CHECK(!kanji_text_has_content(" \t\r\n\f\v "));
}
```

Add cases for `NULL`, exact fit, a two-byte-too-small destination, in-place conversion, and mixed Japanese/Korean UTF-8. Assert every truncated output ends on a UTF-8 boundary and is NUL terminated.

- [ ] **Step 2: Run the focused model test and confirm failure**

Run:

```bash
cmake -S components/vault_core/test/host -B /tmp/kanji-ui-host
cmake --build /tmp/kanji-ui-host --target test_kanji_model -j8
/tmp/kanji-ui-host/test_kanji_model
```

Expected: compile failure because the two new model helpers are undeclared.

- [ ] **Step 3: Implement the model helpers without changing raw snapshot hashing**

```c
bool kanji_text_has_content(const char *text)
{
    if (!text) return false;
    for (; *text; text++) {
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n' &&
            *text != '\f' && *text != '\v') return true;
    }
    return false;
}
```

Implement `kanji_text_collapse_whitespace()` using `kanji_utf8_seq_len()` for copied non-ASCII sequences. Collapse ASCII whitespace runs to one regular space, trim both ends, support `dst == src`, and use the same boundary-safe truncation contract as `kanji_str_copy()`.

- [ ] **Step 4: Add failing navigation tests for every semantic-page combination**

For masks `0..7`, populate shape/hook/parts and assert this table:

```c
const kanji_desc_page_t expected[8][3] = {
    { KANJI_DESC_PAGE_NONE },
    { KANJI_DESC_PAGE_SHAPE },
    { KANJI_DESC_PAGE_HOOK },
    { KANJI_DESC_PAGE_SHAPE, KANJI_DESC_PAGE_HOOK },
    { KANJI_DESC_PAGE_PARTS },
    { KANJI_DESC_PAGE_SHAPE, KANJI_DESC_PAGE_PARTS },
    { KANJI_DESC_PAGE_HOOK, KANJI_DESC_PAGE_PARTS },
    { KANJI_DESC_PAGE_SHAPE, KANJI_DESC_PAGE_HOOK, KANJI_DESC_PAGE_PARTS },
};
```

Also assert whitespace-only prose creates no page, a rich description cycles `0 → 1 → 2 → 0`, and a one-page description returns `KANJI_ACT_NONE` on `KEY0`.

- [ ] **Step 5: Add a failing availability matrix and new copy assertions**

```c
CHECK(!kanji_nav_can_press(NULL, KANJI_BTN_KEY0, &rich));
CHECK(!kanji_nav_can_press(&question, (kanji_button_t)KANJI_BTN_COUNT, &rich));

/* no-card question: reveal/hint hidden, refresh/study info available */
CHECK(!kanji_nav_can_press(&question, KANJI_BTN_KEY0, &empty));
CHECK(!kanji_nav_can_press(&question, KANJI_BTN_KEY1, &empty));
CHECK( kanji_nav_can_press(&question, KANJI_BTN_KEY2, &empty));
CHECK( kanji_nav_can_press(&question, KANJI_BTN_BOOT, &empty));
```

Cover rich question, answer without description, one-page sheet, and multi-page description/comments/FSRS. For every reachable state and button, assert the query equals `kanji_nav_press(copy, button, k).action != KANJI_ACT_NONE` and leaves the original nav unchanged. Update exact hint expectations to `정답 보기`, `힌트`, `학습 정보`, `등급 바꾸기`, `확정`, `설명`, `다음 쪽`, `닫기`, and `다음 탭`.

- [ ] **Step 6: Implement the semantic-page and availability interfaces**

```c
typedef enum {
    KANJI_DESC_PAGE_NONE = 0,
    KANJI_DESC_PAGE_SHAPE,
    KANJI_DESC_PAGE_HOOK,
    KANJI_DESC_PAGE_PARTS,
} kanji_desc_page_t;

bool kanji_nav_can_press(const kanji_nav_t *nav,
                         kanji_button_t button,
                         const kanji_t *k)
{
    if (!nav || button < KANJI_BTN_KEY0 || button >= KANJI_BTN_COUNT) return false;
    kanji_nav_t probe = *nav;
    return kanji_nav_press(&probe, button, k).action != KANJI_ACT_NONE;
}
```

Use one internal section predicate for `sheet_available()`, `kanji_sheet_pages()`, and `kanji_desc_page_at()`. Keep the public page-count contract at `>= 1`, but return `KANJI_DESC_PAGE_NONE` when no description section exists.

- [ ] **Step 7: Run all host model and navigation tests**

Run:

```bash
cmake --build /tmp/kanji-ui-host --target test_kanji_model test_kanji_nav -j8
/tmp/kanji-ui-host/test_kanji_model
/tmp/kanji-ui-host/test_kanji_nav
```

Expected: both executables report `ok` with zero failures.

- [ ] **Step 8: Commit the behavior contract**

```bash
git add components/vault_core/include/kanji_model.h components/vault_core/kanji_model.c \
  components/vault_core/include/kanji_nav.h components/vault_core/kanji_nav.c \
  components/vault_core/include/ui_strings.h \
  components/vault_core/test/host/test_kanji_model.c \
  components/vault_core/test/host/test_kanji_nav.c
git commit -m "feat: define lexicographic UI navigation contracts"
```

---

### Task 2: Half-open partial-refresh boundary contract

**Files:**
- Modify: `components/vault_core/include/ui_kanji_layout.h`
- Modify: `components/vault_core/ui_kanji_layout.c`
- Modify: `components/vault_core/include/ui_kanji.h`
- Modify: `components/vault_core/ui_kanji.c`
- Modify: `components/port_bsp/epd_panel.h`
- Test: `components/vault_core/test/host/test_kanji_layout.c`

**Interfaces:**
- Produces: `void kanji_rect_to_half_open(const kanji_rect_t *, int *, int *, int *, int *)`.
- Preserves: `ui_kanji_dock_area()` public signature while fixing its result.

- [ ] **Step 1: Add failing pure boundary tests**

```c
static void test_rectangles_convert_to_half_open_bounds(void)
{
    const kanji_rect_t r = {3, 4, 5, 6};
    int x1, y1, x2, y2;
    kanji_rect_to_half_open(&r, &x1, &y1, &x2, &y2);
    CHECK_INT(x1, 3); CHECK_INT(y1, 4);
    CHECK_INT(x2, 8); CHECK_INT(y2, 10);
}
```

Test nullable output pointers and the current dock width/height. The final exact new-dock assertion is added in Task 4 when geometry moves.

- [ ] **Step 2: Confirm the focused layout test fails**

Run:

```bash
cmake --build /tmp/kanji-ui-host --target test_kanji_layout -j8
/tmp/kanji-ui-host/test_kanji_layout
```

Expected: compile failure because `kanji_rect_to_half_open()` is undeclared.

- [ ] **Step 3: Implement the pure helper and delegate the public accessor**

```c
void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2)
{
    if (!r) return;
    if (x1) *x1 = r->x;
    if (y1) *y1 = r->y;
    if (x2) *x2 = r->x + r->w;
    if (y2) *y2 = r->y + r->h;
}

void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2)
{
    kanji_rect_to_half_open(&kanji_answer_layout()->dock, x1, y1, x2, y2);
}
```

Document the half-open contract in `ui_kanji_layout.h`, `ui_kanji.h`, and `epd_panel.h`. Do not alter the driver loops; they already use exclusive maxima.

- [ ] **Step 4: Run the host layout test**

Run:

```bash
cmake --build /tmp/kanji-ui-host --target test_kanji_layout -j8
/tmp/kanji-ui-host/test_kanji_layout
```

Expected: `ok` with zero failures.

- [ ] **Step 5: Commit the boundary fix**

```bash
git add components/vault_core/include/ui_kanji_layout.h \
  components/vault_core/ui_kanji_layout.c components/vault_core/include/ui_kanji.h \
  components/vault_core/ui_kanji.c components/port_bsp/epd_panel.h \
  components/vault_core/test/host/test_kanji_layout.c
git commit -m "fix: standardize partial refresh bounds"
```

---

### Task 3: Production typography pipeline

**Files:**
- Modify: `tools/gen_fonts.py`
- Modify: `components/vault_core/include/ui_fonts.h`
- Regenerate: `components/vault_core/fonts/ui_font_jp_56.c`
- Modify: `sdkconfig.defaults`
- Modify: `sim/lv_conf.h`
- Modify: `components/vault_core/ui_internal.h`

**Interfaces:**
- Preserves: public `ui_font_jp_56` symbol and generated output path.
- Produces: `UI_F_UTILITY` backed by `lv_font_montserrat_18`.
- Preserves: 1 bpp generated Noto faces and `LV_FONT_FMT_TXT_LARGE=1`.

- [ ] **Step 1: Save a reproducible Sans baseline outside the repository**

Run:

```bash
cmake -S sim -B /tmp/kanji-sim-sans -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/kanji-sim-sans --target kanji_sim -j8
mkdir -p /tmp/kanji-sans-shots
/tmp/kanji-sim-sans/kanji_sim /tmp/kanji-sans-shots
```

Expected: current simulator exits zero and leaves the Sans baseline in `/tmp/kanji-sans-shots`.

- [ ] **Step 2: Add the official Serif source and switch only the hero face**

Add
`https://raw.githubusercontent.com/notofonts/noto-cjk/main/Serif/SubsetOTF/JP/NotoSerifJP-SemiBold.otf`
as `jp-serif-semibold`, expose a matching CLI override, include it in
`--download`, and set:

```python
FACES = {
    "ui_font_kr_16": (16, ("kr-regular", "jp-regular"), symbol_set),
    "ui_font_kr_20": (20, ("kr-medium", "jp-medium"), symbol_set),
    "ui_font_kr_28": (28, ("kr-medium", "jp-medium"), symbol_set),
    "ui_font_jp_56": (56, ("jp-serif-semibold",), hero_set),
}
```

Keep `--bpp 1`, `--no-compress`, `verify_face()`, the 6,713-symbol hero set, and the public symbol unchanged.

- [ ] **Step 3: Configure Montserrat 18 consistently**

In firmware defaults, disable 14, enable 18, and select it as the LVGL default:

```text
# CONFIG_LV_FONT_MONTSERRAT_14 is not set
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_18=y
```

In `sim/lv_conf.h`, set `LV_FONT_MONTSERRAT_14 0`, `LV_FONT_MONTSERRAT_18 1`, and `LV_FONT_DEFAULT &lv_font_montserrat_18`. Rename the private alias to:

```c
#define UI_F_UTILITY (&lv_font_montserrat_18)
```

- [ ] **Step 4: Regenerate and validate the face**

Run:

```bash
python3 tools/gen_fonts.py --dry-run
python3 tools/gen_fonts.py --download
```

Expected: dry-run reports 9,242 body symbols and 6,713 hero symbols; generation exits zero and `verify_face()` finds every requested glyph.

- [ ] **Step 5: Build a clean Serif simulator binary**

Run:

```bash
cmake -S sim -B /tmp/kanji-sim-serif -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/kanji-sim-serif --target kanji_sim -j8
mkdir -p /tmp/kanji-serif-shots
/tmp/kanji-sim-serif/kanji_sim /tmp/kanji-serif-shots
```

Expected: build and simulator exit zero. Preserve both `/tmp` shot directories for visual comparison during Task 6.

- [ ] **Step 6: Commit generated provenance and configuration together**

```bash
git add tools/gen_fonts.py components/vault_core/include/ui_fonts.h \
  components/vault_core/fonts/ui_font_jp_56.c sdkconfig.defaults sim/lv_conf.h \
  components/vault_core/ui_internal.h
git commit -m "feat: add serif display typography"
```

---

### Task 4: Lexicographic Instrument geometry and LVGL presentation

**Files:**
- Modify: `components/vault_core/include/ui_kanji_layout.h`
- Modify: `components/vault_core/ui_kanji_layout.c`
- Modify: `components/vault_core/ui_internal.h`
- Modify: `components/vault_core/ui_common.c`
- Modify: `components/vault_core/include/ui_strings.h`
- Modify: `components/vault_core/ui_kanji.c`
- Modify: `components/vault_core/ui_card_question.c`
- Modify: `components/vault_core/ui_card_answer.c`
- Modify: `components/vault_core/ui_sheet_desc.c`
- Modify: `components/vault_core/ui_sheet_comments.c`
- Modify: `components/vault_core/ui_sheet_fsrs.c`
- Modify: `components/vault_core/include/kanji_model.h`
- Modify: `main/main.cpp`
- Test: `components/vault_core/test/host/test_kanji_layout.c`

**Interfaces:**
- Consumes: Task 1 semantic pages and availability query.
- Consumes: Task 2 half-open helper.
- Consumes: Task 3 display/title/action/body/utility faces.
- Changes: `ui_sheet_desc_update(const kanji_t *k, int page)`.
- Produces: a shared rail/masthead/footer and five paper-dominant screen panes.

- [ ] **Step 1: Replace the old layout tests with the approved fixed-grid contract**

Define a local assertion helper and pin these exact anchors before changing
implementation:

```c
#define CHECK_RECT(r, X, Y, W, H) do { \
    CHECK_INT((r).x, (X)); CHECK_INT((r).y, (Y)); \
    CHECK_INT((r).w, (W)); CHECK_INT((r).h, (H)); \
} while (0)

CHECK_RECT(c->rail,        16,  16,  80, 408);
CHECK_RECT(c->rail_rule,   96,  16,   1, 408);
CHECK_RECT(c->main,       112,  56, 520, 368);
CHECK_RECT(c->footer,      16, 440, 616,  40);
CHECK_RECT(a->dock,       112, 344, 520,  80);
for (int i = 0; i < KANJI_GRADE_COUNT; i++) CHECK_INT(a->cell[i].w, 130);
```

Also assert every rectangle stays on screen, question hero/prompt/counts do not overlap, three answer example rows remain reachable, description prose body is at least 320 px high, and `kanji_rect_to_half_open(a->dock)` yields exactly `(112,344,632,424)` with both X bounds divisible by eight.

- [ ] **Step 2: Confirm the redesigned layout test fails**

Run:

```bash
cmake --build /tmp/kanji-ui-host --target test_kanji_layout -j8
/tmp/kanji-ui-host/test_kanji_layout
```

Expected: failures against the old header/player/right-rail geometry.

- [ ] **Step 3: Implement the fixed geometry as pure integer data**

Use these primary regions:

```c
#define EDGE      16
#define RAIL_W    80
#define GUTTER    16
#define MAIN_X   112
#define MAIN_W   520
#define MAIN_Y    56
#define MAIN_H   368
#define FOOTER_Y 440
#define FOOTER_H  40
```

Question: black text on paper with hero, primary action, secondary state copy, and remaining-count line. Answer: hero/reading/meaning/three fixed example rows above the exact dock. Plain sheet: title/headword and 320 px body. FSRS sheet: fixed body plus five-stat strip. Do not retain inverted header/player/band/action-rail/scrubber fields.

- [ ] **Step 4: Update shared primitives and coordinate handling**

Set `UI_PAD=16` and add both coordinate transforms because screen panes begin at the main origin:

```c
#define LOCAL_X(v) ((v) - kanji_chrome_layout()->main.x)
#define LOCAL_Y(v) ((v) - kanji_chrome_layout()->main.y)
```

Keep stripped square objects. Add a 1 px rule helper implemented as a black fill. Use transparent or cover opacity only. Do not create a new theme, layout engine, or card abstraction.

- [ ] **Step 5: Rebuild the shared chrome in `ui_kanji.c`**

Create:

- an 80 px rail with exactly two blocks: identity and progress;
- a small `KANJIS` wordmark and `연속 N · 오늘 N` masthead in the main column;
- exceptional-state priority `offline → demo → stale`, replacing rail identity;
- healthy battery silence and a masthead indicator only when a fitted battery
  is at or below 20%;
- four fixed footer slots whose separate keycap/action objects read `1`, `2`, `3`, `i`.

Hide both objects in a footer slot when `kanji_nav_can_press()` is false. Call the chrome/footer updater from `ui_kanji_set_data()`, `ui_kanji_set_nav()`, and `ui_kanji_set_status()` so data-dependent visibility never goes stale.

- [ ] **Step 6: Rebuild question and answer screens on paper**

Question screen requirements:

```text
hero: Japanese 56 px serif when eligible, otherwise multilingual 28 px
prompt: 정답 보기 / 오늘 학습 완료 / 데이터 없음
counts: 새 N · 복습 N · 다시 N
```

Remove the black player, icon rail, deck caption, and scrubber. Keep long-headword fallback and add secondary complete/no-data recovery copy. Treat the existing `NULL` snapshot path as the stable no-data state because the public UI API has no separate loading flag.

Answer screen requirements: remove the inverted band, draw hero/reading/meaning/examples in black on paper, retain all three example rows, and preserve exactly one selected black dock cell with white label/span. No object outside the dock may change when only the cursor changes.

- [ ] **Step 7: Rebuild reading sheets and semantic description pagination**

Change the interface everywhere:

```c
void ui_sheet_desc_update(const kanji_t *k, int page);
```

Clamp stale page indexes, call `kanji_desc_page_at()`, and show exactly one of shape/hook/parts. Normalize prose through `kanji_text_collapse_whitespace()` into a `KANJI_BODY_MAX` buffer. Give prose a 520×320 wrap box; show all three component rows on their page. Rework comments and FSRS to use the same paper title/body origin and pager without an inverted band.

- [ ] **Step 8: Restyle setup and device-facing status copy**

Make setup an opaque white screen with one strong top rule, left-aligned title, and direct instructions. Replace English setup status strings in `main/main.cpp` with fixed Korean strings declared in `ui_strings.h` where they reach the glass.

- [ ] **Step 9: Update the body-limit documentation and compile all targets**

Update `KANJI_BODY_MAX`'s comment to the 520×320 prose-page proof. Run:

```bash
cmake --build /tmp/kanji-ui-host -j8
ctest --test-dir /tmp/kanji-ui-host --output-on-failure
cmake -S sim -B /tmp/kanji-ui-compile -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/kanji-ui-compile --target kanji_sim -j8
```

Expected: every host test passes and the native simulator compiles. Pixel assertions may still describe the old design and are replaced in Task 5.

- [ ] **Step 10: Commit the complete LVGL presentation as one coherent change**

```bash
git add components/vault_core main/main.cpp
git commit -m "feat: build lexicographic instrument UI"
```

---

### Task 5: Native simulator acceptance evidence

**Files:**
- Modify: `sim/main_kanji_sim.c`
- Modify: `sim/kanji_sim.sh`
- Test output: `sim/shots/*.bmp` and `sim/shots/*.png` (gitignored)

**Interfaces:**
- Consumes: all layout rectangles, semantic pages, font faces, and public dock bounds.
- Produces: twenty canonical shots and three auxiliary max-content captures.

- [ ] **Step 1: Replace old inverted-player assertions with paper-system failures**

Before implementation, make the simulator require:

```c
const kanji_chrome_t *c = kanji_chrome_layout();
want_ink("question: hero", q->hero, 500);
want_mostly_paper("question: paper field", c->main, 30);
want_ink("chrome: rail divider", c->rail_rule, c->rail_rule.h / 2);
```

Require ink in rail identity/progress, masthead, each visible footer slot, prompt, remaining counts, answer prose, and every reading-sheet title/body. Require question and session-complete full-screen ink coverage at or below 30% while retaining minimum-ink checks so blank screens cannot pass.

- [ ] **Step 2: Add pre-threshold neutral-ramp and authored-style checks**

Inspect raw RGB565 `capture[]` before `write_bmp()`. Decode channels and accept only neutral values where `R5 == B5` and `G6` matches the scaled neutral ramp within one quantization step. Reject chromatic pixels.

Walk the visible LVGL object tree and assert configured text/background/border/line colors are black or white. Allow label background opacity `LV_OPA_TRANSP` and solid objects `LV_OPA_COVER`; reject intermediate opacity. Montserrat edge coverage is allowed in the framebuffer because it is neutral, not an authored gray token.

- [ ] **Step 3: Prove public dock bounds and partial-diff containment**

```c
int x1, y1, x2, y2;
ui_kanji_dock_area(&x1, &y1, &x2, &y2);
if (x1 != 112 || y1 != 344 || x2 != 632 || y2 != 424)
    FAIL("dock accessor does not match approved half-open bounds");
```

Capture thresholded frames for Good, Easy, and Again. XOR each adjacent pair; require at least one changed pixel inside the dock and exactly zero outside `[112,632) × [344,424)`.

- [ ] **Step 4: Render the twenty canonical gallery states**

Keep the existing sixteen state meanings, rename `05-description` to `05-description-shape`, and add:

```text
05b-description-hook
05c-description-parts
15-no-data
16-stale
```

The stale shot uses a valid last card plus `ui_status_t.stale=true`; the no-data shot passes `NULL` and proves dead reveal/hint controls are absent.

- [ ] **Step 5: Add three auxiliary maximum-content captures**

Use exact names:

```text
aux-description-shape-max
aux-description-hook-max
aux-description-parts-max
```

Find the widest non-whitespace single-byte glyph in `ui_font_kr_16`, fill each prose buffer with `KANJI_BODY_MAX - 1` copies, normalize it, and use `lv_text_get_size()` with the real 520 px width and style spacing. Assert natural height `<= 320`. Fill all part fields to their model limits and require visible ink in all three page rows.

- [ ] **Step 6: Run the native simulator and inspect the gallery**

Run:

```bash
cd sim && ./kanji_sim.sh
```

Expected: exit zero, exactly twenty canonical filenames plus the three `aux-` files, and PNG copies on macOS. Inspect question, answer, all three description pages, complete, offline, stale, no-data, and setup at native size.

- [ ] **Step 7: Commit simulator evidence code**

```bash
git add sim/main_kanji_sim.c sim/kanji_sim.sh
git commit -m "test: prove lexicographic UI pixel contracts"
```

---

### Task 6: Documentation, full verification, and product handoff

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/simulator.md`
- Modify: `docs/graphics.md`
- Modify: `docs/esp-idf-development.md`
- Modify: `docs/epaper-5in83.md`
- Modify: `DESIGN.md` only if verified implementation chose the approved Sans fallback

**Interfaces:**
- Produces: reproducible verification instructions and final size/shot counts.

- [ ] **Step 1: Update documentation to match the shipped system**

Document the paper-dominant rail/main layout, 20 canonical + 3 auxiliary captures, semantic description pagination, half-open dock contract, Noto Serif hero provenance, Montserrat 18 utility face, pre-threshold palette/style validation, and the current simulator commands. Remove claims about a filled player, inverted sheet bands, action rail, Montserrat 14, and sixteen states.

- [ ] **Step 2: Run the complete non-hardware verification stack**

Run from the repository root:

```bash
cmake -S components/vault_core/test/host -B /tmp/kanji-ui-final
cmake --build /tmp/kanji-ui-final -j8
ctest --test-dir /tmp/kanji-ui-final --output-on-failure
sh components/provisioning/test/run.sh
sh components/user_app/test/run.sh
python3 tools/test_mock_kanji_server.py
python3 tools/test_kanji_server.py
(cd sim && ./kanji_sim.sh)
```

Expected: every command exits zero.

- [ ] **Step 3: Build clean firmware and enforce partition headroom**

Run:

```bash
. ~/esp/v5.4.3/esp-idf/export.sh
rm sdkconfig
idf.py build
python3 - <<'PY'
from pathlib import Path
partition = 8 * 1024 * 1024
binary = Path("build/obsidian_board.bin")
used = binary.stat().st_size
free = partition - used
print({"binary": str(binary), "used": used, "free": free})
raise SystemExit(0 if free >= 512 * 1024 else 1)
PY
```

Expected: firmware builds and reports at least 524,288 bytes free in the 8 MiB application partition.

- [ ] **Step 4: Run a separate code-review and verification pass**

Give the reviewer the design contract, spec, this plan, the complete diff, host/producer/simulator outputs, firmware size output, and native screenshots. Require zero blocking findings for coordinate semantics, e-Paper refresh containment, missing glyphs, hidden/dead controls, overflow, and documentation drift. Fix every blocking or high-confidence warning, then rerun the affected command and the full simulator.

- [ ] **Step 5: Commit documentation and verification adjustments**

```bash
git add README.md CLAUDE.md docs DESIGN.md
git commit -m "docs: document lexicographic instrument UI"
```

- [ ] **Step 6: Record the remaining physical-panel gate without claiming it passed**

The software handoff must state that physical A/B review still needs the actual 5.83-inch panel at 50, 70, and 100 cm, plus repeated Good/Easy/Again/Hard partial refreshes through automatic full-refresh promotion. If the serif closes counters or loses hairlines, regenerate `ui_font_jp_56` from Noto Sans JP Regular, rerun Tasks 3–6 verification, and update `DESIGN.md` to record the approved fallback.
