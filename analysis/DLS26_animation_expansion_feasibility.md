# DLS 26 animation expansion into DLS 18: feasibility report

## Scope and evidence

This is an offline assessment of the decoded package under `dls26/decoded` against the original DLS 18 5.064 assets. No mod was built or installed. The package labeled DLS 26 contains `libDLS19.so`; conclusions concern its extracted assets, not the provenance implied by its label.

The catalogs contain 2,541 source animation IDs and 2,535 DLS 18 IDs. Matching by full animation name gives 2,530 shared names, 11 source-only names, and five DLS 18-only names. Only 368 entries have both the same name and the same numeric ID. Therefore source numeric IDs cannot be reused directly.

## What to add and where

The proposed destination IDs below retain all original DLS 18 IDs 0–2534. They are a planning allocation, not a tested patch. Source variants ending `_f` have a header without inline frame samples and borrow both sample pointers from the **immediately preceding clip**. Each pair must stay adjacent and ordered base then `_f`.

| DLS 18 proposed ID | Source ID | Source animation | DLS 18 destination and intended trigger | Assessment |
|---:|---:|---|---|---|
| 2535 | 2522 | `ANM_STUMBLE_FT1350_STUMBLE_TO_CONTINUE_RUNNING_2_COLIN_LT` | Stumble state/group 6; candidate only where a running player recovers and keeps running | Distinct samples; high priority |
| 2536 | 2523 | Same name plus `_f` | Paired variant immediately after 2535 | Header-only; required pair |
| 2537 | 368 | `ANM_STUMBLE_EXE572_STAND_TO_STUMBLE_R_90_TO_STAND___LONGER_STUMBLE_LT` | Stumble state/group 6; strong sideways contact that ends standing | Distinct samples; medium priority |
| 2538 | 369 | Same name plus `_f` | Paired variant immediately after 2537 | Header-only; required pair |
| 2539 | 372 | `ANM_STUMBLE_EXE573_STAND_TO_STUMBLE_BACKWARDS_TO_STAND_LT` | Stumble state/group 6; backward contact that ends standing | Distinct samples; medium priority |
| 2540 | 373 | Same name plus `_f` | Paired variant immediately after 2539 | Header-only; required pair |
| 2541 | 1579 | `ANM_STUMBLE_FT856_STUMBLE__STUMBLE_1_ANDYD_LT` | Stumble state/group 6; additional general contact candidate | Distinct samples; medium priority; inspect trajectory first |
| 2542 | 1580 | Same name plus `_f` | Paired variant immediately after 2541 | Header-only; required pair |
| 2543 | 378 | `ANM_STUMBLE_EXE578_JOG_TO_STUMBLE_R_90_TO_STAND_D` | Stumble state/group 6; possible variation of DLS 18 ID 372 | Same frame sample bytes as source 376 and DLS 18 ID 372; only SAT header bytes 96–98 differ. Low priority until header meaning is understood |
| 2544 | 379 | Same name plus `_f` | Paired variant immediately after 2543 | Header-only; required pair |
| 2545 | 2540 | `ANM_STAND_EXE183X_STAND_LOOK_HIGH` | Stand state/group 0; only after a specific idle or presentation trigger is identified | 21 frames; no matching XML reference found; low priority |

`LT` and `_D` are kept as original name suffixes; their exact semantics have not been established. The trigger suggestions above are inferred from names, adjacent clips, and database group, not confirmed runtime conditions. The new stumble records are group 6 in the source database, alongside their existing DLS 18 counterparts. The stand pose is group 0. The source-only stumble bases have 6, 6, 10, 6, and 6 frames respectively, all with sample interval 3; the stand pose has 21 frames at interval 3.

## Native and archive feasibility

**Assets:** The source and destination both use SAT clips with 104-byte headers and a 42-bone animation mapping. Four new stumble bases have distinct frame payloads from their matching existing source moves. This is a credible asset-level port, subject to skeleton, root-motion, and in-game transition verification.

**Animation database:** DLS 18 `animdb.adb` starts with count 2,535 and 2,535 fixed 100-byte records, followed by 312,462 bytes of variable data. The source has count 2,541 and a separately encoded variable region. The intended additions need new records in groups 6 and 0 plus a correctly rebuilt variable region. Copying the source database would reorder existing IDs and lose DLS 18-specific entries.

**Name/hash lookup:** In DLS 18 `libDLS18.so`, `ANIM_sName` and `ANIM_uHashName` are each 10,140 bytes = 2,535 pointers or hashes. Both `CAnimManager::GetAnimID(char const*)` at `0x2aa498` and `GetAnimID(unsigned int)` at `0x2aa4c8` have an immediate upper ID of `0x9e6` (2,534). New names need extended tables and both lookup bounds changed.

**Animation library storage:** `CAnimLib` constructor at `0x2f8500` clears `0x279c` bytes = 2,535 pointer slots, then loops exactly `0x9e7` (2,535) IDs. `LoadAnim` at `0x2f85d8` writes a pointer at `this + 4*ID`, reads a flag at `this + 0x279c + ID`, and writes another per-ID pointer at `this + 0x3188 + 4*ID`. These are fixed object offsets. Simply increasing the constructor loop would write into neighboring fields. A true append requires redesigned/relocated storage or hooks for every affected access, plus an audit of other fixed ID bounds and enum-indexed tables.

**Archive:** The current DLS 18 `repack_kpx` routine replaces existing members in `anims.pak`; it does not add member-table entries. Appending 11 SAT members requires an archive writer that extends folder/file/name tables and verifies readback. This is solvable but is additional work.

**Selection:** Group 6 membership gives the stumble clips a plausible route into selection, but the exact DLS 18 candidate filters, weighting, and state transitions have not been proved for these source records. The stand pose has no discovered XML reference and no proven gameplay trigger. A loadable clip can remain invisible in play unless selection rules include it.

## Recommended decision

**Technically possible, but a high-effort native extension.** The first practical milestone is a disposable 2,536-clip build that appends one base SAT, extends the database, relocates or hooks the library arrays, and proves lookup, loading, and selection on device. If that works, add the four distinct stumble motions with their pairs. Defer the `_D` header variation and stand pose until their metadata and triggers are understood. Keep DLS 18's four dribble clips and goalkeeper-ball variant; they are absent from the source package.

An alternative with much lower native risk is to improve DLS 18's existing clips in place, but that would not meet the stated expansion goal. No runtime behavior or device compatibility is claimed here because no mod was built or launched.
