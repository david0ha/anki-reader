# 오늘의 일진 — the day pillar

The home page shows one line of 사주: the **일진**, the day's 60갑자 (sexagenary) pillar. Implemented
in `components/fortune_core/saju.c`, tested in `test/host/test_saju.c`.

## Scope

**The day pillar only.** A full 사주팔자 (four pillars, eight characters) needs the birth date *and
time*, the solar-term month boundaries and a lunar conversion — and a UI to enter all of it. This
device asks the user for nothing, so it can only show what today is, not who you are.

## The calculation

The sexagenary day count has run unbroken for over two millennia, so the whole thing is one anchor
plus modular arithmetic:

```c
jdn = saju_jdn(y, m, d)                    /* Fliegel & Van Flandern, integer only */
s   = (jdn - 11) mod 60                    /* 0 = 甲子 */
gan = s mod 10                             /* 천간 甲乙丙丁戊己庚辛壬癸       */
ji  = s mod 12                             /* 지지 子丑寅卯辰巳午未申酉戌亥 */
```

## The anchor, and why it is written down

`JIAZI_JDN_RESIDUE = 11` is the load-bearing constant. Get it wrong and the code still returns a
perfectly plausible 갑자 name — just the wrong one, every single day, silently. There is no crash,
no warning, and nothing on screen that looks off unless you happen to check a 만세력.

So it is not derived, it is **pinned against two independent sources**:

1. **Liu Y.T., ["Sexagenary Cycle (六十干支)"](https://ytliu0.github.io/ChineseCalendar/sexagenary.html)** —
   *"the sexagenary date of January 27, 2019 was jiǎ zǐ"*, with JD<sub>noon</sub> = 2458511, giving
   `S = 1 + mod(JD_noon − 11, 60)`.
2. **A Korean 만세력 일진표 for August 2026** ([sazasaju.com](https://www.sazasaju.com/saju/manseryeok/iljin/2026/8)) —
   an entire month from a different tradition and toolchain, agreeing day for day.

Both are baked into `test_saju.c`, along with:

- published JDN values (2451545 for J2000.0, 2440588 for the Unix epoch),
- day-of-week cross-checks (1900-01-01 was a Monday, 2000-01-01 a Saturday) that catch a JDN error
  at the far end of the range,
- a 730-day walk asserting the index advances by exactly one across month ends, a leap day and a
  year end,
- a full 60-day cycle producing 60 *distinct* names, starting at 甲子 and ending at 癸亥.

If you change `saju.c` and the cycle shifts, the build fails. That is the point.

## Day boundary: local midnight

The pillar rolls over at **local midnight**, not at 자시 (23:00) as some traditions and 만세력 apps
offer. This is a deliberate simplification: 23:00–24:00 would belong to the *next* day's pillar under
the traditional rule, and supporting both would mean a setting the device has no way to ask about.

`test_local_midnight_boundary` pins the behaviour explicitly — 2026-08-08 23:30 KST is still 甲寅,
and 00:30 the next morning is 乙卯.

The consequence: **`CONFIG_FORTUNE_TIMEZONE` matters.** With the wrong TZ the device shows the
previous or next day's pillar for hours at a time. It defaults to `KST-9`.

## 년주 — the year pillar

`saju_yearju_for_date()` returns the sexagenary **year** pillar, reusing `saju_iljin_t`. It is a
separate 60-year cycle from the day pillar above — same table, same index math, different anchor:

```c
effective_year = (month < 2 || (month == 2 && day < 4)) ? year - 1 : year
idx = mod(effective_year - 4, 60)          /* 0 = 甲子 */
gan = idx mod 10
ji  = idx mod 12
```

`1984 - 4 = 1980`, and `1980 mod 60 == 0`, so 1984 is 갑자년 — the reason a 60th birthday is
celebrated as 환갑 ("return to the [stem-branch] cycle"). That identity is one of the anchors in
`test_saju.c`; the others (2026 병오년, 2025 을사년, 1900 경자년) are cross-checked against an
independent 만세력 site.

**The year boundary is a fixed Feb 4, and that is an approximation.** A 사주 year actually starts at
입춘, the solar term marking the start of spring — not the Gregorian new year, and not a fixed
calendar date. 입춘 wanders between **Feb 3 and Feb 5** depending on the year (it tracks the sun's
ecliptic longitude, not the calendar). Pinning it to Feb 4 means the pillar can be **off by one day**
in years where 입춘 actually falls on the 3rd or the 5th. Getting the true date right needs a solar
ephemeris, which is out of scope for a device that has no lunar-calendar support anywhere else —
so the approximation is deliberate, and documented here rather than silently accepted.

`test_saju.c` pins the fixed boundary explicitly: 2026-02-03 still belongs to 을사년 (2025's
pillar), while 2026-02-04 and after belong to 병오년 (2026's pillar).

## 오행 — the five elements of a stem

`saju_element_of_gan()` maps a 천간 index (0..9) to its element, 0..4 for 목화토금수. The ten stems
pair up yang/yin within each element in table order, so the mapping is exact integer division:

| gan | 0 甲 | 1 乙 | 2 丙 | 3 丁 | 4 戊 | 5 己 | 6 庚 | 7 辛 | 8 壬 | 9 癸 |
|-----|------|------|------|------|------|------|------|------|------|------|
| element | 0 목 | 0 목 | 1 화 | 1 화 | 2 토 | 2 토 | 3 금 | 3 금 | 4 수 | 4 수 |

Out-of-range input (`gan < 0` or `gan > 9`) returns `-1` rather than indexing off the table.

## What the device does with it

- At boot and again whenever the local date rolls over, `roll_day()` recomputes the pillar **and
  draws a new omikuji** — the board is a fresh slip each morning rather than a stale one from
  whenever it was last powered on.
- The clock is seeded from the battery-backed RTC before Wi-Fi comes up, so the pillar is right
  immediately rather than after SNTP.
- Rendered on the home page as `甲寅 갑인` — Hanja and its Hangul reading, from the 16px subset face.
