# Stat system rework: design report (2026-09-26, revised)

## Build status (2026-09-26, v27)

**Built:** [`DLS18_career_market_v27.apk`](build/DLS18_career_market_v27.apk) (61,797,454 bytes), based on v26 with
`libDLS18_market_v18.so` and `libCareerMarket_v27.so`. The gameplay library is byte-identical to v14/v26; v27 adds inline Squad club navigation and streamlined Finances actions, while retaining the inline Sales position toolbar and
independent persistent Market/Sales position filters while retaining deterministic rating, market-value, and wage
sorting. APK SHA-256:
`D0ED942881E65A3FD001D6CADCF17BFE22CCE08D65FAFF3EB9B7BF85E5D1DEAF`. Mod library SHA-256:
`05DDE1F7AAA0E7D591D7A7207721FCF30E3CA04255726E63686173B363FA3B64`. Career bridge SHA-256:
`8114D147F492327CC32157334417E698A27A697EC691533DC426C8835CF684FD`.

**Measured results:** input-map minimum slope 0.054313 on raw 20–40; sprint speed ratios 1.215 (62/85) and 1.106
(68/80); jog ratio 1.180 (62/85); full-sprint times 4.876 / 3.103 seconds; shot errors 8.479° / 3.873° / 0.948° /
0.824° at shooting 35 / 54 / 85 / 92; standing duel odds 9.888% / 50% / 90.112% at Δ −20 / 0 / +20. Maximum
duel-table interpolation error is 0.3511 percentage points. `analysis/stat_curves.py` reports `ALL PASS`, as does
`mod/test_patch.py ...libDLS18_market_v18.so`; `mod/career_market/tests/test_hooks.py
...libDLS18_market_v18.so` reports `ALL OK`, including 13/13 pagination and 5/5 selected-detail wiring checks. Patch
emulation covers raw stats 0–99 at a nonzero load address and both duel outcomes; it also checks that on-ball
close-control urgency rises strictly for every control rating from 0 to 99. Measured close-control urgency is 670 at
control 30, 680 at 40, 985 at 70, and 1280 at 99.

**Market UI update:** v27 retains native `CFEPlayerCard` two-column grids, four-item pages, and selected details. The
Market tab now keeps position, club, and name-search controls above the card grid. Position cycles in place and active
filters use the highlighted button scheme. Page navigation stays in the footer, so redundant Previous/Next actions
were removed from the player action pane. Sales, Shortlist, and Squad now follow the same model: card/page navigation
selects a player, while the right pane contains only contextual actions. Primary submit, accept, confirm, continue,
offer, renewal, and get-offers actions use the verified highlighted button scheme. Back returns child pages to their
owning tab while preserving the active selection and exits only from root pages. A teal outline is drawn over the
selected card to link it visually to the right pane. The Market toolbar cycles through rating, value, and wage sort
orders using an iterative merge sort. Market and Sales retain separate position filters across tab changes, Sales
cards apply their filter consistently, and Sales position filtering now cycles inline above the card grid. The right pane
shows the selected player's or offer's available details above its actions; empty player lists show their explanation
there as well. Four or more actions use two columns when player-card details are shown. Shared layout math reserves
space for both text and controls; the host harness checks 1–10 actions at 768 px and 300 px screen heights, including
button bounds and minimum tap area. Drawing and touch behavior have not been exercised in the game.

**Device status:** v27 is built but not installed or exercised in-game. Dynamic-difficulty
variables 0x13–0x15 are set to zero through the career price-rule hook. The stock interpolation API still accepts an
integer stat, so some downstream gameplay values can share an outcome after its Q8.8 input is rounded; the direct
close-control pace plateau at control 30/40 is removed.

User feedback: "it is actually hard now to win against bigger teams almost impossible". The user asked for "a smoother
and wider gap rather than a hard code".

---

## 1. How stats reach gameplay today

The player's stats are raw 0–99 bytes on `CPlayer`: tackling `+0x123`, control `+0x127`, passing `+0x128`, shooting
`+0x12a`, strength and others nearby. They reach gameplay in three ways:

| Path | What uses it | Current mod behaviour |
|---|---|---|
| **A. `CPlayer::AttributeInterpolate_Internal`** (33 call sites, full list in `analysis/stat_audit.csv`) | speed, jog/sprint speed, acceleration, ball speed, first touch, dribble touch, tackle reach, shot placement, shot assist, GK charge… | Hook at `0x2daebc` (`cave2_stat`): Q8.8 softplus map below 48, a smooth blend to identity from 48–56, then raw identity at 56+. The widened mechanic-specific `lo/hi` ranges live in `retune_patches()`. |
| **B. Raw stat reads in mod code** | standing/slide duels (`cave2_duel_roll`), foul scaling (`cave2_foul`), collisions (strength²), animation crossfade, dribble tiers, close-control urgency | Duels use a logistic curve on tackling minus control plus half the strength difference; standing tackle adds a facing bonus. Foul chance uses a smooth table from raw tackling. On-ball close-control pace is strictly increasing over raw control 0–99. |
| **C. Kick error** (`ACT_KickErrorAccuracyGetRange`, `cave2_err_entry/exit`) | shot and pass direction error | Uses raw shooting/passing bytes in its own convex power-2.6 curve; it bypasses the global map. Type-2 short passes keep their fixed value. |

## 2. Why it is too hard for a weaker team (measured)

Club-league population (3,222 players, `data/dls18_dataset.json`):
- Overall rating: median 71, p10 64, p90 79. Team best-XI average: median 73, p90 80.
- A young Dream League team is about 56–65.
- Individual stats: speed median 73.5 (p5 65.5, p95 85.2); ball control median 66 (p5 26); shooting median 54 (p25 45);
  tackling median 60.

### 2.1 The global curve saturates and double-dips

Effective stat fed to `InterpolateClamp` (window 40–99):

| raw stat | 30 | 40 | 50 | 55 | 60 | 62 | 65 | 70 | 75 | 80 | 85 | 90 | 99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| stock | 40* | 40 | 50 | 55 | 60 | 62 | 65 | 70 | 75 | 80 | 85 | 90 | 99 |
| **current** | 40* | 40* | 40.6 | 48.8 | 56.9 | 60.1 | 65 | 73.1 | 81.2 | 89.4 | 97.5 | 99* | 99* |

\* = clamped (flat).

- Everything at or below about 50 is identical (worst), and everything at or above about 89 is identical (best).
- Below the pivot 65 every stat is **worse than stock**, and above it better. A young team loses on every stat against
  everyone, no matter what its opponents are like.
- The widened per-site ranges (retune) stack on top of the ×1.625 stretch, so the gap is amplified twice:

| Mechanic | 62 vs 85 player | stock | current |
|---|---|---|---|
| Sprint speed ratio | | 1.077 | **1.201** |
| Seconds to full sprint | | 4.5 / 3.9 | 4.6 / **2.9** |
| Sprint speed ratio, 68 vs 80 | | 1.040 | 1.100 |

### 2.2 Shooting: weak and average shooters are crippled

Shot direction error by shooting stat:

| shooting | 35 | 45 | 54 (median) | 62 | 70 | 78 | 85 | 92 |
|---|---|---|---|---|---|---|---|---|
| stock | 2.8° | 2.6° | 2.3° | 2.0° | 1.8° | 1.5° | 1.2° | 1.0° |
| **current** | 14.8° | 14.8° | **13.1°** | 10.1° | 7.1° | 4.1° | 1.4° | 1.05° |

A median shooter gets about 13° (5.6× stock). A 62 gets 10°, while an 85 gets 1.4°. That is the "our shots all miss,
theirs all go in" feeling.

### 2.3 Duels are linear with hard walls

Standing tackle, by Δ = tackling − control (before the strength and facing terms):

| Δ | −30 | −20 | −10 | −5 | 0 | +5 | +10 | +20 | +30 |
|---|---|---|---|---|---|---|---|---|---|
| current | 15 | 15 | 35 | 45 | 55 | 65 | 75 | 95 | 95 |

From Δ ≈ −20 downward, the weaker side sits on the 15% floor. From +20 upward the stronger side never fails, and the facing
bonus (+20 points) pushes more cases onto the wall. A big team's dribblers (control 80+) against a young team's defenders
(tackling around 60) are almost never stopped.

### 2.4 Other difficulty factors (not the stat curve, but they add up)
- **Dynamic difficulty.** The stock game raises AI difficulty when the user spends coins
  (`CProfileGameSettings::IncDynamicDifficulty`; config vars 0x13 BuyPlayer, 0x14 StadiumPurchase and 0x15 PlayerDev,
  bundled 4/1/2). The v7 economy makes stadium and training spending routine, so the AI gets harder as the user uses the
  economy as intended. The market's signings bypass `SignPlayerAttempt`; verify whether that path bumps difficulty.
- The user's chosen difficulty level applies as in stock.

## 3. Design principles (revised after user feedback: "continuous, and the gap must be wider so ratings are noticeable")

1. **Continuous everywhere.** No flat regions and no hard clamps anywhere a real player's stat can be. Every extra point
   of stat always changes the outcome. This includes the tails: a 30 and a 40 must differ, and so must a 90 and a 99.
2. **Wide, noticeable gaps.** Rating differences must be clearly felt: wider than stock, and at least as wide as the
   current build where it matters (pace, acceleration, finishing, duels).
3. **Put the width in the right place.** `XMATH_InterpolateClamp` only accepts stats 40–99, so **stretching the input
   stat is what causes the flat zones**. Keep the input mapping near identity (continuous), and create the width in each
   mechanic's **output** range or response curve, which can be as wide as needed without clamping.
4. **No global penalty for being low-rated.** Above about 55 the input stat is passed unchanged: a 60 plays as a 60. The
   difference between two players comes only from their stats, through each mechanic's response.
5. **Contests are decided by the difference between the two players**, on a continuous S-curve (logistic) with
   asymptotes near 5% and 95%. It's wide in effect but never a wall.
6. **One source of truth.** Every curve is a formula in Python (`build_mod.py`), sampled into a lookup table written into
   the cave, and printed by a report script. Tests compare the emulated ARM code against the same formula.

Honest trade-off: wider gaps make a big team harder to beat, by design. What makes it fair rather than "impossible" is
continuity (no automatic wins or losses, no flat worst case for a whole squad of weak stats) and a squad that can improve
(market, training). The dynamic-difficulty quick win in section 8 removes an unrelated extra handicap.

## 4. Proposed design

### 4.1 Continuous input map (replaces `cave_stat`'s linear stretch)
Default knee: 7. For raw stat `s <= 48`, `q(s) = 40 + 7·ln(1 + e^((s − 40)/7))`. For `48 < s < 56`, blend
smoothly to identity with `t = (s − 48)/8`, `w = t²(3 − 2t)`, and `s' = q(s) + w(s − q(s))`. At 56 and above, `s' = s`.
The blend makes both value and slope meet the identity at 56.

| raw | 10 | 20 | 30 | 35 | 40 | 45 | 50 | 55 | 56 | 60 | 62 | 70 | 85 | 99 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **implemented s'** | 40.096 | 40.391 | 41.504 | 42.789 | 44.852 | 47.789 | 51.269 | 55.033 | 56 | 60 | 62 | 70 | 85 | 99 |

The cave stores 15 u16 Q8.8 samples for raw stats 0, 4, …, 56 and linearly interpolates between samples. The formula is
strictly increasing; its minimum slope on raw 20–40 is 0.054313, and the identity branch begins at 56. The stock API takes
an integer stat, so the computed Q8.8 value is rounded before its tail call; gameplay output retains that API's integer
granularity.

### 4.2 Width in the output ranges (re-derive `retune_patches()`)
Since the input is no longer stretched, widen the per-site `lo/hi` ranges of the mechanics that should show ratings.
Optionally give each a convex exponent (`v = lo + (hi − lo)·t^p`, table-driven). Targets:

| Mechanic | Stat | Target for a 62 vs 85 player | Stock | Current |
|---|---|---|---|---|
| Sprint speed | speed | ratio 1.20–1.25 (e.g. linear 2950 → 5000 gives 1.215) | 1.077 | 1.201 |
| Jog / run speed | speed | ratio 1.15–1.20 | 1.07 | 1.16 |
| Acceleration to full sprint | acceleration | about 5.0 s vs 3.1 s | 4.5 / 3.9 | 4.876 / 3.103 |
| Ball speed, first touch, dribble touch | control | about 1.3× the current width, continuous | — | clamped below 50 |
| Shot placement / assist | shooting | keep the current range (20 → 80%, −80 → 96) now that input no longer clamps | — | — |

A 68 vs 80 pair should differ by at least 1.09 in sprint speed (current 1.10). Very low raw stats (below 45) are rare for
the stats that matter (speed p5 = 65.5), so a slow low end is acceptable, but it must stay continuous.

### 4.3 Kick error: its own convex response curve on the RAW stat
Kick error bypasses the global map. For raw stats 25–99, `x = (s − 25)/74`; below 25, continue it smoothly with
`u = s − 25`, `x = u/74 + (13/92500)u²`. Then
`err(s) = best + (worst − best)·(1 − x)^2.6`. The continuation matches value and slope at 25 and removes the old flat tail;
raw 0 is therefore slightly worse than the 12° range at 25.
- Shots: 12° at raw 25, 0.8° at raw 99.
- Passes: 5° at raw 25, 0.6° at raw 99.
- The table covers all 100 valid raw stats in Q1.15 and is strictly decreasing.

| shooting | 35 | 45 | 54 (median) | 62 | 70 | 78 | 85 | 92 |
|---|---|---|---|---|---|---|---|---|
| stock | 2.8° | 2.6° | 2.3° | 2.0° | 1.8° | 1.5° | 1.2° | 1.0° |
| current | 14.8° | 14.8° | 13.1° | 10.1° | 7.1° | 4.1° | 1.4° | 1.05° |
| **proposed** | 8.5° | 5.7° | 3.9° | 2.7° | 1.8° | 1.2° | 0.95° | 0.82° |

An 85 is still about 4× more accurate than a median shooter (stock 1.9×), and elite shooters are sharper than today. An
average striker can score, and bad shooters (defenders) still miss a lot. Keep type 2 short passes stock.

### 4.4 Duels: continuous logistic on the difference
Standing tackle:
- `z = (tackling − control) + 0.5·(strength_d − strength_c) + 8 if facing the carrier within 45°`.
- `P = 5 + 90 / (1 + e^(−z/7))` %.
- Slide tackle: the same, with asymptotes 4–88% and z shifted by −3.

| Δ | −30 | −20 | −10 | −5 | 0 | +5 | +10 | +20 | +30 |
|---|---|---|---|---|---|---|---|---|---|
| current | 15 | 15 | 35 | 45 | 55 | 65 | 75 | 95 | 95 |
| **proposed** | 6.2 | 9.9 | 22.4 | 34.6 | 50.0 | 65.4 | 77.6 | 90.1 | 93.8 |

The measured standing values at Δ −20/0/+20 are 9.888% / 50% / 90.112%. The CAVE2 Q0.16 sigmoid table spans
z = −160…+160 in steps of 4, covering every z possible from valid raw stats, strength, facing, and slide shift. Its
maximum interpolation error over the valid half-stat inputs is 0.3511 percentage points. Encoded standing endpoints are
5.0078% and 94.9922%; slide endpoints stay inside 4–88% as well. The underlying logistic approaches but does not reach
its asymptotes. Foul scaling derives from `(160 − tackling)/90` and is smoothly softened with default width 0.1 into the
open interval (0.6, 1.4); the Q4.12 table is strictly decreasing across raw stats 0–99.

### 4.5 Leave as is (already continuous or context-specific)
- Collisions (strength², proportional).
- Animation crossfade.
- Dribble tiers (clip preference).
- Close-control urgency.
- Stamina.

Re-measure them after 4.1, since their inputs change slightly where they go through path A.

## 5. Acceptance targets

| Metric | Stock | Current | Target |
|---|---|---|---|
| Input map: curve/table monotonicity and slope | raw input is clamped below 40 | current contrast has flat tails | **strictly increasing curve and Q8.8 table**; slope 0.054313 at the low end of 20–40, exactly 1 from 56 up |
| Sprint speed ratio 62 vs 85 / 68 vs 80 | 1.077 / 1.040 | 1.201 / 1.100 | 1.20–1.25 / at least 1.09 |
| Seconds to full sprint, 62 vs 85 | 4.5 / 3.9 | 4.6 / 2.9 | 4.876 / 3.103 (about 5.0 / 3.1) |
| Shot error, shooting 35 / 54 / 85 / 92 | 2.8 / 2.3 / 1.2 / 1.0° | 14.8 / 13.1 / 1.4 / 1.05° | about 8.5 / 3.9 / 0.95 / 0.82° (±10%) |
| Standing tackle, Δ = −20 / 0 / +20 | 100% at any Δ | 15 / 55 / 95 (walls) | about 10 / 50 / 90, asymptotes 5 and 95 never reached |
| Monotonicity | — | — | stat, duel, and foul tables strictly increase; kick-error table strictly decreases across each encoded input range |

## 6. Implementation notes (for whoever builds it)

- **Code space:**
  - CAVE1 (`FTTCollectionsTest`, 0x1fc6f0) uses 1,356 code + 198 data bytes of 1,676; it retains the control and market dispatch paths.
  - CAVE2 (`DEBUGCHARACTER_RenderPlayerData`, 0x381368) uses 516 code + 596 data bytes of 1,112; the stat/error/duel/foul routines and aligned tables fill the reservation.
  - Tables live in CAVE2's rodata tail, which begins at offset 516 (4-byte aligned) and has even length.
- **Follow `mod/PATCHING_RULES.md`:**
  - only PC-relative `b.w`/`bl` out of a cave;
  - no far conditional branches (short conditional plus `b.w`);
  - data addresses via `adr` plus a link-time delta;
  - Keystone statement-count and branch guards must pass;
  - no heredoc patches for files containing backslashes.
- **Registers:** `cave_stat` is entered with the stat in r0 and must tail-call `XMATH_InterpolateClamp` with r1–r3 intact.
  `cave_duel` and `cave_sduel` must keep the exact register and stack contract of today's routines (see their current
  asm in `build_mod.py`).
- **Integer maths:** use table lookup plus linear interpolation in fixed point, with no division in hot paths
  (`cave_stat` runs for every stat read, many times per frame).
- **Config:** `--stat-knee` defaults to 7, `--duel-scale` to 7, `--slide-shift` to 3, and `--foul-softness` to 0.1; shot/pass ranges and facing flags remain configurable.
- **Save:** unaffected; this is library-only.

## 7. Validation status (v27; gameplay library unchanged from v18)

1. `analysis/stat_curves.py`: `ALL PASS`, including strictly monotone stat, shot-error, duel, and foul tables, measured
   speed/shot/tackle targets, and CAVE2 size/alignment guards.
2. `mod/test_patch.py ...libDLS18_market_v18.so`: `ALL PASS` at a nonzero base. It checks all raw stats 0–99, both
   duel exits, and strictly increasing close-control urgency for all 100 raw control values.
3. `mod/build_mod.py --out ...libDLS18_market_v18.so`: all CAVE/CAVE2/CAVE3 guards pass. `test_hooks.py ...v18.so`:
   `ALL OK`, with 13/13 pagination, 5/5 selected-detail, 7/7 filter-toolbar, 4/4 focused-action, 5/5 primary-scheme, 6/6 contextual-back, 5/5 selected-outline, and 6/6 sorting checks, plus all seven list collectors wired.
4. `wsl -e sh mod/career_market/tests/run_tests.sh 12 1`: page-boundary, action-layout, and Market-toolbar checks pass;
   `api_digest=ba4f50439bcba846`, `violations=0`.
5. `arm_crosscheck.py` on `small.txt` for 2 seasons and the matching host command
   (`harness small.txt 2 1 --restart-every -1 --no-user --quiet`) both produced `api_digest=b0d31a38190d3683`;
   the host reported `violations=0`.
6. `DLS18_career_market_v27.apk` packaged successfully (61,797,454 bytes; 1,014 entries + v1 signature; SHA-256
   `D0ED942881E65A3FD001D6CADCF17BFE22CCE08D65FAFF3EB9B7BF85E5D1DEAF`). It has not been installed, so runtime
   behavior remains unconfirmed.

## 8. Dynamic-difficulty values

The career price schedule sets variables `0x13`, `0x14`, and `0x15` (BuyPlayer, StadiumPurchase, and PlayerDev adjust)
to zero. The v27 host harness checks the values after applying the schedule and after save/reload.
