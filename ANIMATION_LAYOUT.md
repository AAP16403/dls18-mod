# Animation and sequence layout

The extracted animation package contains 2,535 `.sat` clips. `catalog/animation_clips.csv` records their original paths, sizes, hashes, and initial bytes. `animdb.adb` is 565,966 bytes; its 4-byte word catalog remains in `catalog/animdb_words.csv`. The 199 NIS XML sequences remain editable under `unpacked/kpx/nis/`.

## Decoded SAT data

`analysis/decode_assets.py animations` writes a compact index to `decoded_assets/animations/index.csv` and one compressed JSON file per clip (`decoded_assets/animations/0000.json.gz`, etc.). The source `.sat` files are left untouched.

The native `SAT_LoadAnimation` loader reads a 0x68-byte (104-byte) header. The confirmed fields are:

| Offset | Meaning |
| --- | --- |
| `0x00` | 32-bit header value; meaning unresolved |
| `0x02` | signed 16-bit sample interval in 30 Hz ticks |
| `0x04` | signed frame count |
| `0x05` | format flag; nonzero clips do not load inline sample arrays |
| `0x06` | signed count of transform-map slots |
| `0x07` | signed count of packed transform tracks |
| `0x08` | transform activity/map bytes (up to 42 slots) |
| `0x32` | signed count of explicit-position tracks |
| `0x33` | 45 signed explicit-position map entries |
| `0x60`, `0x64` | two 32-bit fields used as references in header-only variants |

For flag-zero clips, the inline payload follows the header:

- Transform samples use 14 bytes per track per frame: four signed 16-bit quaternion values followed by three signed 16-bit position values. The native sampler scales quaternion values by `1/16384` and position values by `1/128`.
- Explicit positions use three signed 16-bit values per position track per frame. Their engine-side interpolation returns these values in raw units.
- Transform records are stored track-major, then frame. Explicit-position records are frame-major, then track, matching the native lookup `frame × position_tracks + mapped_track`. The expected file size is `104 + 14 × frames × transform_tracks + 6 × frames × position_tracks`.
- Sample times advance by `interval_ticks / 30` seconds. The 42-entry `bone_remap` table from `libDLS18.so` maps active transform slots to model-bone slots.

The decoder resolved inline samples for 1,402 clips. The other 1,133 are 104-byte header-only variants; their reference fields are recorded, but their shared-data meaning is not yet established. Raw signed values are preserved in the JSON so they can be inspected and adjusted without losing precision.

## Named animation IDs

`analysis/catalog_animation_names.py` extracts the native library's 2,535-entry `ANIM_sName` pointer table and writes `catalog/animation_names.csv`. The table size matches the 2,535 `.sat` files, and each row's numeric animation ID maps to the `.sat` file with the same four-digit ID. Names ending in `_f` are marked as a suffix variant; the suffix's exact meaning is not asserted here.

This crosswalk makes the dribble families identifiable. Examples include the generic jog and sprint loops (`ANM_CONTROL_EXE153_SPRINTING_DRIBBLE` at ID 53), skill moves (`ANM_DEEK_EXE165_MARSEILLE_TURN` at ID 67), and later jog/sprint variants (IDs 2245–2290). Most of these entries have 33 transform tracks and 19 explicit-position tracks. Several `_f` entries have header-only payloads, so any edit workflow must preserve or correctly resolve those references instead of treating them as empty clips.

The animation database has 2,535 records of 100 bytes after its count header. Native `AnimDataFill` copies each record's first byte into an animation category, and `StateInfoListFill` groups records by that category into 20 candidate lists. The dribble family IDs 2245–2290 are all in loader group 1, which contains 351 animation records. `analysis/catalog_animdb_groups.py` exports all 20 lists to `catalog/animdb_candidate_groups.csv`; this confirms the controlled-style candidate IDs exist in the engine's locomotion candidate data.

The native `GC_SpecialMoveDribbling` path checks the player's control attribute at `CPlayer+0x127`. Version 9 aligns its cutoffs with the locomotion tiers: low control keeps style 2, mid control can use styles 0 or 2, and high control can use styles 0, 1, or 2. The v9 locomotion selector biases every candidate across three dribble families by control tier: low favors forward-jog IDs 2245–2248, forward-sprint IDs 2257–2258, and regular angled-jog IDs 2265–2284; mid favors 2249–2252, 2261–2264, and `CONTROL` IDs 2285–2288; high favors 2253–2256, 2259–2260, and the `DRIB` IDs 2289–2290. It biases the native direction-fit score rather than bypassing it, so movement fit can still select another candidate and a preferred clip must be present in the active state list. See `mod/ANIMATION_REWORK_REPORT.md` for thresholds, score weights, and limits.

## Player rig preview

The player FTM hierarchy and named rig are now decoded for the stick-skeleton previews in `previews/animations/`. In `body_0_1.ftm`, chunk `0x25` contains 42 first-child/next-sibling links and a root index; chunk `0x1e` gives the bone names; chunk `0x1c` gives each bone's static local scale, quaternion, and translation. The native SAT hierarchy resolver confirms the link traversal and multiplies each child's local matrix by its parent matrix. SAT clip tracks replace the corresponding local transforms after applying the native 42-entry bone-remap table.

`analysis/preview_sat_skeleton.py` can draw named poses or render a front/profile GIF for every keyframe. Its `--sat-dir` option points the renderer at edited files without replacing extracted originals. The v5 previews under `previews/animations/skill_tiers_v5/` cover forward jog, forward sprint, the regular angled family, and the `CONTROL` / `DRIB` angled takes; a GIF is included for each of the 23 edited base clips.

## Reworking the controlled dribble samples

`analysis/rework_dribble_animations.py` applies one-pass cyclic quaternion midpoint cleanup to 23 decoded non-female base takes in the forward-jog, forward-sprint, and angled-jog dribble families. It also applies a lighter open-curve pass to eight decoded non-female DEEK clips (IDs 67, 69, 71, 73, 624, 2329, 2331, and 2335). Loop edits wrap around the seam; one-shot edits touch interior samples only and preserve both endpoint poses. Rotation cleanup covers selected arm, torso, thigh, calf, and twist tracks. Position cleanup covers selected head, neck, clavicle, upper-arm, and forearm tracks, with control-tier-specific strength and caps. Transform translations and pelvis, leg, hand, foot, and toe position tracks remain byte-for-byte unchanged; hand, foot, and toe rotations also remain unchanged. Suffix variants are not rewritten.

The v9 edited copies are in `mod/source/animations_skill_tiers_v7/`; the rebuilt package is `mod/build/anims_skill_tiers_v7.pak`. The repacker preserves member order, names, metadata, and every unchanged decoded payload; exactly 31 SAT members differ from source. `mod/build/dribble_animation_metrics_tiered_v7.json` records hashes, affected bones, rotation and position kink measurements, per-sample changes, and preserved-data invariants. The 23 loops changed 1,983 rotational samples across 394 clip-track instances and 1,070 explicit-position samples across 184 tracks. Their position kink fell 5.90% across edited tracks on average. The eight one-shot skill moves changed 648 rotation samples across 121 clip-track instances and 400 explicit-position samples across 59 tracks; their position kink fell 1.39% across edited tracks. The one-shot rotation endpoints and all one-shot position endpoints are unchanged. Timing, transform translations, pelvis/leg/hand/foot/toe positions, and hand/foot/toe rotations remain unchanged. The explicit-position stream maps skeletal joints; the edit does not alter the hand or foot samples used for contact.

The player model investigation now decodes the four body geometry/weight sections, but the first fully skinned preview still has an unresolved coordinate mismatch in the bind/deformation matrices. Existing named line-skeleton previews cover the earlier rotation-only package, not the v9 position edits. The v9 build is local and has not been installed or match-tested; no new device checks were made at your request.

## Related files

The NIS catalog is in `catalog/nis_sequences.csv`. It summarizes XML structure and animation-reference text such as `AnimID`; the XML files remain the editable source for scene timing, camera actions, and player nodes. The native animation-name crosswalk is `catalog/animation_names.csv`.
