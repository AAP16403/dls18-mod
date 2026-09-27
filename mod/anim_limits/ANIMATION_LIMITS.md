# DLS18 animation system: every cap on adding or unlocking animations

Target: `unpacked/apk/lib/armeabi-v7a/libDLS18.so` (v5.064, armeabi-v7a, **stock** library, not the modded build).
This was static analysis only. The game was not run and adb was not used. Scratch files are in `F:\android modding\tmp\anim_caps\`
(`text_all.txt` is the full .text disassembly, `gotres.py` annotates GOT and literals, `d.sh` disassembles one function, `hits.py` does immediate searches).

Tags: **[V]** = verified in disassembly or data. **[G]** = guessed or inferred and not fully traced.
All VAs are link-time offsets. At runtime add the random load base (see `mod/PATCHING_RULES.md`).

---

## 0. Summary

* The only hard ceiling on clip count is **`CAnimLib`**, the SAT clip cache. Its object has a fixed layout for **2535** clips. Every other animation table is sized at runtime from the `animdb.adb` count.
* `animdb.adb` is fully dynamic (`count` header). The **anim ID is the record index**, so new animations must be **appended as IDs 2535+**. Existing IDs are hard-coded throughout the game code.
* The **20 state/candidate lists** (`s_tStateList`) are a fixed count. Their contents are built dynamically from the category byte of each adb record. Any appended record with category 0..19 automatically becomes a candidate for that state's selector. No list capacity limits it.
* NIS scenes (celebrations and cut-scenes) find anims **by name hash**, and that lookup only searches the fixed 2535-entry `ANIM_uHashName` table. New IDs cannot be named in NIS XML unless that lookup is hooked.
* Unlockable today with data only: **ID 69/70** (stand-to-45° stepover), which no caller ever requests (deek style 3). Also **≈166 non-mirror NIS clips** that no NIS XML references, including celebrations such as violin, salute, arms wide, shine boots and several dance/excited sets.

---

## 1. Asset pipeline

### 1.1 anims.pak (KPX)  [V]
`unpacked/apk/assets/data/anims/anims.pak` is 11,251,772 bytes with marker `\0KPX`. It has **1 folder and 2536 files** (0000.sat..2534.sat plus `animdb.adb` as member 2535), and a names blob of 22,828 bytes.

| Part | Layout |
|---|---|
| header | `char[4] marker; u32 folders; u32 files; u32 names_size` |
| folder rec (20 B) | `name_off, file_count, child_count, first_file, first_child` (root: `0,2536,0,0,0`) |
| file rec (24 B) | `name_off, size, offset, zip(0/1), crc/timestamp, zsize` |
| names | NUL-terminated names, offsets relative to the end of the tables |

Native reader: `CFTTFileSystem_PAK::Initialise` @0x3af571 allocates the folder table (`new[n*0x14]` @0x3af67a), the file table (`n*0x18` @0x3af6a0) and the names blob (@0x3af6b2) from the header counts. There is **no fixed member cap**; counts are u32.
Lookup: `GetFileIndexInternal` @0x3afad5 walks folders by `/`, then does a **linear `strcasecmp`** over the folder's file range (@0x3afb40..0x3afb50). Member order and sorting do not matter.
To add a member, append a file record inside folder 0's range, bump `folder[0].file_count` and `header.files`, append the name, then rewrite all offsets. The existing `repack_kpx` in `analysis/rework_dribble_animations.py` only replaces members, so it needs a full table rewrite to add them.

### 1.2 Who opens the pak  [V]
* `CAnimManager::LoadAnimDB` @0x2a9235 opens `PKG:/Data/anims/anims.pak`, mounts it as `g_pFTTFileSystem` and reads `animdb.adb`.
* `CAnimLib::CAnimLib` @0x2f8501 opens it again and keeps the PAK FS at `this+0x80cc` and the file at `this+0x80c8`. `CAnimLib::LoadAnim` @0x2f85d9 swaps it into `g_pFTTFileSystem` and opens `sprintf("%04i.sat", id)` (format string @0x6067ee). An ID ≥ 10000 simply gets 5 digits.

### 1.3 SAT clip loader: `SAT_LoadAnimation(const char*, int&)` @0x2f73d5  [V]
* Reads a 0x68-byte header, then does **one allocation**: `CFTTMem::Allocate_Internal(heap 0, size)` @0x2f74ba. The size is `0x68 + 14*frames*tracks + 6*frames*postracks` (computed @0x2f7482..0x2f749e).
* `frames` = header `+4`, read **signed 8-bit** (`sxtb` @0x2f7484), so **≤127 frames per clip**. The sampler also clamps the frame index with this s8 (`SAT_CreateSamples` @0x2f7d84/0x2f7da0).
* Transform tracks = `+7` (s8). The transform map is **42** bytes at `+8..+0x31`. Position tracks = `+0x32` (s8). The position map is **45** bytes at `+0x33` (memcpy 0x2d @0x2f745a).
* If `fopen` fails, the function returns NULL, and `LoadAnim` then dereferences it (`ldrb r1,[r0,#5]` @0x2f861a). **Every ID 0..2534 must have a .sat member, or the game crashes at boot.**
* Header `+5` ≠ 0 means a header-only **mirror** clip (the "_f" names). `LoadAnim` loads **ID-1** first and copies its data pointers `+0x60/+0x64` (@0x2f861e..0x2f864a). `SAT_CreateSamples` then negates components at runtime (vneg block @0x2f7e2a..0x2f7e80). **A mirror must sit immediately after its source ID.**
* Timing: header `+2` is the s16 sample interval in 30 Hz ticks (@0x2f7d5c). Long moves can use a coarser interval to stay under 127 frames.

### 1.4 Clip cache: `CAnimLib` (the hard cap)  [V]
It is allocated once in `CGfxCharacter::Init` with `operator new(0x80d0)` @0x2fae96 and stored in `CGfxCharacter::s_pAnimLib` (GOT 0x73169c).

| Offset | Size | Field |
|---|---|---|
| 0x0000 | 2535×4 | `TSATAnim_TSX* clip[id]` |
| 0x279c | 2535×1 | `u8 state[id]` (0 = unloaded, 1 = permanent, 2 = dynamic) |
| 0x3184 | 4 | loaded-clip count |
| 0x3188 | 2535×4 | `int bytes[id]` |
| 0x5924 | 2535×4 | `int lastUsedTick[id]` |
| 0x80c0 / 0x80c4 | 4+4 | permanent bytes / dynamic bytes |
| 0x80c8 / 0x80cc | 4+4 | pak file / PAK FS |

* The ctor **loads all 2535 clips as permanent** (loop `movw r6,#0x9e7` @0x2f8576, `LoadAnim(i,1)`). It then runs the 15-entry `ms_tPreLoadAnimData` table (0x7451fc, 240 B). Those clips are already resident, so the preload is a no-op.
* All clips stay resident: about **17.2 MB** of SAT data on the general heap (id 0). There is no fixed pool. `UnloadPermanentAnims`/`ReloadPermanentAnims` have no callers [V via xref].
* A dynamic LRU exists for clips loaded later through `GetAnim` (@0x2f88d9): when the dynamic total exceeds **0x60000**, it frees clips older than 60 ticks down to **0x30000** (`FreeDynamicAnims` @0x2f8751). In practice this is inactive.
* **No bounds checks.** Clip ID 2535 lands on the last padding byte of `state[]`, and ID ≥ 2536 corrupts `+0x3184` onwards. A new ID therefore needs a relayout of `CAnimLib`.
* Every site that bakes in 2535, all `movw`: offsets 0x279c/0x3184/0x3188/0x5924/0x80c0-0x80cc at 42 sites in the ctor, `LoadAnim`, `PreLoadAnims`, `FreeDynamicAnims`, `FreeAnim`, `Unload/ReloadPermanentAnims`, `GetAnim`, `CheckAnim` and the dtor. The full list with VAs is in `tmp/anim_caps/caps_hits.txt`. The loop bound `0x9e7` appears @0x2f8576, 0x2f8778, 0x2f8866, 0x2f889a and 0x2f8962. `PreLoadAnims` uses the end bound `2535*0x84 = 0x51b1c` (`movw sb,#0x1b1c` @0x2f86e4 + `movt sb,#5` @0x2f86f4). The allocation size is `0x80d0` @0x2fae96.

### 1.5 Skeleton / sampling  [V]
* Fixed **42-bone** rig: `bone_remap` (0x745138, 168 B = 42 ints). `SAT_BlendSamples(...,0x2a,...)` @0x2fca96. `TSATFrameSample` is 28 bytes, 42 per pose, and lives in stack buffers in `CGfxCharacter::GenerateSamples` @0x2fc925 (`sub sp,#0x938`).
* Blending per player is **2 clips**: `CPlayer+0x54` current id / `+0x58` frame, and `+0x60` previous id / `+0x64` frame. The crossfade weight at `+0x6e` starts at 0x2000 (`CPlayer::SetAnim` @0x2dc785, 0x2dc8b6..0x2dc8c2). Locomotion adds walk/jog/sprint layers from the hard-coded `s_iStandardWalk[5]` (0x63a9cc: 0,1607,704,2159,2161), `s_iStandardSprint[6]` (0x63a9e0) and `s_iStandardSprintJostle[4]` (0x63a9f8), indexed by `%5`, `%6` and `&3` in `GenerateSamples`. There is also `s_iWalkDirectionAnimLookUp[8]` (0x74532c).

---

## 2. Animation database (`animdb.adb`)

### 2.1 File format  [V] (`LoadAnimDB` @0x2a9235)
`u32 count`, then `count × 100-byte records`, then for each record whose `+0x28` / `+0x2c` fields are nonzero, in record order: `u32 byteSize + byteSize bytes` of s16 curve data.
* Records are copied into a temporary `s_pAnimEntries = new[count*0x68]` (@0x2a92b8). `+0x66` is set to **the loop index = anim ID** (@0x2a9404), so the ID is the record position.
* The curve blobs go into **one** `s_pS16Pool = new[2*Σ]` (@0x2a9462). Σ is `2*rec[+8]` for each `+0x28` blob plus `rec[+8]` for each `+0x2c` blob (@0x2a9424..0x2a943e). **The pool is sized from the declared `+8` value, not from the blob sizes.** A new record whose blob is larger than `+8` implies will overflow the heap.
* `count` is 2535 in the stock file (565,966 bytes).

### 2.2 Runtime `TAnimData` (0x84 bytes each)  [V]
`s_tAnimData = new[s_iAnimCount*0x84]` in `AnimDataListInit` @0x2a9dd6 (dynamic). It is filled by `AnimDataFill(id)` @0x2a98a5, which is called for every ID from `InitAnimations` @0x2a9df5. After that `s_pAnimEntries` is freed.

| TAnimData | From raw rec | Meaning |
|---|---|---|
| +0x08 s8 | +0x00 | **category = state list 0..19** |
| +0x09 s8 | +0x54 | loop mode (1→0, 2→-1, else 1) |
| +0x0c u32 | +0x0c, +0x02 bit0, +0x56 sign | flags (bit 1 is tested by the deek picker; 0x8000/0x4000 come from the +0x56 sign) |
| +0x10 | 0x10000/(+4·+6)·(+0x5c)/100 | playback rate |
| +0x14 | +0x60 | |
| +0x18 | +0x5e | stride (README: locomotion stride) |
| +0x1c / +0x1e | +0x58<<8 / +0x5a<<8 | |
| +0x20..+0x4c | +0x30/+0x38/+0x40/+0x48 | **up to 4 action/contact points** (time, dist, angle, height). Count at **+0x50** (0..4) |
| +0x54 / +0x58 | +0x28 / +0x2c | pointers into the s16 pool |
| +0x5c/+0x5e/+0x60 | +0x06/+0x08/+0x0a | |
| +0x64 | +0x16 | direction angle (<<9) |
| +0x66 | +0x20 | |
| +0x68/+0x6a | +0x1a<<3 / +0x1c<<3 | |
| +0x6c | +0x18 | |
| **+0x6e** | **+0x0e** | **sub-type / style** (deek style, preload key) |
| +0x70/+0x72 | +0x12<<9 / +0x14<<9 | |
| +0x74 | +0x0e | |
| +0x76 | +0x10 | mask used by `PreLoadAnims` |
| +0x78 | (+0x1e<<9)&0x3e00 | |
| +0x7a/+0x7b | +0x22 / +0x24 (bytes) | |

### 2.3 State / candidate lists  [V]
`CAnimManager::s_tStateList` is 0x778b10 in .bss, **20 × 24 bytes** (480 B). `s_iStateFlags[20]` is at 0x63a97c (values 1,0,1,1,1,0,6,0,0,0,6,6,6,6,0,0,0,0,0,10).
Each 24-byte entry: `+0 s16 flags`, `+2 s16 count`, `+8 u16* ids`, and `+0xc/+0xe/+0x10/+0x12/+0x14` min, max, avg and max action-point stats.
`StateInfoListFill` @0x2a96b9 loops `state=0..19` (`cmp sb,#0x14` @0x2a9864). For each state it counts the records with `category==state`, allocates `new[count*2]` **only if the pointer is still NULL** (@0x2a9762..0x2a9772), and stores the IDs. Records with category ≥ 20 are silently left out of every list; they can only be reached by an explicit `SetAnim(id)`.
`GetState(id)` @0x2a9e71 searches all 20 lists (`cmp r0,#0x13`) and reads the IDs with **`ldrsh`**, so IDs are effectively limited to **≤ 32767**.

Stock populations (category = list): 0 STAND 94, 1 CONTROL 351, 2 STOP 7, 3 SIDESTEP 36, 4 LOCO 78, 5 TURN 154, 6 STUMBLE 33, 7 THROW 5, 8 TACKLESLIDE 24, 9 TACKLE 10, 10 FALL 38, 11 GETUP 37, 12 KICK 604, 13 KICKSETPIECE 34, 14 NIS 749, 15 GK 216, 16 GK 12, 17 GK 10, 18 REFEREE 27, **19 DEEK 16**.

Selectors that read these lists, all via `StateInfoGet(player+0x4c)`: `SetAnimFromStateGen/GenMinMax/Action/Loco`, `SetAnimControl`, `SetAnimTurn`, `GA_SetAnimFromDeek`, `GA_SetAnimGKSave(Direct)`, `GL_GoalAnimCheckBounds`, `GL_GoalSetMultiCeleb`, `GC_OpenPlayControl`, `GC_DribblingControl`, `GC_MovementOffBall`, `ACT_TackleSetPlayerState` and `NewPlayerStateData(NIS)`. They are all **single-pass best-score scans with no fixed-size candidate buffers** [V: no indexed stack stores in any of them]. A list can therefore grow to the s16 count limit with no crash, at a linear CPU cost per selection.

### 2.4 Name tables (fixed 2535)  [V]
* `ANIM_sName` is 0x7424e8 (10140 B = 2535 pointers). It holds debug names; `catalog/animation_names.csv` comes from it.
* `ANIM_uHashName` is 0x776374 (2535 × u32 FTTHash). `CAnimManager::GetAnimID(const char*)` @0x2aa499 and `GetAnimID(unsigned)` @0x2aa4c9 scan it with `movw ip,#0x9e6` (@0x2aa4a4 / @0x2aa4ce) and return -1 when there is no match.
* `CNISInterfaceDebug::ms_bAnimsUsed[2535]` (0x7c1244) and `OutputAnimsUsed` (`movw r1,#0x9e7` @0x2d30d2) are debug only.
* NIS "no anim" sentinel: `0x9e7` in `CNISActionPlayAnim` ctor @0x2cc8c2, `CNISActionHappy` @0x2cba00 and `CNISActionSad` @0x2cd198. If IDs grow past 2535, the sentinel ID 2535 becomes a real clip. Move the sentinel, for example to 0x7fff, when adding clips.

### 2.5 NIS (celebrations and cut-scenes)  [V]
* There are **52 fixed scene folders** (`CNISInterface::ms_sXmlFolders`, 0x744fa8, 208 B). `CNISAct::LoadFileNames(...,0x34,...)` @0x2d0f74/0x2d103c enumerates **each folder with OpenDir** and stores the per-folder file count in a **u8**, so the limit is **≤255 XML variants per folder** (`strb` @0x2d416e/0x2d419a/0x2d4214). File names are free. The celebration folders are `goal/regular` (13 files) and `goal/big` (13).
* NIS memory pool: `NISMem_Init(0x32000)` @0x2d0f50 is a **fixed 200 KB** for one loaded scene [V value, G on what overflows].
* `NISAnimLists` (for example `goal/common/animlist.xml` with `GOALCELEB_STAND_ANIM_LIST`, `GOALCELEB_SLIDE_ANIM_LIST`, `TEAMCELEB_ANIM_LIST` and `UPSET_ANIM_LIST`) are parsed by `CNISAnimManager` @0x2d42a9. Arrays are allocated with `new[]` (@0x2d4334). `CNISAnimList(const char*, int*, u8 count)` @0x2d4277 stores a **u8 count, so ≤255 anims per list**. Each name goes through `GetAnimID(name)` @0x2d4404, and an unknown name logs "Invalid AnimName".
* `<AnimID>` in actions is stored as an FTTHash (`CNISActionPlayAnim` ctor @0x2cc900) and resolved through the same 2535-entry hash table. **NIS XML cannot reference new IDs** unless `GetAnimID` is extended.
* Goal celebration importance is `GL_GetGoalCelebType` @0x2bd48d and returns 0..3. Which XML file plays is picked from the folder file list [G on exact selection].

### 2.6 Replays  [V]
`CReplayFrame` stores per-player anim IDs as **u16** (`RC_UpdateAnimsPRE4000` @0x2ee26d: `+4` current, `+0xa` previous, 0x1c bytes per player, 0x408 per frame, 240 frames). The PRE4000/PRE5000 functions only remap old saved replays. IDs up to 65535 are fine.

---

## 3. Every cap, ranked

| # | Cap | Value | Where | Raise method | Difficulty |
|---|---|---|---|---|---|
| 1 | **Clip cache size** (`CAnimLib`) | **2535 clips** | object layout: `new(0x80d0)` @0x2fae96; 42 `movw` offset sites 0x2f850c..0x2f8994; loop `0x9e7` ×5; `0x51b1c` @0x2f86e4/0x2f86f4 | (a) Re-lay out by patching immediates. The layout is 13·N+~0x15 bytes, so a new `movw` value works for **N ≤ ~5030**; the `0x51b1c` bound becomes `N*0x84` via the movw/movt pair. (b) Hook the 9 `CAnimLib` methods through modcore into MODDATA arrays sized at runtime from `CAnimManager::s_iAnimCount` | Medium (a) / Medium-high (b) |
| 2 | Name→ID lookup table | 2535 hashes | `ANIM_uHashName` 0x776374; `GetAnimID` @0x2aa499/0x2aa4c9 (`#0x9e6` @0x2aa4a4/0x2aa4ce) | Hook both `GetAnimID` overloads: after the stock scan fails, search a mod-owned hash table for IDs ≥2535. Alternatively repoint the GOT slot 0x731680 to a MODDATA copy and patch the two bounds | Easy-medium |
| 3 | NIS/none sentinel | ID 2535 (0x9e7) | @0x2cc8c2, 0x2cba00, 0x2cd198 (+ debug 0x2d30d2) | Patch to 0x7fff or another unused value when count > 2535 | Easy |
| 4 | State/candidate list count | **20 states** | `StateInfoListFill` `cmp #0x14` @0x2a9864; `GetState` `cmp #0x13` @0x2a9eaa; .bss 0x778b10 (480 B); `s_iStateFlags[20]` | Not needed to add clips. A new state also needs new CPlayer state code; relocating `s_tStateList` to MODDATA plus 3 bounds is possible | Hard (and rarely useful) |
| 5 | IDs per list / ID width | count s16 (32767); IDs read with `ldrsh`, so ≤32767 | `GetState` @0x2a9e9c, `GA_SetAnimFromDeek` @0x2e4366 | None needed | n/a |
| 6 | Frames per clip | **127** (s8) | `SAT_LoadAnimation` @0x2f7482, `SAT_CreateSamples` @0x2f7da0 | Use a larger sample interval (hdr `+2`) for long moves. True fix: rewrite the sampler | Workaround easy / fix hard |
| 7 | Skeleton | **42 bones**, 42 transform slots, 45 position slots | `bone_remap` 0x745138; `SAT_BlendSamples(…,0x2a)` @0x2fca96; SAT hdr maps | Format-level; not raisable without new model/sampler code | Very hard |
| 8 | Action/contact points per anim | **4** | adb raw +0x30/+0x38/+0x40/+0x48 → TAnimData +0x20..+0x4c, count +0x50 | Fixed record format | Hard |
| 9 | Blend slots per player | **2** (current + previous) | CPlayer +0x54/+0x58, +0x60/+0x64, weight +0x6e | Engine redesign | Very hard |
| 10 | Mirror clips | must be ID = source+1 | `LoadAnim` @0x2f861e | Keep pairs adjacent when appending | Easy (rule) |
| 11 | Curve-pool sizing | from rec `+8`, not blob size | `LoadAnimDB` @0x2a9424..0x2a9462 | Keep `+8` consistent with the blob, or hook to size from real sizes | Easy (rule) |
| 12 | NIS XML per folder | 255 (u8) | `CNISAct::LoadFileNames` @0x2d416e | Enough (13 are used) | n/a |
| 13 | Anims per NIS AnimList | 255 (u8) | `CNISAnimList` ctor @0x2d4277 | Enough | n/a |
| 14 | NIS scene memory | 0x32000 B | `NISMem_Init` @0x2d0f50 | Patch the immediate (`mov.w r0,#0x32000`) | Easy [G on side effects] |
| 15 | NIS folder types | 52 | `0x34` @0x2d0f72/0x2d103a; `ms_sXmlFolders` | Adding a new NIS *type* needs new code | Hard |
| 16 | Deek (skill move) styles requested | **0,1,2 only** (style 3 never requested) | `GC_SpecialMoveDribbling` @0x2e40d1 (`Random(3)` @0x2e415a) | Data: set adb `+0x0e` of the clip to 0/1/2. Code: `Random(4)`. The v9 mod patches 0x2e4156/0x2e416e nearby, so coordinate with it | Easy |
| 17 | Deek gates | cooldown `tGame+0xa650 ≥ 0x79` (121 ticks); `player+0x13c ≤ 0x1d`; ball height `cBall+0xc ≤ 0x1ec7`; `\|vz\| ≤ 0xc3`; heading window ±0x600 in `GA_SetAnimFromDeek` @0x2e4412 | same function | Patch the immediates | Easy |
| 18 | Dynamic clip budget | 0x60000 trigger / 0x30000 target | `GetAnim` @0x2f8906, `FreeDynamicAnims` @0x2f875e | Inactive (all clips permanent) | n/a |
| 19 | KPX members | none (u32, dynamic tables) | `CFTTFileSystem_PAK::Initialise` | n/a | n/a |
| 20 | Clip memory | about 17 MB resident, general heap | `CAnimLib` ctor | Every added clip costs RAM for its whole life (no pool limit) [G on device headroom] | n/a |
| 21 | Replay anim IDs | u16 | `CReplayFrame` | n/a | n/a |
| 22 | Hard-coded standard loco IDs | walk[5], sprint[6], jostle[4], walk-dir[8] | 0x63a9cc / 0x63a9e0 / 0x63a9f8 / 0x74532c (.rodata/.data) | Edit the table values in place (same length) | Easy |

---

## 4. Adding a new animation end to end

1. **Clip:** write `NNNN.sat` (N ≥ 2535) with ≤127 frames, 42-slot transform map and 45-slot position map, following the `ANIMATION_LAYOUT.md` format. For a mirrored variant, add a 104-byte header-only clip with `+5 ≠ 0` at N+1.
2. **pak:** append the member(s) to `anims.pak`. Rewrite the file table and names, bump `folder[0].file_count` and `header.files`, and fix every offset. No native limit applies.
3. **adb:** increase `count`, **append** the 100-byte record (IDs are positional) and put any curve blobs **after all records**, in record order. Set `+0x00` category to the target state (0..19) and `+0x0e` sub-type/style, `+0x04/+0x06` frame/interval fields, `+0x16` direction and so on. Copy the other fields from a similar stock record. Keep `+0x08` consistent with the blob sizes (pool sizing).
4. **Native caps that block it:** #1 `CAnimLib` (**mandatory**; without it, ID 2535+ corrupts memory or is never loaded). If NIS will use it: #2 `GetAnimID` and #3 the sentinel.
5. **Referencing it:**
   * Gameplay states: nothing to do. `StateInfoListFill` adds it to list `category` and the state's selector scores it along with the stock clips. The selector filters must match (sub-type `+0x6e`, direction `+0x64`, action points, flags). For skill moves: category 19 with style 0/1/2 in `+0x0e`.
   * NIS/celebrations: add the name to an AnimList, for example `goal/common/animlist.xml` → `GOALCELEB_STAND_ANIM_LIST`, or an `<AnimID>` in a `goal/*/goal_*.xml`. This needs #2, because the name has no hash in `ANIM_uHashName`.
   * Hard-coded use (for example replacing a standard walk): edit the `s_iStandard*` tables.

### Concrete ways to raise #1
* **Immediate relayout (no new code).** For N clips: ptrs@0, `state`@4N, `count`@align4(5N), `bytes`@+4, `lastUsed`@+4N, then the 6 trailing words. Patch the 42 `movw` sites, the five `0x9e7` sites, the `0x51b1c` movw/movt pair, and `new(0x80d0)` @0x2fae96. All are `movw` with 16-bit immediates, which caps N at about 5030. The ctor then loads 0..N-1, so **every ID up to N-1 must exist** in the pak.
* **Hook approach (modcore/CAVE3, `mod/modcore_build.py` + `mod/elf_extend.py`).** Hook `CAnimLib::LoadAnim/GetAnim/CheckAnim/FreeAnim/FreeDynamicAnims/PreLoadAnims`. For `id < 2535`, fall through to stock. For `id ≥ 2535`, serve from MODDATA arrays sized from `CAnimManager::s_iAnimCount` (GOT 0x731690) and load with `SAT_LoadAnimation` @0x2f73d5 while `g_pFTTFileSystem` is swapped to `this+0x80cc`. Call the loader for the extra IDs once after the `CAnimLib` ctor, from a hook at the end of `CGfxCharacter::Init` @0x2faea6. This leaves the stock layout untouched.

## 5. Present but disabled/locked, and how to unlock

| What | IDs | Gate | Unlock |
|---|---|---|---|
| Stand-to-45° left-foot stepover (skill move) | 69, 70 (mirror) | DEEK style `+0x0e = 3`. The only caller (`GC_SpecialMoveDribbling` → `NewPlayerStateXDeek` @0x2e1e35 → `GA_SetAnimFromDeek` @0x2e425d) only passes styles 0/1/2 (@0x2e414a..0x2e417c), and the style filter is at @0x2e43e4 [V]. Flag bit 1 also prefers a standing-ball state [G] | Set adb record 69 and 70 `+0x0e` to 2 (data only), or make the picker's `Random(3)` a `Random(4)` for high-control players |
| Skill-move-like clips not in DEEK | 2333/2334 `SKILL_MOVE_5_NOEL_KICK`, 1324/1325 `FT_DEEK_0_ANDY_CTRL`, rabona 1895/1896, flicks 1889-1894 | They live in KICK (12) / CONTROL (1) lists and are picked only by those selectors' filters [V category; G usage] | Change category `+0x00` to 19 and set a style (keep `+0x0e` meaningful). Needs matching action-point data, or the move will not track the ball |
| Unreferenced NIS/celebration clips | 396 category-14 IDs not referenced by any of the 199 NIS XMLs (166 non-mirror). Examples: 839 CELEB_VIOLIN, 840 CELEB_ARMS_WIDE, 845 CELEB_SALUTE, 955/957 SHINE_BOOTS, 490 TAKE_A_BOW, 451 RUB_HANDS, 1946/1948/1950/2010/2012 dance sets, 1952/1954 covering face, 2464-2472 and 2489-2495 excited/standing, 1988/1989 and 2507/2509 two-man | Only NIS XML names select them. None is referenced as a code immediate in a spot check (0x347/0x34d), but code refs were not ruled out exhaustively [G] | **Data only:** add the names to `GOALCELEB_STAND_ANIM_LIST` / `TEAMCELEB_ANIM_LIST` in `nis.pak:goal/common/animlist.xml`, or to `goal_*` XMLs (≤255 per list). They are already in the hash table |
| Records with category ≥ 20 | none in stock | never enter a list | n/a |

Full list of unreferenced NIS clips: `F:\android modding\tmp\anim_caps\nis_cov.txt`.
