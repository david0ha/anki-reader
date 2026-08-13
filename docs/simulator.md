# The landscape artwork simulator

```bash
cd sim && ./artwork_sim.sh
```

This builds the real UI against desktop LVGL and renders it at the panel's native `648 × 480`
resolution. It applies the same RGB565-to-monochrome threshold as firmware, writes
`sim/shots/artwork.png`, and exits non-zero when a visual contract assertion fails. It is a test
that also leaves a preview.

```bash
VAULT_URL=http://localhost:8123/vault.json ./artwork_sim.sh
```

With `VAULT_URL`, the simulator uses the device's own fetch and schema-3 parse path. The producer
sends a normalized daily tarot value; `ui_artwork.c` draws the same bounded value on desktop and
ESP32.

## Composition under test

The composition spends nearly the full glass on two jobs:

- left: one `272 × 464` Rider–Waite–Smith I1 card, rendered without scaling;
- gutter: a ten-pixel hatched deck spine with registration diamonds;
- right: a double cut-corner frame containing date, headline, card identity, flow, caution and
  action;
- frame details: three diamond-notched rules and a card-family seal.

There is no app header, footer, clock, breadcrumb, refresh status, badge, page dot or navigation
chrome.

## What it checks

- output is exactly `648 × 480` in the native landscape coordinate system;
- the card occupies 96% of panel height, remains byte-aligned and preserves its native I1 stride;
- the card, deck spine and reading frame do not overlap;
- the double cut-corner frame, three rules, spine and seal are intact;
- both headline rows and all three two-row reading sections contain ink in their fixed boxes;
- every displayed string exists in the shipped Korean fonts;
- all 78 card descriptors decode and render through LVGL with plausible 1-bit ink polarity;
- the producer's maximum 22-cell headline and 32-cell body rows fit their real font/pixel boxes;
- total monochrome ink stays between blank-screen and accidentally-filled-screen bounds.

`./artwork_sim.sh` is the panel acceptance test for the single Artwork composition.
