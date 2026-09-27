# DLS18 animation rework report

## Current build and device state

Version 9 is built locally at `mod/build/DLS18_skill_tiered_dribble_v9.apk`. It carries low/mid/high locomotion bias, cyclic rotation and upper-body position cleanup for 23 dribble loops, endpoint-preserving cleanup for eight one-shot moves, and tier-aligned special-move variant selection. The original APK, OBB, and extracted source SAT files are unchanged.

You reported that the earlier offline match check worked. Version 9 itself has not been installed or run on the tablet. No further device checks were made at your request.

## Skill-tier selection

The native matcher at 0x2e0f74 scores locomotion candidates by movement-direction fit, with lower scores preferred. The patch assigns every candidate ID in the forward-jog, forward-sprint, and angled-jog dribble ranges to a control tier:

| Control rating | Tier | Forward jog candidates | Forward sprint candidates | 45-degree jog candidates |
|---:|---|---|---|---|
| Below 60 | Low | 2245–2248 | 2257–2258 | 2265–2284 |
| 60–84 | Mid | 2249–2252 | 2261–2264 | 2285–2288 |
| 85–99 | High | 2253–2256 | 2259–2260 | 2289–2290 |

The v9 default style bias is 0xC000. Candidates in the preferred tier receive a 0x18000 score reduction; candidates in the other two tier pools receive a 0x6000 penalty. On this score scale, that is about 16.9° of preference and 4.2° of penalty. Thresholds and bias remain configurable with --dribble-skill-low, --dribble-skill-high, and --dribble-style-bias.

The movement-direction score remains active. A better directional fit can still win, and a preferred candidate must be present in the current state list. The patch biases the locomotion matcher across the full candidate families without forcing a clip when it is unavailable or a poor direction fit.

## Special-move animation variety

Version 8 aligns the `GC_SpecialMoveDribbling` variant picker with those control cutoffs. That native path requests one of three style filters for the deek state; `GA_SetAnimFromDeek` then considers compatible clips from the existing special-move candidate list. Below the low cutoff it keeps the narrow style-2 pool. Between the low and high cutoffs it randomly uses style 0 or 2. At or above the high cutoff it can use styles 0, 1, or 2. The native patch changes only the two threshold immediates at `0x2e416e` and `0x2e4156`, so both cutoffs follow `--dribble-skill-low/high`. Candidate availability, move direction, and the engine's compatibility checks still determine the final take.

## Wrap-aware rotation cleanup

The source comparison found that v4 preserved the first and last samples even though these locomotion clips are treated as repeated takes. In selected arm, torso, thigh, and calf tracks, mean wrap-edge kink was higher than the mean interior kink. Version 5 applies the same one-pass quaternion midpoint filter to every sample, using the preceding/following samples modulo clip length. This includes first/last samples and smooths the cycle boundary.

The rotation filter uses tier- and family-specific strength, angular deadbands, and per-sample correction caps. It preserves clip duration and frame count, all translations, explicit-position samples, root/pelvis rotations, and hand/foot/toe rotations. The separate v7 position pass described below adjusts only selected upper-body explicit-position samples; the pelvis, leg, hand, foot, and toe positions remain unchanged. Selected first/last loop rotations and upper-body positions can change to smooth the seam. No new frames are added.

There are 23 decoded non-female base takes: six forward jogs, four forward sprints, and thirteen angled jogs. The suffix variants remain in the native candidate ranges but are not rewritten by the SAT editor. Across the edited base clips, 1,983 rotation samples changed in 394 clip-track instances. The per-clip metric is the unweighted mean cyclic kink across edited tracks; it fell by 1.9–11.5% per clip, averaging 7.5%. Across 1,150 first/last-frame endpoint measurements, mean kink fell from 13.84° to 11.94° (13.7%).

## One-shot skill-move cleanup

The v7 animation package adds a separate open-curve pass for the eight decoded non-female DEEK candidates used by the special-move state. These are not loops, so the filter uses only adjacent interior samples; the first and last poses stay byte-for-byte unchanged. It applies a lighter, shared correction cap (3.5° for arms, 2.8° for torso, and 2° for legs) to selected rotations. The new position pass also uses interior samples only and preserves both endpoint positions.

Across the eight one-shot clips, 648 rotational samples changed in 121 clip-track instances. Track-weighted mean interior kink across edited tracks fell from 7.003° to 6.809° (2.76%). The per-clip reduction averages 2.56%, ranging from 1.2% to 3.9%; the restrained pass keeps the authored flick and spin accents. The per-clip results and preserved-data invariants are in `mod/build/dribble_animation_metrics_tiered_v7.json`.

| ID | Move | Frames | Samples / tracks changed | Mean interior kink before → after | Reduction | Max sample change |
|---:|---|---:|---:|---:|---:|---:|
| 67 | Marseille turn | 26 | 114 / 16 | 7.078° → 6.921° | 2.2% | 2.12° |
| 69 | Stand-to-45° jog left-foot stepover | 16 | 33 / 13 | 5.278° → 5.216° | 1.2% | 1.47° |
| 71 | Forward-jog left-foot stepover | 11 | 48 / 14 | 8.445° → 8.210° | 2.8% | 2.29° |
| 73 | Jog heel flick | 21 | 126 / 16 | 8.073° → 7.755° | 3.9% | 2.66° |
| 624 | Special stepover | 15 | 73 / 16 | 7.594° → 7.298° | 3.9% | 3.50° |
| 2329 | Forward skill move, Noel | 20 | 59 / 14 | 6.155° → 6.040° | 1.9% | 2.30° |
| 2331 | Forward overhead flick, Colin | 27 | 147 / 18 | 7.349° → 7.107° | 3.3% | 3.50° |
| 2335 | Forward spin, Colin | 24 | 48 / 14 | 5.582° → 5.508° | 1.3% | 1.52° |

| Tier | Family | ID | Take | Frames | Samples / tracks changed | Mean cyclic kink before → after | Reduction | Max sample change |
|---|---|---:|---|---:|---:|---:|---:|---:|
| Low | Forward jog | 2245 | FWD 1 · NOEL | 10 | 97 / 17 | 10.273° → 9.225° | 10.2% | 4.50° |
| Low | Forward jog | 2247 | FWD 2 · COLIN | 11 | 93 / 16 | 10.429° → 9.546° | 8.5% | 4.50° |
| Mid | Forward jog | 2249 | FWD 3 · COLIN | 12 | 98 / 16 | 10.341° → 9.156° | 11.5% | 6.00° |
| Mid | Forward jog | 2251 | FWD 4 · NOEL | 12 | 88 / 18 | 7.713° → 6.926° | 10.2% | 6.00° |
| High | Forward jog | 2253 | FWD 5 · COLIN | 12 | 62 / 14 | 10.575° → 10.035° | 5.1% | 3.50° |
| High | Forward jog | 2255 | FWD 5 · COLIN D | 13 | 53 / 15 | 7.597° → 7.424° | 2.3% | 2.14° |
| Low | Forward sprint | 2257 | FWD 4 · COLIN | 11 | 121 / 16 | 16.096° → 14.724° | 8.5% | 4.00° |
| High | Forward sprint | 2259 | FWD 4 · COLIN D | 11 | 106 / 17 | 15.575° → 14.806° | 4.9% | 3.00° |
| Mid | Forward sprint | 2261 | FWD 2 · NOEL | 11 | 115 / 19 | 10.671° → 9.533° | 10.7% | 5.50° |
| Mid | Forward sprint | 2263 | FWD 3 · COLIN | 11 | 100 / 17 | 9.769° → 8.644° | 11.5% | 5.50° |
| Low | Angled jog | 2265 | 45-2 · COLIN | 13 | 75 / 18 | 7.233° → 6.689° | 7.5% | 4.50° |
| Low | Angled jog | 2267 | 45-3 · COLIN | 11 | 79 / 18 | 7.739° → 7.072° | 8.6% | 4.50° |
| Low | Angled jog | 2269 | 45-4 · COLIN | 11 | 88 / 16 | 9.785° → 8.853° | 9.5% | 4.50° |
| Low | Angled jog | 2271 | 45-5 · COLIN | 17 | 78 / 17 | 6.089° → 5.737° | 5.8% | 4.50° |
| Low | Angled jog | 2273 | 45-6 · NOEL | 16 | 70 / 17 | 6.526° → 6.040° | 7.4% | 4.50° |
| Low | Angled jog | 2275 | 45-6 D · NOEL | 14 | 42 / 14 | 4.700° → 4.539° | 3.4% | 2.07° |
| Low | Angled jog | 2277 | 45-7 · NOEL | 12 | 43 / 15 | 6.596° → 6.157° | 6.7% | 4.50° |
| Low | Angled jog | 2279 | 45-7 D · NOEL | 15 | 60 / 18 | 5.644° → 5.452° | 3.4% | 2.50° |
| Low | Angled jog | 2281 | 45 · COLIN | 12 | 48 / 19 | 5.518° → 5.121° | 7.2% | 4.50° |
| Low | Angled jog | 2283 | 45-10 · NOEL | 12 | 104 / 19 | 8.711° → 8.184° | 6.0% | 3.32° |
| Mid | Angled jog | 2285 | 45-11 CTRL · NOEL | 19 | 167 / 22 | 7.042° → 6.236° | 11.4% | 7.00° |
| Mid | Angled jog | 2287 | 45-8 CTRL · COLIN | 16 | 125 / 18 | 7.808° → 6.968° | 10.8% | 6.50° |
| High | Angled jog | 2289 | 45-9 DRIB · COLIN | 16 | 71 / 18 | 6.945° → 6.811° | 1.9% | 2.12° |

The high-tier rotation profile remains lighter so its sharp accents survive more of the cleanup. Rotation kink measures continuity in decoded samples; it does not prove subjective animation quality or the exact in-game clip selected.

## Tier-specific upper-body position cleanup

The v7 pass also edits the SAT explicit-position stream for selected head, neck, clavicle, upper-arm, and forearm tracks. Native lookup uses each bone's map value as the packed position-track index, so the editor follows that mapping instead of assuming that bone order and payload order match. It does not edit the pelvis, leg, hand, foot, or toe position tracks, any transform translation, or hand/foot/toe rotations.

For each sample, the editor finds the midpoint of the preceding and following XYZ samples. It moves the current point toward that midpoint only when its distance exceeds the deadband. Each pass uses the original decoded samples, not the previously filtered result. Loop clips wrap around the seam; one-shot clips process interior frames and preserve both endpoint positions. The values below are `(strength, deadband, maximum correction)` in raw SAT position units:

| Tier | Arm tracks | Torso tracks |
|---|---|---|
| Low | (0.12, 90, 45) | (0.10, 100, 35) |
| Mid | (0.16, 75, 55) | (0.12, 85, 42) |
| High | (0.07, 110, 30) | (0.06, 120, 22) |
| One-shot moves | (0.08, 100, 32) | (0.07, 110, 26) |

Across 23 locomotion loops, 1,070 position samples changed in 184 edited tracks. The mean per-track local kink fell from 139.046 to 130.844 raw units (5.90%); per-clip reductions range from 1.9% to 9.7%. Across eight one-shot moves, 400 samples changed in 59 tracks. Their mean per-track kink fell from 106.691 to 105.213 raw units (1.39%); per-clip reductions range from 0.5% to 1.8%. The largest recorded single-sample displacement is 55.5 raw units on a loop and 32.0 on a one-shot move. These raw-unit metrics describe sample continuity only; they do not establish visual improvement in a match.

### Locomotion position results

“Mean kink” is the average XYZ distance to the adjacent-sample midpoint over each edited track, then averaged by clip. “Max shift” is the largest Euclidean change to one XYZ sample. The detailed metrics JSON also lists each affected bone and track.

| Tier | Clip ID | Position samples / tracks changed | Mean kink before → after (raw) | Reduction | Max shift (raw) |
|---|---:|---:|---:|---:|---:|
| Low | 2245 | 31 / 8 | 142.6 → 132.1 | 7.3% | 45.0 |
| Low | 2247 | 42 / 8 | 141.6 → 131.8 | 7.0% | 45.5 |
| Mid | 2249 | 59 / 8 | 129.1 → 120.1 | 6.9% | 55.5 |
| Mid | 2251 | 65 / 8 | 129.0 → 119.5 | 7.3% | 55.5 |
| High | 2253 | 40 / 8 | 117.6 → 113.9 | 3.1% | 30.0 |
| High | 2255 | 41 / 8 | 111.8 → 109.7 | 1.9% | 18.4 |
| Low | 2257 | 53 / 8 | 162.3 → 152.0 | 6.3% | 45.4 |
| High | 2259 | 44 / 8 | 162.3 → 157.4 | 3.0% | 30.3 |
| Mid | 2261 | 56 / 8 | 158.3 → 142.9 | 9.7% | 55.0 |
| Mid | 2263 | 53 / 8 | 128.3 → 119.5 | 6.9% | 50.5 |
| Low | 2265 | 54 / 8 | 152.2 → 143.5 | 5.7% | 45.5 |
| Low | 2267 | 47 / 8 | 158.8 → 147.0 | 7.4% | 45.2 |
| Low | 2269 | 47 / 8 | 149.0 → 140.0 | 6.0% | 45.4 |
| Low | 2271 | 46 / 8 | 146.9 → 139.7 | 4.9% | 45.5 |
| Low | 2273 | 38 / 8 | 135.1 → 126.8 | 6.2% | 45.2 |
| Low | 2275 | 26 / 8 | 87.2 → 84.7 | 2.9% | 45.5 |
| Low | 2277 | 27 / 8 | 148.7 → 139.3 | 6.3% | 45.6 |
| Low | 2279 | 36 / 8 | 110.0 → 103.6 | 5.8% | 45.2 |
| Low | 2281 | 29 / 8 | 163.8 → 154.3 | 5.8% | 45.3 |
| Low | 2283 | 56 / 8 | 206.4 → 195.6 | 5.2% | 45.4 |
| Mid | 2285 | 79 / 8 | 126.7 → 115.0 | 9.2% | 55.5 |
| Mid | 2287 | 63 / 8 | 98.4 → 92.9 | 5.5% | 40.8 |
| High | 2289 | 38 / 8 | 132.2 → 127.8 | 3.3% | 30.2 |

### One-shot position results

| Clip ID | Position samples / tracks changed | Mean interior kink before → after (raw) | Reduction | Max shift (raw) |
|---:|---:|---:|---:|---:|
| 67 | 98 / 8 | 119.4 → 117.5 | 1.6% | 17.8 |
| 69 | 20 / 7 | 88.6 → 88.1 | 0.6% | 13.0 |
| 71 | 52 / 8 | 143.5 → 141.0 | 1.8% | 15.6 |
| 73 | 64 / 8 | 102.7 → 101.0 | 1.7% | 17.2 |
| 624 | 28 / 8 | 100.7 → 99.1 | 1.5% | 32.0 |
| 2329 | 25 / 4 | 101.4 → 100.4 | 1.0% | 14.0 |
| 2331 | 77 / 8 | 109.3 → 107.5 | 1.6% | 31.6 |
| 2335 | 36 / 8 | 83.0 → 82.6 | 0.5% | 9.5 |

## Preview files and remaining visual limit

The cyclic animation edits are previewed in previews/animations/skill_tiers_v5/:

- forward_jog_all_tiers.png and forward_sprint_all_tiers.png compare the available takes in tier order.
- angled_regular_family.png covers the regular angled family; angled_skill_tiers.png compares the two CONTROL takes and the DRIB take.
- dribble_2245.gif through dribble_2289.gif include one GIF for each of the 23 edited clips.
- previews/animations/skill_tiers_v6/forward_jog_tiers.gif, forward_sprint_tiers.gif, and angled_jog_tiers.gif show synchronized low-, mid-, and high-tier representative clips side by side. These previews cover earlier rotation-only edits and do not show the v9 upper-body position pass.

The renderer reads the edited SAT directory directly through --sat-dir. These are named 42-bone stick-rig previews. The full skinned-player preview still has an unresolved coordinate mismatch in bind/deformation matrices and is not a reliable check of final player-mesh appearance.

## Build artifacts

- Native library: `mod/build/lib/armeabi-v7a/libDLS18_skill_tiers_v9.so`. The selector and transition code use 1,244 of the 1,676 bytes in the existing function cave. SHA-256 prefix: `ddd50f45ebcd3378`. The special-move tier change uses two in-place 16-bit comparisons.
- Animation package: `mod/build/anims_skill_tiers_v7.pak`, 2,536 members; exactly 31 SAT members differ from source. Across those members, 2,631 rotation samples and 1,470 explicit-position samples changed. SHA-256: `8ddec51817215502f726fe4ec9453475e5c4e164156774b186e8fbe7993f6f1c`.
- Metrics: `mod/build/dribble_animation_metrics_tiered_v7.json` records all 31 clips, affected bones and packed tracks, per-track and per-sample edits, source/output hashes, and preserved-data invariants.
- APK: `mod/build/DLS18_skill_tiered_dribble_v9.apk`, 61,686,557 bytes, 1,013 entries plus its v1 signature.

The v9 local build completed, and `mod/install.ps1` points to v9. The script was not run. The v9 animation changes have not been checked in a match.

## Rebuild commands

Run these from the project root:

    python mod/build_mod.py --dribble-skill-low 60 --dribble-skill-high 85 --dribble-style-bias 0xC000 --out mod/build/lib/armeabi-v7a/libDLS18_skill_tiers_v9.so
    python analysis/rework_dribble_animations.py --sat-dir mod/source/animations_skill_tiers_v7 --pak-out mod/build/anims_skill_tiers_v7.pak --metrics-out mod/build/dribble_animation_metrics_tiered_v7.json
    python mod/build_apk.py --lib mod/build/lib/armeabi-v7a/libDLS18_skill_tiers_v9.so --anims-pak mod/build/anims_skill_tiers_v7.pak --out mod/build/DLS18_skill_tiered_dribble_v9.apk
    powershell -File mod/install.ps1

The install command is included for a future update; it was not run. Build requirements are Python with capstone and keystone-engine, WSL with openssl, and Android support for 32-bit ARM apps.

## Related files

- mod/build_mod.py — tier routing, transitions, and gameplay patches.
- analysis/rework_dribble_animations.py — per-clip cyclic quaternion smoothing, package rebuilding, and metrics.
- analysis/preview_sat_skeleton.py — rig reconstruction, preview rendering, and SAT directory selection.
- mod/build_apk.py — APK assembly.
- mod/README.md — gameplay changes, tier mapping, and build instructions.
- ANIMATION_LAYOUT.md and MODEL_LAYOUT.md — format notes and remaining skinned-model work.
