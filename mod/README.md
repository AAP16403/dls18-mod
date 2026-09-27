# DLS18 (v5.064) — DLS15-style analog sprint + hold-to-close-control

## Controls after the mod
| Input | Result |
|---|---|
| Push the stick a little or halfway | **Jog** (the game's normal running speed) |
| Push the stick to the edge (≥ 87.5 %) | **Sprint**. Sprinting continues until you ease back below 78 %. |
| Hold a finger anywhere on the right side, **not on a button**, while dribbling | **Close control**: slower pace and shorter touches, so the ball stays at your feet. No sprinting. The pace depends on the **control stat**: it starts at urgency 0x280 for control 0, rises by one urgency point per stat point through 40, and then increases smoothly to 0x500 at 99. Touch size follows the pace. |
| Same hold without the ball | Slow, controlled movement (for defensive positioning) |
| Swipe with the holding finger | Skill move, same as before |
| Tap A, B or C (even while holding) | Pass, shoot and through-ball work as normal. The hold ends when that finger lifts. |

**Joystick zones** (measured in `XCTRL_TouchProcessHWExtra`; dead zone = stick radius ÷ 12): 0–8.3 % no movement · 8.3–87.5 % jog · ≥87.5 % sprint, ending when you ease back below 78 %. The stick floats (its centre is where your thumb lands), so sprinting means dragging about 88 % of the ring's radius. Tune with `--sprint-on/--sprint-off` (0x4000 = 100 %).

Sprint still uses stamina. You can't *start* a sprint below 25 % stamina (the game's own rule), but a sprint already running continues until stamina hits zero. Outside open play (set pieces and so on) the game's normal behaviour applies.

The mod uses and edits the game's existing animation clips. It adapts transition timing to player ratings and biases low, mid, and high control players across the full forward-jog, forward-sprint, and angled-jog dribble families. The v9 APK combines that tier routing with wrap-aware cleanup for 23 dribble loops, endpoint-preserving cleanup for eight one-shot skill moves, restrained upper-body position smoothing, and matching control thresholds in the special-move picker. The source OBB and source SAT files remain untouched; the animation package changes only those 31 members.

### Dribble animation selection

The locomotion matcher continues to score clips by movement direction. It adds a control-tier preference using the player's control rating (`CPlayer+0x127`):

| Control rating | Preferred style | Candidate score adjustment |
|---|---|---|
| Below 60 | Forward jog 2245–2248, forward sprint 2257–2258, regular angled jog 2265–2284 | Preferred low-tier IDs: `−2 × bias`; mid/high tier IDs: `+½ × bias` |
| 60–84 | Forward jog 2249–2252, forward sprint 2261–2264, controlled angled jog 2285–2288 | Preferred mid-tier IDs: `−2 × bias`; low/high tier IDs: `+½ × bias` |
| 85–99 | Forward jog 2253–2256, forward sprint 2259–2260, angled DRIB take 2289–2290 | Preferred high-tier IDs: `−2 × bias`; low/mid tier IDs: `+½ × bias` |

The v9 default `--dribble-style-bias` is `0xC000`: the preferred tier gets a `−0x18000` score adjustment, and the other tiers get `+0x6000`. On this score scale, that is about 16.9° of preference and 4.2° of penalty. Direction fit can still choose another clip if it is a better match or the preferred take is absent from the active state list, so tiers remain weighted preferences. The special-move picker also uses these low/high thresholds: low control gets its existing narrow style pool, mid control can use two styles, and high control can use all three. Tune with `--dribble-skill-low`, `--dribble-skill-high`, and `--dribble-style-bias`.

**Correction (2026-09-27, anim_fit):** the IDs 2245–2290 are category-1 *control/touch* clips. The locomotion matcher at A2 only scores the state 0/4 lists (every caller passes state 0 or 4, or a filter value no category-1 record has), so the A2 tiers above never reached these clips in a match. They are chosen by `CPlayer::SetAnimControl` (from `ControlTakeBall`/`DribbleBall`), which scores each clip's action point against the projected ball and adds `random(1024)`.

### Ability-fit selection (`anim_fit.py`, CAVE3 +0x20000)

| Category / picker | Clips | Rule (c = control, t = tackling, s = strength) |
|---|---|---|
| 1, `SetAnimControl` | close control 2553–2558 | score + 0x28·(75 − c): c 20 → +2200, 60 → +600, 75 → 0, 85 → −400, 99 → −960 |
| 1, `SetAnimControl` | aerial control 2559/2560 | score + 0x40·(85 − c): c 20 → +4160, 60 → +1600, 85 → 0, 99 → −896 (never excluded) |
| 1, `SetAnimControl` | stock pools 2245–2290 (A2 pools) | low + 0x28·(c − 60), mid + 0x28·(\|c − 72\| − 12), high + 0x28·(85 − c); piecewise linear, pools cross at 60 and 85 (`--no-ctrl-fit-stock` to drop) |
| 19, `GA_SetAnimFromDeek` | 2547–2550 / 2551–2552 | kept when random(64) < ramp: 1/64 at c ≤ 26 rising to 1 at 99 / 1/64 at c ≤ 6 rising to 1 at 80 (c 40: 19 % / 45 %, 60: 47 % / 72 %, 85: 80 % / 100 %) |
| 19, `GC_SpecialMoveDribbling` | deek style 0/1/2 | random(100): style 1 with 1 → 34 % (ramp from c 25 to 99), style 0 with 2 → 33 % (c 10 to 99), style 2 the rest (c 20: 1/3/96 %, 60: 16/18/66 %, 99: 34/33/33 %); replaces the stock steps at 75/85 (`--no-deek-style-fit` keeps them, at `--dribble-skill-low/high`) |
| 9, `ACT_TackleSetPlayerState` | lunge 2535/2536 | allowed when random(64) < ramp(t; 30 → 95), floor 1/64 (t 40: 14 %, 60: 45 %, 85: 84 %), else +0x40000 (used only when no stock clip fits) |
| 6, `SetAnimFromStateGen` | 2537/2538 recover; 2539–2546 dramatic | ±0.8·(s − 60), clamped to the ±32 random tie-break only at s ≤ 20 (dramatic port 100 % at 20, 88 % at 40, 48 % at 60, 6 % at 85) |

Every rule is continuous in its stat (no step between adjacent ratings, no clip excluded at any rating); `test_anim_fit.py` emulates each stub at every stat value 0..99 and checks this.

The lunge's contact point (action point 0: 147 cm at 22°, 4 game ticks) sits inside the stock standing-tackle envelope (334: 142 cm at 26°, 739: 174 cm), and both the selector's reach test and the contact check use that point, so it adds no range; its extra travel is follow-through after contact. Flags: `--ctrl-fit-tier-step`, `--ctrl-fit-close-mid/step`, `--ctrl-fit-aerial-mid/step`, `--no-ctrl-fit-stock`, `--deek-fit-high-lo/hi`, `--deek-fit-mid-lo/hi`, `--deek-style1-lo`, `--deek-style0-lo`, `--no-deek-style-fit`, `--lunge-tackling-lo/hi`, `--stumble-strength-mid/span`, `--no-anim-fit`. The build refuses an `--anim-count` that adds a clip without a rule in `anim_fit.RULES`; `test_anim_fit.py` (run by `test_patch.py`) emulates all five hooks and checks the rules against the animation package.

Transition crossfade speed also varies by the average of acceleration and control: about 14 ticks at 40 and 9 at 99 (stock is 8), continuing linearly below 40 (17 ticks at 20) instead of clamping. The current local APK is `build/DLS18_skill_tiered_dribble_v9.apk`. See [`ANIMATION_REWORK_REPORT.md`](ANIMATION_REWORK_REPORT.md) for clip inventories, sample edits, build hashes, device status, and limits.

## Player quality: stats, shooting, tackling

| Change | Stock DLS18 | Stat rework v14 |
|---|---|---|
| **All interpolated stats** | `XMATH_InterpolateClamp` accepts an integer stat in the 40–99 window. | A Q8.8 softplus map rises continuously below 48, blends smoothly to identity from 48–56, and passes stats 56–99 unchanged. The minimum mathematical slope from 20–40 is 0.0543. Wider gameplay differences come from per-mechanic output ranges. |
| **Shot direction error** | About 2.8° for the weakest shooter down to 0.75°. | Raw shooting stat drives a convex power-2.6 curve: 12° at raw stat 25 to 0.8° at 99, with a smooth continuation below 25. Measured errors at stats 35 / 54 / 85 / 92 are 8.479° / 3.873° / 0.948° / 0.824°. |
| **Pass direction error** | About 2.8° to 0.75°. | Raw passing stat drives a 5° to 0.6° curve. Type-2 short passes keep their fixed accuracy. |
| **Standing tackle** | If the tackle animation reaches the ball, the ball is won regardless of stats. | At contact, `z = tackling − control + 0.5 × strength difference + 8 when facing within 45°`; chance is `5 + 90 / (1 + e^(−z/7))`%. At tackling-minus-control −20 / 0 / +20 (equal strength, no facing bonus), the chance is 9.888% / 50% / 90.112%. Both win and stumble exits are retained. |
| **Slide tackle** | Same. | Uses the same stat and strength difference, with `z − 3` and a 4–88% logistic chance. A miss uses up the ball contact; the game's own body-contact and foul logic handles the rest. |
| **Fouls** | Contact without winning the ball: 30–100%, depending on distance. | The distance-based chance is multiplied by a smooth Q12 curve derived from `(160 − tackling) / 90`, with open limits near 0.6–1.4 and no clamped stat tail. |
| **Body collisions** | A stronger player (even by 1 point) is not pushed. | Push is shared in proportion to strength². |
| **Stamina** | Sprint drain 0x500 → 0x300, recovery 0x300 → 0x500 over stamina 40–99, flat below 40. | Drain 0x600 → 0x280, recovery 0x280 → 0x600 over 40–99, the same lines continued down to stamina 1 (drain 0x850, recovery 0x30); GK reaction delay likewise runs 25 → 0 frames over 1–99 (14 at 45, as before). |

### Stat → gameplay retune

`analysis/stat_audit.py` lists all 33 `AttributeInterpolate_Internal` call sites. These output ranges carry the rating gaps while the input map stays near identity:

| What | Stat | Stock | Stat rework v14 |
|---|---|---|---|
| Sprint speed | speed | 3738 → 4539 | 2950 → 5000; ratio 1.215 at stats 62/85 and 1.106 at 68/80 |
| Jog / average run speed | speed | 3204 → 3738 | 2950 → 4600; ratio 1.180 at stats 62/85 |
| Walk speed | speed | 801 for everyone | 760 → 840 |
| Acceleration (urgency ramp per frame) | acceleration | 13 → 19 | 7 → 27; stats 62/85 produce 14/22 per-frame steps, or about 4.88/3.10 seconds to full sprint |
| Speed with the ball (÷1024) | control | 870 → 990 | 768 → 1042 |
| Close-control carry pace | control | No modded hold-to-close-control pace | 0x280 at control 0; rises by 1 urgency per point through 40, then smoothly reaches 0x500 at 99 |
| First touch / ball-take radius | control | 947 → 1178, 8544 → 13350 | 820 → 1320, 6500 → 15600 |
| Dribble touch strength, jog / sprint | control | 1869 → 1602 / 2670 → 2136 | 2075 → 1425 / 2950 → 1845 |
| Tackle reach and action scaling | tackling | 1638 → 2048, 512 → 1024 | 1400 → 2150, 400 → 1100 |
| Shot-placement chance (%) | shooting | 33 → 66 | 20 → 80 |
| Shot assist | shooting | −60 → 80 | −80 → 96 |
| Keeper reaction delay (frames) | GK reaction, read directly | 50..99 → 9..0 | 45..99 → 14..0 |

Tuning flags: `--stat-knee` (default 7), `--shot-error-worst-deg` / `--shot-error-best-deg`, `--pass-error-worst-deg` / `--pass-error-best-deg`, `--duel-scale` (default 7), `--slide-shift` (default 3), `--foul-softness` (default 0.1), `--head-on-bonus` (default 8), and `--head-on-angle` (`0x800` = 45° half-angle; full turn is `0x4000`). The old `--stat-pivot`, `--stat-contrast`, and linear tackle-probability flags have been removed.

The current stats library is `build/lib/armeabi-v7a/libDLS18_market_v18.so`; the packaged career build is `build/DLS18_career_market_v27.apk`. The gameplay library is byte-identical to v14; the v27 career bridge adds inline Squad club navigation, a streamlined Finances pane, and an inline Sales filter toolbar plus independent persistent Market/Sales position filters and efficient rating, value, and wage sorting to the paged dedicated market UI. The close-control pace check covers every raw control rating from 0–99. Rebuild and print the full curve tables/section 5 measurements with:

```
python analysis/stat_curves.py
python mod/build_mod.py --out mod/build/lib/armeabi-v7a/libDLS18_market_v18.so
python mod/test_patch.py mod/build/lib/armeabi-v7a/libDLS18_market_v18.so
```

The stats come from the database at load time (`CPlayer::SetupPlayer`): col4 → tackling (`+0x123`), col5 → control (`+0x127`), col6 → shooting (`+0x12a`), col7 → passing (`+0x128`).

**Animation rework.**
- *Transitions depend on agility.* Every change from one animation to the next (starting to run, turning, going into a tackle, kicking, recovering) used to crossfade over a fixed 8 ticks for every player. The crossfade (`CPlayer::Animate`, weight `+0x6e`) now takes about 14 ticks for low-rated players, 11 for average players, and 9 for high-rated players. Tune it with `--blend-step-worst/best`.
- *Dribble takes vary by control tier and movement family.* The selector applies rating-aware preferences across every candidate in the forward jog, forward sprint, and 45-degree jog families while retaining movement-direction scoring. It does not force a take when another candidate is a better directional fit.
- *Dribble rotations and upper-body motion are cleaned across the family and loop seam.* A cyclic quaternion midpoint filter cleans selected upper-body and leg rotations in all 23 decoded base SAT takes. A second, restrained tier-aware filter smooths selected head, neck, clavicle, upper-arm, and forearm explicit-position samples; low and mid tiers get stronger position cleanup while high-tier clips retain sharper accents. It preserves timing, transform translations, pelvis/leg/hand/foot/toe positions, and hand/foot/toe rotations. Loop edits can adjust first/last samples to smooth the seam; one-shot moves preserve both endpoint poses. The original SAT inputs stay unchanged; edited copies live in `mod/source/animations_skill_tiers_v7/`.
- *Run-cycle pace follows the new speeds.* `CPlayer::UpdateAnimation` / `Animate` advance the locomotion clip by *distance moved ÷ clip stride length* (`TAnimData+0x18`), and blend walk, jog and sprint clips by urgency. Use `--no-retune` to keep the stock ranges.

## How it works (libDLS18.so patch)
| Site | Function | Change |
|---|---|---|
| `0x2b17be` | `CTRL_ControllerGetInput` | The "hold one direction for ~1.5 s → auto sprint" logic is replaced. The sprint flag (`TController+0x54`) now depends on stick power (`+0x80`, range 0–0x4000), with an on/off gap so it doesn't flicker. |
| `0x2e4f44` | `GC_DribblingControl` | Urgency with the ball: sprint 0x1000, close control 0x380, otherwise jog 0x800 |
| `0x2e94ae` | `GC_MovementOffBall` | Urgency without the ball: sprint 0x1000, close control 0x500, otherwise the game's own logic |
| `0x2e4cf8` | `GPM_DribbleTouch` | For urgency below 0x600, touch strength × (2·urgency + 0x400)/0x1000 |
| `0x2daebc` | `CPlayer::AttributeInterpolate_Internal` | CAVE2 Q8.8 softplus map before `XMATH_InterpolateClamp`; identity at raw stat 56+ |
| `0x2e5b34` / `0x2e5c02` | `ACT_KickErrorAccuracyGetRange` | Kick type is remembered; raw-stat convex error tables handle shots and passes (type 2 stays fixed) |
| `0x2dc5be` | `CPlayer::Animate` | Animation crossfade decay per tick depends on agility, clamped at 0 |
| `0x2db860` | `UpdateActionConservativeTackle` | Logistic standing duel at contact with strength and facing terms; on a loss, the tackler trips and the function returns |
| `0x2db430` | `UpdateActionSlideTackleX` | Logistic slide duel at contact; on a loss, `+0x158` is set and the ball handling is skipped |
| `0x2dba18` | same | Smooth Q12 foul factor indexed by raw tackling stat |
| `0x2df2ac` | `COL_PlayerAllCollisionProcess` | Push weights = strength² |
| `0x2da22c`… | `CPlayer::UpdateSprint` | Wider stamina drain and recovery ranges |

An earlier build put the tackle duel in `GL_SetTouch`. That ran *after* the tackle had already knocked the ball away, which broke the tackle stagger. `GL_SetTouch` is now left at stock.

CAVE1 (`FTTCollectionsTest`, 0x1fc6f0) contains the control and market dispatch code. CAVE2 (`DEBUGCHARACTER_RenderPlayerData`, 0x381368) contains the stat, error, duel, and foul routines plus their aligned fixed-point tables (516 bytes of code + 596 bytes of data). Close control is detected with the game's own `XCTRL_GetGameTouchTouching` (the right-hand touch track) and `XCTRL_GetButtonDown`, checked against all 9 HUD buttons.

## Build / install

Run these from the project root to rebuild the current animation APK:

```
python mod/build_mod.py --dribble-skill-low 60 --dribble-skill-high 85 --dribble-style-bias 0xC000 --out mod/build/lib/armeabi-v7a/libDLS18_skill_tiers_v9.so
python analysis/rework_dribble_animations.py --sat-dir mod/source/animations_skill_tiers_v7 --pak-out mod/build/anims_skill_tiers_v7.pak --metrics-out mod/build/dribble_animation_metrics_tiered_v7.json
python mod/build_apk.py --lib mod/build/lib/armeabi-v7a/libDLS18_skill_tiers_v9.so --anims-pak mod/build/anims_skill_tiers_v7.pak --out mod/build/DLS18_skill_tiered_dribble_v9.apk
powershell -File mod/install.ps1
```
Requirements: Python with capstone and keystone-engine; WSL with openssl. The device must run 32-bit ARM apps.

## Caveats
- `install.ps1` uses `adb install -r` so a matching-signature update preserves app data; it copies the OBB only if its hash differs. If Android rejects an update because an installed store build has a different signing key, the script stops without uninstalling the app or deleting its save.
- Online multiplayer: both players would need the mod, or the matches will desync. Don't use it online.
- The tuning values are starting points. Rebuild with different `--cc-urg` / `--sprint-on` values to taste.
