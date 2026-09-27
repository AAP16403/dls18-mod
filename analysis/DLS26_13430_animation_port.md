# DLS26 13.430 animation extraction for the DLS18 revamp

## Current extraction

The supplied 13.430 package is extracted under `dls26/decoded_13.430`. Its
`anims.pak` has 4,172 numbered SAT clips and one `animdb.adb` file. The
`game/rc/anims.rcd.txt` list has 4,172 named records in the same order as the
numbered clips. `dls26/catalog_sat_13430.py` now joins those sources with the
DLS18 name catalog and writes:

- `dls26/decoded_13.430/catalog/sat_13430_catalog.csv`: every 13.430 clip,
  its raw header values, hash, name, family, and same-name DLS18 ID.
- `dls26/decoded_13.430/catalog/sat_13430_tackle_candidates.csv`: 47 source-only
  base motions in the tackle, slide tackle, and stumble families.
- `dls26/decoded_13.430/catalog/sat_13430_summary.json`: counts and pairing checks.

The catalog found 948 names shared with DLS18, 3,224 names present only in
13.430, and 1,587 DLS18-only names. The 13.430 set has 2,090 base clips and
2,082 20-byte `_f` companions. Every `_f` companion follows its named base,
has matching leading timing fields, and has no inline payload. All 4,172 files
have zeroes in header bytes 4–19. The pairing audit reported no errors.

## Format boundary

The earlier feasibility note `DLS26_animation_expansion_feasibility.md`
examined a different extracted package with the DLS18-style 104-byte SAT
header and 2,541 animation IDs. It does not describe the current 13.430 files.
Here a base SAT starts with a 20-byte header, followed by a different packed
payload. A base clip's leading raw fields are a little-endian 16-bit value,
an 8-bit count, and a zero flag; the companion changes that flag to one and
ends after byte 20. For example, clip 0000 is 5,097 bytes and begins
`2c 00 0f 00`; clip 0001 is 20 bytes and begins `2c 00 0f 01`.

The SAT loader confirms that the leading 16-bit value is duration and the
following byte is sample count. The payload after byte 20 is not compatible with the DLS18
decoder. The current DLS26 `decoded_assets/animations/index.csv` applies the
old DLS18 loader formula and therefore marks these clips as mismatches. Those
decoder JSON files must not be used as keyframe data for a port.

The 13.430 `animdb.adb` starts with a 32-bit count of 4,172. At the database
loader in `libDLS26.so` around virtual address `0x6cbb00`, the loader reads
each serialized record as `0x68` bytes and expands it to an in-memory `0x88`
byte record. The old `extract_animdb.py` assumes `0x64` byte records and is
therefore not valid for this extraction. For every one of the 2,090 base SAT
clips, the first four SAT bytes match bytes 4–7 of its `0x68` byte database
record. The 2,082 `_f` SATs share those bytes except for their variant flag,
which is zero in the database record and one in the SAT. This confirms the
record order and provides a useful cross-check for a 13.430 parser. It does
not yet identify the SAT keyframe coding or bone mapping.

`dls26/extract_animdb_13430.py` now walks every variable array using the
13.430 loader's rules. Its catalog records all 4,172 primary arrays, 1,897
serialized secondary arrays, and 2,090 serialized tertiary arrays. Mirrored
records synthesize their secondary and tertiary data in memory. The walk ends
at byte 892,282, exactly the database file length, with no errors. The
primary arrays are signed 16-bit sample times; for example the standing
tackle at DLS26 ID 2812 has times `0, 4, 7, 10, 12, 15, 18, 22, 26, 29,
33, 38, 42, 45, 48`.

The SAT loader at `0x7724e4` calls two section decoders at `0x7720c4` and
`0x77233c`. Each base SAT stores a length-prefixed 33-track transform section
followed by a length-prefixed 19-track position section. The first transform
sample is seven signed 16-bit channels, with the seventh quaternion channel
reconstructed from channels 4–6 by the native loader; later samples use a
per-sample bitmask and signed 8-bit or 16-bit deltas for the first six
channels. Each position track starts with three signed 16-bit channels and
then uses bitmask-controlled deltas. `dls26/decode_sat_13430.py` exports those
raw integer tracks and the database sample times. It has decoded source
standing tackle ID 2812 and slide tackle ID 2808 into JSON. The source track
counts match the common DLS18 33/19 layout, but track-to-bone mapping and
gameplay timing still need conversion before installing any replacement.

`DLS18_project/analysis/port_dls26_tackle_candidate.py` builds an experimental
standing-tackle SAT using DLS26 ID 2812 and DLS18 target ID 331. It maps the
DLS26 channels into the DLS18 sample order and interpolates 15 irregular
source samples onto the target's original 11-frame, 30-tick timeline. The
DLS18 header, clip size, and existing animation selection metadata remain
unchanged. The candidate is at
`DLS18_project/mod/source/dls26_tackle_candidate_v1/0331.sat`, with a hash and
source manifest in the same directory. The DLS18 SAT decoder reads the output,
and the stick-skeleton preview renders a coherent tackling pose sequence at
`DLS18_project/previews/animations/dls26_tackle_candidate_v1.png`. This is
still an uninstalled prototype: a skeleton preview does not prove every
contact point or blend matches in the actual game.

There is stronger rig evidence than the matching track counts alone. The
13.430 `player_regular.ftm` and DLS18 `body_0_1.ftm` both have 42 bones and
identical 42-entry hierarchy links. The DLS18 42-entry bone-remap table occurs
byte-for-byte as 32-bit integers in `libDLS26.so` at virtual address
`0x1d89a4`; the DLS26 native SAT sampler references it at `0x772c0c` and
`0x772de4`. The adjacent DLS26 42-entry explicit-position map at `0x1d8a4c`
matches the first 42 entries of target DLS18 clip 331's position map. This
supports the direct track-order port, though the transform activity map still
needs a full comparison.
The DLS18 target clips 331, 737, and 1286 have identical 42-slot transform
activity maps and identical 42-slot explicit-position maps, each with 33 and
19 packed tracks. The DLS26 native position map agrees with all three.
The adjacent DLS18 `_f` companion clips are header-only mirrors: the native
`CAnimLib::LoadAnim` path loads ID minus one and copies its sample pointers,
then the sampler negates mirror components at runtime (see
`mod/anim_limits/ANIMATION_LIMITS.md`, section 1.3). Replacing an existing
base SAT therefore also changes its mirrored companion's motion without
rewriting the companion header.

## Revamp path

The immediate gameplay target is more varied, readable tackle and recovery
motion. The candidate catalog includes newer standing tackle, slide tackle,
and stumble bases with their adjacent `_f` companions. Their names are useful
for prioritization, but a source clip must be decoded, mapped to DLS18's rig,
and checked for root motion, ball contact, and foot planting before a DLS18
SAT replacement is built. Matching names alone do not imply matching keyframe
layout or selection behavior.

The prototype is packaged into a local APK based on the project's currently
installed v30 build, not the older v9 animation APK documented as crashing.
`DLS18_project/mod/build/anims_career_market_v30_dls26_tackle_v1.pak` replaces
only member `0331.sat` in v30's animation package. The signed local APK is
`DLS18_project/mod/build/DLS18_career_market_v30_dls26_tackle_proto_v1.apk`.
Its 1,017 APK entries match v30 by name; only the animation package and the
three signature entries differ in content. The v30 game library and market
library are byte-for-byte unchanged. The prototype was not installed.

A second candidate uses DLS26 ID 2306,
`ANM_TACKLE_FT1302_STANDING_TACKLES_FWD_NOEL`, in the same DLS18 target slot
331. It is the preferred forward-tackle prototype. Its right and left foot
height ranges are 319–544 and 320–690 raw units in the resampled 11 frames;
the first candidate ID 2812 raises the feet as high as 1,454 and 1,579 units.
The original DLS18 tackle spans 291–756 and 318–981. The root translation
stays `[0,0,0]` throughout all three clips. Its rig preview is
`DLS18_project/previews/animations/dls26_tackle_2306_to_0331.png`.
The source SAT, package, and signed APK are:

- `DLS18_project/mod/source/dls26_tackle_candidate_2306/0331.sat`
- `DLS18_project/mod/build/anims_career_market_v30_dls26_tackle_2306_v2.pak`
- `DLS18_project/mod/build/DLS18_career_market_v30_dls26_tackle_2306_proto_v2.apk`

The v2 APK has exactly the same entry names as v30; only the animation package
and its three signature entries differ. It has not been installed or observed
in a match, so foot placement and blends remain unproven in gameplay.

The first slide-tackle prototype uses DLS26 ID 2808,
`ANM_TACKLESLIDE_FT1491_TACKLE_SLIDE_00_ARMANI`, resampled into existing DLS18
ID 1286's 17 frames and 48-tick duration. The stick-skeleton preview at
`DLS18_project/previews/animations/dls26_slide_2808_to_1286.png` shows a
coherent transition from upright to a ground slide. Its candidate SAT is
`DLS18_project/mod/source/dls26_slide_candidate_2808_to_1286/1286.sat`.
The combined v3 package changes exactly `0331.sat` and `1286.sat` relative to
v30's animation PAK. The signed local APK is
`DLS18_project/mod/build/DLS18_career_market_v30_dls26_tackle_slide_proto_v3.apk`.
It has not been installed or match-tested.

### Contact-aligned revision (preferred local prototype)

The earlier prototypes resampled each source clip by total duration. The
database contact markers support a better time anchor: DLS26 standing tackle
2306 marks contact at tick 12 and DLS18 target 331 at tick 6; DLS26 slide
tackle 1316 marks contact at tick 24 and DLS18 target 1286 at tick 12. A
shared walk loop also runs 61 DLS26 ticks versus 32 DLS18 ticks. Together
these support roughly two DLS26 ticks per DLS18 tick. The port script now
maps both contact markers exactly and stretches the remaining tail smoothly
to the target end, preserving the target header and gameplay metadata.

The revised slide uses DLS26 ID 1316,
`ANM_TACKLESLIDE_FT0678_FT_TACKLE_TACKLESLIDE_0_ALEX`. Its 101 source ticks
are close to target 1286's 48 DLS18 ticks in real time, and its preview ends
upright like the original DLS18 clip. The v3 slide source 2808 ended on the
ground at the target clip's exit and should not be used for that slot.

The revised standing source remains DLS26 ID 2306. The two source SATs and
previews are under `mod/source/dls26_tackle_2306_contact_v4`,
`mod/source/dls26_slide_1316_contact_v4`, and
`previews/animations/dls26_{tackle_2306,slide_1316}_contact_v4.png`.
The v4 package is
`mod/build/anims_career_market_v30_dls26_contact_v4.pak`; its signed APK is
`mod/build/DLS18_career_market_v30_dls26_contact_proto_v4.apk`.
This remains uninstalled and unverified in a match.

### Short slide variant (current local prototype)

DLS26 clip 3648, `ANM_TACKLESLIDE_FT1679_TACKLE_SLIDE_00_3_ADAM`, fits DLS18
short slide slot 737. The source lasts 44 DLS26 ticks and the target lasts 21
DLS18 ticks. Their first contact markers are 24 and 12, respectively. The
source and target both finish in a low sliding pose in the stick-skeleton
preview. The candidate is
`mod/source/dls26_short_slide_3648_to_0737_v5/0737.sat`, and the preview is
`previews/animations/dls26_short_slide_3648_to_0737_v5.png`.

The current local package `mod/build/anims_career_market_v30_dls26_contact_v5.pak`
changes three existing SAT members relative to v30: `0331.sat` (forward
standing tackle), `0737.sat` (short slide), and `1286.sat` (long slide with
recovery). The signed APK is
`mod/build/DLS18_career_market_v30_dls26_contact_proto_v5.apk`. Compared with
v30, its APK entry names are identical and only the animation package and
three signing entries differ. It has not been installed or match-tested.

An edge-pose experiment built separate SATs under
`mod/source/dls26_tackle_2306_blended_v6` and
`mod/source/dls26_short_slide_3648_blended_v6`. It mixed the first and last
two frames with the original DLS18 poses. This made the short slide's maximum
mean position step across its 19 tracks rise from 651 raw units (v5 source)
to 947; a three-frame edge mix still reached 846. The standing tackle's
largest step rose from 380 to 620. The blended files were not packaged;
v5 remains the current local prototype. Runtime crossfade behavior cannot be
settled from this skeleton measure alone.

`analysis/rank_dls26_tackle_ports.py` screened all 13 source-only standing
tackles against slot 331 and all 13 source-only slide tackles against slots
737 and 1286. It writes per-candidate timing, endpoint pose difference, and
largest mean position step to three CSV files in `analysis/`. The v5 standing
source 2306 had the smallest largest step among the 13 standing candidates
(380 raw units). The v5 short-slide source 3648 likewise had the smallest
among the 13 short-slide candidates (651). For the longer slot 1286, source
1316 is less smooth by that measure (758), but its start/end position
differences (144/276) and near-matched duration make it more compatible with
the original action than shorter clips that finish far from its upright exit.
These offline measures guide source selection; they do not establish in-game
blend quality.

The other two directional DLS18 standing-tackle slots, 329 (left 45 degrees)
and 333 (right 45 degrees), were screened against the same 13 DLS26 standing
candidates in `analysis/dls26_standing_0329_rank.csv` and
`analysis/dls26_standing_0333_rank.csv`. Source 2814 in slot 329 and source
2310 in slot 333 can be decoded and drawn, but their exit poses differ from
the original DLS18 directional clips. The available source names and database
direction fields do not prove a safe left/right correspondence. These two
slots remain unchanged in v5; replacing them would risk a wrong-direction
action during a tackle.

### Appended animation pair (isolated experiment)

`analysis/append_tackle_animation.py` appends a fourth DLS26 motion without
overwriting another DLS18 slot. DLS26 clip 2308
(`ANM_TACKLE_FT1303_STANDING_TACKLES_FWD_1_NOEL`) was converted using the
forward standing-tackle layout and timing. The new DLS18 IDs are 2535 (base)
and 2536 (header-only mirror); their database records and serialized curve
arrays are copied from the compatible 331/332 pair. The resulting package is
`mod/build/anims_career_market_v30_dls26_append_2308_experiment.pak`.
Its 2,538 members and 2,537 database records decode, and all existing package
members retain their decoded bytes except `animdb.adb`.

`analysis/enable_appended_tackle_ids.py` raises the v30 library's clip count
from 2,535 to 2,537 at the five load-loop sites, adjusts the preload bound,
and moves three NIS sentinel uses to 0x7fff. The already expanded 3,840-slot
cache layout is unchanged. The signed, uninstalled package is
`mod/build/DLS18_career_market_v30_dls26_append_2308_experiment.apk`.
That first experiment exactly cloned 331/332's contact coordinates. In
`ACT_TackleSetPlayerState`, the candidate scan retains the earlier clip when
two scores tie, so the new pair would lose a tie to 331/332. The revised
`mod/build/anims_career_market_v30_dls26_append_2308_contact_v2.pak` keeps
the DLS18 contact time of 6 but uses DLS26 2308's contact position
(-2184, 160, -883), with the lateral coordinate mirrored for 2536. This
gives the new pair distinct selector geometry. The corresponding signed APK
is `mod/build/DLS18_career_market_v30_dls26_append_2308_contact_v2.apk`.
The source contact coordinates are assumed to share DLS18 spatial units;
the actual selection frequency and alignment still require an in-game check.

### Added stumble recovery pair (current isolated experiment)

The append tool now accepts an already expanded animation package. DLS26
clip 2408, `ANM_STUMBLE_FT1350_STUMBLE_TO_CONTINUE_RUNNING_2_COLIN_LT`, was
retimed to the DLS18 2517 layout and added as IDs 2537/2538. The original
source pose differed substantially at entry and exit. A four-frame blend
with DLS18 2517 at both ends keeps those transition poses aligned while
preserving ten full DLS26 middle frames. Its largest mean position step is
400 raw units, versus 440 for the original clip and 453 for the earlier
three-frame blend. Its largest mean rotation step is 13.3 degrees, versus
14.1 degrees for the original. The unblended port was smoother internally
but began 1,139 raw units away from the original pose. The chosen SAT and
preview are `mod/source/dls26_stumble_2408_to_2517_edge4/2517.sat` and
`previews/animations/dls26_stumble_2408_edge4_v6.png`.

The source screen in `analysis/dls26_stumble_2517_rank_raw.csv` and
`analysis/dls26_stumble_2517_rank_edge3.csv` covers all 21 DLS26-only
stumble bases. Some have smaller pose or frame-step differences, but their
named actions are jumps, avoidance, or stand-and-fall reactions rather than
continuing a run. The 2408 variant remains the semantic match for 2517.

The new stumble record keeps the DLS18 2517 metadata except for a distinct
first action point from DLS26 2408. Its source time 4 was scaled from 41
source ticks to 34 target ticks and rounded to DLS18 time 3; its position is
(1353, 96, -1054), mirrored across the lateral axis for ID 2538. The
current isolated package is
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v6.apk`.
It contains 2,539 animation records and a library with a matching 2,539
clip load count. The archive and APK contents decode locally. Runtime
selection, contact alignment, and transition quality are unverified.

### Second stumble recovery pair

DLS26 clip 870, `ANM_STUMBLE_EXE573_STAND_TO_STUMBLE_BACKWARDS_TO_STAND_LT`,
was retimed into DLS18's backward recovery slot 368 and appended as IDs
2539/2540. The two-frame edge blend matches the original entry and exit
poses; the candidate's largest mean position step is 399 raw units versus
426 for the original. The new record remains in state group 6 and keeps a
zero first action time, matching the DLS18 slot. The final local package is
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v7.apk`.
It contains 2,541 database records, 2,542 KPX members, and patched load-loop
constants for 2,541 clips. The native package reader scans its dynamic file
table by name, so the new numbered SAT members can follow `animdb.adb`.
All six appended SAT IDs (2535–2540) are present and decode in the APK.
The reproducibility record with component hashes and source IDs is
`analysis/animation_revamp_manifest.json`.

### Retarget endpoint alignment

`port_dls26_tackle_candidate.py` now has an opt-in `--align-positions`
retarget mode. It linearly offsets each DLS26 position track so its first and
last samples land on the DLS18 target endpoints while retaining the source
motion in between. Applying it to the forward standing tackle 2306→331
reduces the largest mean position step from 380 to 331 raw units and makes
both endpoint position differences zero. Applying it to the short slide
3648→737 raises the largest step from 651 to 741, and applying it to the
long slide 1316→1286 raises it from 758 to 761, so those slides remain on
their original v5 ports. The aligned standing SAT is packaged in
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v9.apk`.
The v9 verifier confirmed all eight appended pairs and the 2,543-clip native
count. The current v10 package adds two further general-stumble pairs below.

The screened DLS26 slide 3650→737 variant has a 1,095/1,482 raw-unit
endpoint mismatch and a 686 raw-unit largest step before alignment; it is
not included because it would make slide transitions less predictable.

### Third recovery pair and current package

DLS26 clip 864, `ANM_STUMBLE_EXE572_STAND_TO_STUMBLE_R_90_TO_STAND___LONGER_STUMBLE_LT`,
was retimed into DLS18's matching sideways recovery slot 366 and appended as
IDs 2541/2542. A two-frame edge blend matches the original entry and exit
poses. Its largest mean position step is 570 raw units versus 502 for the
original slot; the source name and trajectory are a direct semantic match,
so it is retained as a directional variation despite the higher internal
step. The current package is
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v9.apk`.
It had 2,543 database records and 2,544 KPX members. The v10 package retains
this pair and adds the general-stumble variants below.

### General stumble variants

DLS26 clips 1292 (`ANM_STUMBLE_FT0653_FT_STUMBLE_0_ALEX`) and 1294
(`ANM_STUMBLE_FT0658_FT_STUMBLE_0_ANDY`) were retimed into the matching DLS18
general-stumble layouts 1257 and 1259. They were appended as IDs 2543/2544
and 2545/2546, respectively, each with a two-frame edge blend. The 1292
candidate's largest mean position step is 537 raw units versus 525 for its
DLS18 target; the 1294 candidate measures 644 versus 579. Both previews show
complete, coherent stumble poses and exact endpoint position matches. These
are retained as source-name-matched variation clips rather than replacements
for the original DLS18 actions.

An endpoint-aligned pass was screened for both variants. It raises the
largest mean position step to 561 for 1292→1257 and 697 for 1294→1259, so
the v10 package keeps the edge-blended candidates without position
alignment.

The v10 package is
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v10.apk`.
It contains 2,547 database records, 2,548 KPX members, and native load-loop
constants for 2,547 clips. The verifier reports all twelve appended IDs
(2535–2546), their category and mirror flags, and all five native count
sites. The state-group counts rise from 33 to 43 for category 6 (stumbles)
and from 10 to 12 for category 9 (tackles), confirming that the appended
records enter the same dynamic candidate lists as their target actions.

### Source-only DLS26 deek variations

DLS26 clips 3304, 3306, and 3308
(`ANM_DEEK_FT1638_DEEK_01_DAVID`, `_02_DAVID`, and `_03_DAVID`) have no
DLS18 ID with the same source name. They were retimed to DLS18 deek layouts
2331, 2335, and 2335 and appended as pairs 2547/2548, 2549/2550, and
2551/2552. Each keeps the DLS18 category-19 selector metadata, uses a
four-frame edge blend, and linearly aligns position tracks to the target
entry and exit.

The largest mean position steps are 463 raw units for 3304→2331, 434 for
3306→2335, and 374 for 3308→2335. The corresponding DLS18 targets measure
601 and 415 raw units. The stick-skeleton previews show complete one-shot
poses and preserve the target entry/exit positions. The 3306 clip has a
more pronounced mid-move lean, while 3308 supplies a smoother alternate
trajectory.

The v11 package is
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v11.apk`.
It contains 2,553 database records, 2,554 KPX members, and native load-loop
constants for 2,553 clips. The verifier reports all eighteen appended IDs
(2535–2552), their category and mirror flags, and all five native count
sites.
Category 19 rises from 16 to 22, so the deek selector now has three new
source-derived variations plus their mirrored forms.

### Source-only DLS26 close-control variations

Four DLS26 control clips were screened from the source-only `CONTROL` family
against DLS18 category-1 layouts. The retained set is DLS26 3260
(`ANM_CONTROL_FT1631_CONTROL_DRIBBLE_01_ADAM`) to layout 1331, 3262
(`...DRIBBLE_02_DAVID`) to 1333, 3264 (`...DRIBBLE_03_DAVID`) to 1335,
and 3282 (`ANM_CONTROL_FT1635_CONTROL_SPECIAL_06_DAVID`) to 1360.
Each retarget uses a four-frame edge blend and endpoint position alignment.
They were appended as category-1 pairs 2553/2554, 2555/2556, 2557/2558,
and 2559/2560.

The candidate maximum mean position steps are 535, 487, 466, and 464 raw
units respectively. Their entry and exit positions remain aligned to the
DLS18 templates, and the skeleton previews show complete close-control
poses. The screening table is `analysis/dls26_control_screen.csv`.
The category-1 selector grows from 351 to 359 candidates.

The v12 package is the first close-control build: it contains 2,561 database records and 2,562
KPX members, with native load-loop constants for 2,561 clips. The verifier
reports all twenty-six appended IDs (2535–2560), their category and mirror
flags, and all five native count sites. Category 1 rises from 351 to 359
and category 19 remains at 22.

### DLS26 sprint loop replacements

The DLS26 ET0141, ET0142, and ET0143 sprint loops (source IDs 72, 74,
and 76) were retimed to the existing hard-coded DLS18 sprint slots 512,
514, and 516. Their mirrored headers at 513, 515, and 517 remain paired
with the replaced base clips. The replacements use fit timing with no edge
blend so the loop endpoints stay cyclic, plus a constant endpoint position
alignment.

The largest mean position steps fall from 521 to 498 for slot 512, from
545 to 517 for slot 514, and from 496 to 477 for slot 516. The candidate
loops preserve the stock frame counts and are payload replacements only,
so no state-list or native count changes were needed. The replacement
record is analysis/anims_career_market_v30_dls26_tackle_stumble_append_v13_sprint.json.

The current package is v13.
APK: mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v13_sprint.apk.
It retains the 2,561-record v12 database and adds the three DLS26 sprint
payload replacements. The append verifier reports all twenty-six appended
IDs (2535–2560), category 1 at 359, category 19 at 22, and all five native
count sites at 2,561.

### DLS26 sidestep replacements

Five DLS26 sidestep clips were screened against their matching DLS18 category-3
layouts and retained as payload replacements. The set is DLS26 186 to DLS18
slot 686 (forward), 188 to 693 (180 degrees), 190 to 695 (low 0 degrees),
192 to 698 (low 90 degrees), and 194 to 702 (low 180 degrees). Each candidate
uses fit timing, a two-frame edge blend, and endpoint position alignment. The
clips retain the DLS18 frame counts and headers, so the animation database and
native count remain unchanged. Slots 686 and 695 are standalone DLS18 bases;
their adjacent entries were left untouched. The other three keep their existing
mirrored entries.

The largest mean position step changes are 330 to 327 for 686, 306 to 308 for
693, 265 to 245 for 695, 379 to 383 for 698, and 268 to 239 for 702. The
screening table is `analysis/dls26_sidestep_screen.csv`; the five candidate SAT
directories are under `mod/source/sidestep_screen/`.

The current package is v14:
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v14_sidestep.apk`.
It retains the 2,561-record v13 database and its sprint replacements while
using the five DLS26 sidestep payloads in the existing category-3 slots. v13
and the original SATs remain rollback sources.

Next implementation step: when device testing is requested, check the five
sidestep directions together with the control, deek, recovery, and sprint
loops in a match. Tune selector metadata only after observing the actual
triggers.

### DLS26 turn replacements

Six exact-name DLS26 turn clips were screened against DLS18 category-5 turn
layouts and retained as payload replacements. The first four cover the basic
standing 45, 90, 135, and 180 degree turns (source IDs 510, 514, 518, and
522 to DLS18 slots 13, 15, 17, and 19). Sources 1288 and 1290 replace the
matching ALEX stand-turn slots 1253 and 1255. Fit timing and endpoint position
alignment keep the DLS18 frame counts and transition positions; no edge blend
was needed because these are exact-name counterparts. Their mirrored entries
remain paired and unchanged.

The mean position step falls for all six retained clips: 49 to 44, 75 to 68,
109 to 98, 125 to 107, 121 to 119, and 96 to 86 raw units respectively.
The walk-turn and stand-to-sprint candidates were screened but not included
because their largest position steps increased sharply. The screening table is
`analysis/dls26_turn_screen.csv` and the candidate SATs are under
`mod/source/turn_screen/`.

The current package is v15:
`mod/build/DLS18_career_market_v30_dls26_tackle_stumble_append_v15_turns.apk`.
It retains the v14 sidestep replacements and the 2,561-record database while
using the six DLS26 turn payloads in the existing category-5 slots. v14, v13,
and the original SATs remain rollback sources.

No DLS26 13.430 SAT payload has been installed in DLS18 yet. This document
records an extraction, decoder, and locally packaged retarget prototype, not
a claim of a working game mod.

## v16 tuning: contact timing, ball points and root travel

v15 was playtested on the tablet: the clips play, but ball placement and
tackle contact don't match the motion, and the players don't travel with
their feet. One appended control clip also crashed the game: a divide by zero
in `CPlayer::SetAnimControl`, with r10 = 2553. The v16 package fixes the data
behind both problems. It keeps the same ID layout: 2,561 records, 2,562 KPX
members, and native count 2,561. No library change is needed.

- Package: `mod/build/anims_career_market_v30_dls26_tackle_stumble_append_v16_tuned.pak`
  (11,366,370 bytes, SHA-256 `6d463f0f...3937386`). It is built from v15 and changes
  44 members. It is smaller than v15 because the v15 append tool left stale
  database payloads in the archive.
- SATs and database: `mod/source/dls26_tuned_v16/` (`NNNN.sat` and `animdb.adb`).
- Scripts: `analysis/anim_tune_lib.py` (helpers), `analysis/tune_dls26_ports.py`
  (build, including the log `analysis/dls26_tuned_v16_build.json`),
  `analysis/validate_dls26_ports.py` (writes `dls26_tuned_v16_validation.{csv,md}`),
  and `analysis/preview_dls26_tuning.py`.
- Previews: `previews/animations/dls26_tuned_v16_topview.png` shows the root path, foot paths
  and action points from above for stock, v15 and v16.
  `previews/animations/dls26_tuned_v16_poses.png` shows the skeleton poses.

### Engine data model (from the library, verified on stock data)

- **Root motion is stored in `animdb.adb`, not in the SAT.** Every stock SAT keeps the
  pelvis at x = y = 0. Record `+0x28` holds an XZ root curve with `+8` samples,
  one every `+0xa` record ticks. Record `+0x2c` holds a heading curve with 2,048
  units per turn. `SET_ROOT_POS` @0x2aa058 samples the curve at
  `frac * dur / step`. `CPlayer::GetTrueRot` @0x2dca58 adds the heading curve
  (shifted left by 3) to the player angle. Record `+0x58`/`+0x5a` hold the start and end
  heading divided by 32, and every stock record agrees with its curve. DLS26 stores
  the same curves as its secondary and tertiary arrays, on the same axes and in the same
  units: exact-name pairs such as 554->49 and 510->13 have almost identical curves.
- **Time units.** Record `+6` is the clip duration in 60 Hz record ticks, and SAT
  header `+0` is exactly half of it. SAT frame f plays at record tick
  `2 * f * interval`, the same tick as root-curve sample f. Action-point times (`+0x30 + 8k`) are
  30 Hz ticks: `AnimDataFill` stores `(t << 17) / dur` as a u16, so t must stay below
  `dur / 2`. `GetActionTime` @0x2aa194 returns about `t * 100 / pct` game ticks. DLS26 stores
  action-point times in record ticks, so a DLS26 time 12 is DLS18 time 6.
- **Action-point position** is the ball centre relative to the clip's start, including root
  travel. Its units are the root-curve units: 16 = 1 cm, and SAT position units are
  32 = 1 cm. x is negative forward and z is positive to the left. Fitting feet to the action
  points of 189 stock control, tackle and deek clips confirmed this convention (median
  error 275; flipping either axis gives 554 or more). The frame is the start frame, not the
  contact frame, and time uses 2t/dur (fitting t/dur gives 634).
  `UpdateActionDeek` @0x2dbbec moves the ball to action point k+1 by its tick after
  each touch. SetAnimControl and GA_SetAnimFromDeek score candidates on action point 0
  alone. Stock control clips carry two action points in 345 of 351 records: the touch, and
  the ball position on the last tick (`dur/2 - 1`).
- **SAT position tracks** equal forward kinematics of the transforms times 32 in every stock
  clip, to within 0.1 cm. `CAnimManager::GetBonePositionAnim` reads them. DLS26
  transform records are ordered (t0..t2, q0..q3), while DLS18 uses (q0..q3, t0..t2). The
  feet and toes from DLS18-rig FK match the DLS26 position tracks to within 1 cm.
- A root-curve scale of 2 (SAT units per curve unit) minimises stance-foot slide over
  180 stock clips. For example, dribble loops 710 and 712 drop from 900-1,100 to about 30
  units per frame. Heading is counter-clockwise positive.

### What v15 got wrong

1. The 14 appended deek and control records (2547-2560) had no action points. The
   selector's tick count to action point 0 was then 0, which caused the crash. With no
   touches, the ball also never moved with the deek.
2. The appended records kept the template's root and heading curves under the DLS26
   pose, so the body moved along the old clip's path. For example, 2535 swings its leg
   forward-right while the 331 curve carries the player 63 degrees to the left. 2557 is a
   forward dribble that ran on 1335's backward curve. 2559 moves 45 degrees to the
   right on 1360's 52-degree-left curve. These paths are the "sliding off-line" seen in the playtest.
3. Position tracks were shifted linearly without changing the transforms, so the
   gameplay bone positions disagreed with the rendered skeleton.
4. The sampling time was `(frames - 1) * interval` rather than the played length
   `dur / 2`, so loops 514 and 516 lost their last source tick at the seam.

### What v16 does

- **Resampling.** Each port is rebuilt from its DLS26 source through a piecewise-linear
  time map that places every source contact marker exactly on a DLS18 action-point
  tick. Quaternions are normalised-lerped, and positions are recomputed with FK.
- **Appended clips (2535-2560).** Each one keeps the source duration, rounded up to an
  even number, with a 2-tick SAT interval. It uses the source's own root curve, heading
  curve and action points. The mirror record negates z and the heading. Control clips
  also get the DLS18 end-of-clip ball point: the template's ball lead from its root end,
  rotated by the change in end heading. Because of the time map, source contact ticks
  12, 16 and 24 land exactly on DLS18 ticks 6, 8 and 12.
- **Replaced tackles and slides (331, 737, 1286).** These keep the stock duration, stock
  action point and stock travel. The source root path is bent with one similarity
  transform up to contact, which puts the source ball point on the stock action point
  at the stock tick, and a second transform from contact to the end, which lands on the
  stock displacement. A heading ramp after contact makes the body's final yaw match the
  stock clip's (heading plus pose yaw), so the next DLS18 clip in the chain lines up.
  - 331: scale 3.05 and rotation +24 deg before contact, scale 2.00 and +8.5 deg after,
    exit heading -12 deg.
  - 737: scale 1.56 and -5.9 deg before contact, 1.33 and -1.4 deg after, exit heading -65 deg.
  - 1286: scale 1.02 and +8.3 deg before contact, 0.96 and +6.4 deg after, exit heading -13 deg.
- **Loops, sidesteps and turns.** These keep their stock layout. The source curves are
  used where they exist (the turns), and sampling runs over the played length.
- **Edge blending with the template pose** is now 0 for every clip with an XZ root curve.
  Under a real root curve it made the planted foot skate: 2541 measured 56 units per tick
  with a 2-frame blend and 9 without. The engine's own crossfade between the two blended
  clips covers the entry and exit instead. The only clips that keep their v15 blends are
  the sidesteps and the stumbles 2543/2545, which have no XZ curve.
- **Heading curves are not wrapped**, so the 3304 full spin stays continuous from -1,969 to 79.
  GetTrueRot masks the angle after adding the curve value.

### Validation (`analysis/validate_dls26_ports.py`)

Column definitions:

- **AP**: action-point ticks.
- **err**: the tick where a foot or toe passes closest to the action-point ball, minus the action-point tick.
- **gap**: the foot-to-ball distance at the action-point tick, in root units.
- **travel**: the root displacement at the clip's end, as length@degrees.
- **slide**: the mean horizontal speed of a planted foot, in root units per 60 Hz tick.
- **sweep**: the stance-foot speed in the pose.

The reference ("ref") is the stock slot for replacements and the copied template for
appended clips. The DLS26 source row ("src") shows the relationship the source
authors built. The full table is `analysis/dls26_tuned_v16_validation.md`.

| id | ref: AP / err / gap / travel / slide | src | v15 | v16 |
|---|---|---|---|---|
| 331 | 6 / 5.8 / 552 / 3961@63 / 39.6 | 6 / 13.9 / 218 / 1721@50 / 25.6 | 6 / 13.4 / 540 / 3961@63 / 60.3 | 6 / 11.2 / 233 / 3864@64 / 49.8 |
| 737 | 12 / 8.7 / 1570 / 4449@-13 / 143.7 | 12 / 10.0 / 1444 / 3005@-8 / 74.7 | 12 / 9.0 / 1425 / 4449@-13 / 129.9 | 12 / 5.1 / 1468 / 4449@-13 / 112.6 |
| 1286 | 12 / 25.9 / 1562 / 4114@-6 / 69.0 | 12 / 25.4 / 1499 / 4133@-14 / 41.3 | 12 / 24.5 / 1537 / 4114@-6 / 85.0 | 12 / 23.9 / 1499 / 4117@-6 / 80.7 |
| 2535 | 6 / 5.8 / 552 / 3961@63 / 39.6 | 6 / 7.2 / 240 / 4825@-8 / 26.9 | 6 / -0.6 / 1839 / 3961@63 / 90.6 | 6 / 7.2 / 238 / 4825@-8 / 29.5 |
| 2537 | - / 6284@7 / 53.9 | 2 / 4084@27 / 36.7 | 3 / 6284@7 / 82.7 | 2 / 3970@27 / 37.6 |
| 2539 | 1652@-171 / 18.7 | 1664@-171 / 18.7 | 1652@-171 / 17.4 | 1651@-171 / 19.1 |
| 2541 | 2795@101 / 30.5 | 2077@102 / 16.0 | 2795@101 / 60.3 | 2056@102 / 18.0 |
| 2543 | 0 / 39.9 | 237@165 / 41.8 | 0 / 31.9 | 0 / 29.8 |
| 2545 | 0 / 45.5 | 428@-165 / 39.2 | 0 / 39.2 | 0 / 40.7 |
| 2547 | 12 17 50 / 0.5 / 127 / 6301@-13 / 69.4 | 12 16.5 35.5 / 3.0 / 769 / 7272@6 / 75.2 | none / 6301@-13 / 74.7 | 12 16 36 / 3.1 / 770 / 7159@6 / 71.2 |
| 2549 | 12 17 44 / -5.9 / 613 / 5479@16 / 71.3 | 14 27 / 1.4 / 240 / 5114@-22 / 32.3 | none / 5479@16 / 79.1 | 14 27 / 1.4 / 241 / 5030@-21 / 32.1 |
| 2551 | 12 17 44 / -5.9 / 613 / 5479@16 / 71.3 | 17 28 / 3.0 / 121 / 5578@12 / 27.4 | none / 5479@16 / 125.0 | 17 28 / 3.3 / 170 / 5578@12 / 24.0 |
| 2553 | 8 21 / 1.2 / 146 / 4542@-2 / 30.5 | 8 / 1.8 / 136 / 5368@-2 / 22.8 | none / 4542@-2 / 14.2 | 8 25 / 1.8 / 134 / 5368@-2 / 22.5 |
| 2555 | 8 22 / 0.2 / 101 / 5164@-21 / 36.6 | 8 / 0.9 / 169 / 6130@13 / 29.1 | none / 5164@-21 / 48.6 | 8 25 / 0.8 / 171 / 6130@13 / 28.3 |
| 2557 | 8 28 / 17.6 / 689 / 1596@-145 / 61.0 | 8 / 0.5 / 162 / 5706@4 / 29.6 | none / 1596@-145 / 96.0 | 8 24 / 0.5 / 162 / 5574@4 / 31.5 |
| 2559 | 8 29 / 0.9 / 222 / 4185@52 / 69.5 | 8 / -1.9 / 438 / 3985@-45 / 37.7 | none / 4185@52 / 104.3 | 8 31 / -1.9 / 438 / 3985@-45 / 35.5 |

Game ticks to action point 0 (`GetActionTime`) in v16: 4 for 331 and 2535, 9 for 737 and
1286, 1 for 2537, 10/11/13 for the deeks, and 6 for all four control clips. v15 had 0 for
2547-2560. The build refuses any category 1, 8, 9 or 19 record with zero ticks or a u16
time overflow.

The per-cycle numbers for locomotion clips are below. v16 keeps the stock duration for
each of them.

| clip | stock sweep | v15 | v16 | note |
|---|---|---|---|---|
| sprint 512 | 93.5 | 89.8 | 89.8 | -4% |
| sprint 514 | 91.9 | 86.7 | 104.3 | +13%; the DLS26 ET0142 source itself sweeps 25% more per tick |
| sprint 516 | 108.3 | 93.9 | 102.0 | -6% |
| sidesteps 686/693/695/698/702 | 44.6/48.5/47.9/47.6/45.7 | 42.9/40.2/45.6/58.4/43.0 | 45.0/47.5/45.6/48.0/45.9 | all within 5% |

- **Turns 13/15/17/19/1253/1255**: travel is 142/96/150/134/69/148 against a stock
  140/75/134/127/62/168. The largest difference is 21 units, about 1.3 cm. Planted-foot
  slide is lower than v15 for all six and within 1 unit per tick of stock.

### Open points (need the device)

- **331** now meets the stock action point at tick 6 and ends on the stock travel. Its
  DLS26 source (2306) is a standing tackle, while slot 331 is `JOG_FWD_TO_TACKLE`. The body
  therefore has to glide to the stock contact point: the root scale is 3x before contact,
  and planted-foot slide is 49.8 against 39.6 for stock (v15 was 60.3). If this reads as
  skating, either keep 2306 with its own reach by setting 331's action point from the
  source motion, or revert 331 to stock.
- **737** now ends with the stock body yaw, which takes a 65 deg heading ramp after contact
  because 3648 finishes turned. It could be revisited with DLS26 226, which has the same
  name as 737, `ET0627_EX2037_SLIDE`, and an almost identical action point.
- **Appended clips enter from their own first pose.** The mean entry difference from
  the template's first pose is 9-42 cm. The engine crossfade hides it, but check the
  transitions into the dribbles (2553-2559), the deeks and stumble 2537.
- **Ball behaviour after the touch** needs a device check: the control end ball points
  come from the template lead, and the deeks now touch the ball at the DLS26 ticks.
- Stumbles 2543/2545 keep the template's lack of XZ root motion, which drops 15 cm and
  27 cm of source drift.
