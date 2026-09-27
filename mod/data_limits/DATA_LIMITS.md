# DLS18 (libDLS18.so v5.064, armeabi-v7a) - hard capacity limits for modding

Static analysis only (no device run). VAs are library VAs. [V] = verified in disassembly this pass,
[D] = taken from existing docs (ROSTER_MECHANICS / STOCK_TRANSFER_FLOW / UI_COMPONENTS), [G] = inference.
Tools used: `analysis/armdis.py`, `analysis/xref.py`, `tmp/econ/gdis.py`. Scratch dumps: `F:\android modding\tmp\caps\d*.txt`.

General pattern: **the database (players/teams/links) is heap-allocated from file counts - no fixed table cap.**
The real walls are (a) 16-bit ids, (b) fixed per-team arrays of 32, (c) fixed save-format arrays, (d) small UI arrays.

---

## 1. Players

| Limit | Value | Where | Notes |
|---|---|---|---|
| ROM player count | 5816 records (players.dat v5060 rev 8), heap-sized | `CDataBase::LoadPlayers` 0x20bc98: `UncompressFile` -> `db+0x30` (buffer), `db+0x44 = hdr[2]` (count), `db+0x10 = hdr[0]` (version). Loop 0x20bd2c..0x20bd48 computes `db+0x8c` = max u16 id (GetMaxPlayerID 0x20cbc0). [V] | No compile-time cap. Adding records = rebuild players.dat with count in header; records must stay **sorted by u16 id** (LoadPlayerROM 0x20c710 binary-searches id at +0, stride 0xB4, base+0xC). |
| Player id space | u16, 1..0xFFDE for DB players (current max 16316) | TPlayerInfo+0 u16; `ldrh` everywhere; link ids stored int32 but only low 16 bits used [D]. 0xFFDE is used as "none" sentinel in lineup / stats serialize (0x2f02ea, 0x36c4ce) [V]. 0xFFDF..0xFFFE = created players, 0xFFFF = invalid. | Effective DB id ceiling 0xFFDD (~65.5k). Cannot be raised without rewriting every u16 load (hundreds of sites) - treat as fixed. |
| TPlayerROM / TPlayerInfo size | ROM 0xB4 (180 B), Info 0xB0 (176 B); ROM_V6 (players_3050) 0xB8 | LoadPlayers stride `adds r2,#0xb4` 0x20bd38; CCreatePlayer copies 0xB0 + 4-byte level (0x205c0a). [V] | Fixed layout; adding fields = new struct everywhere. Use spare/unused bytes instead. |
| Created-player id range | 0xFFFE down to 0xFFDF => **32 ids** | `CCreatePlayer::GetNextAvailableCreatedPlayerID` 0x205c68: starts 0xFFFE (`movw r0,#0xfffe` 0x205c7c), decrements while id is taken, returns -1 below 0xFFDF (`movw ip,#0xffdf` 0x205c6c, `cmp r0,ip` 0x205c9c). [V] | Hard max 32 created players (also bounded by u8 count). |
| Created-player count | u8 `CCreatePlayer::ms_uCreatedPlayerCount` 0x75c388; array `ms_pCreatedPlayers` 0x75c38c (heap, grows by realloc of (n+1)*0xB4 in AddPlayer 0x205bb4) [V] | Serialized as u8 (CCreatePlayer::Serialize 0x205918, version gate 0x94). | To allow >32: lower the floor constant 0xFFDF at 0x205c6c (movw imm) - but the range below 0xFFDF collides with the 0xFFDE sentinel and real DB ids; not recommended. u8 caps at 255 anyway. |
| CanAddCreatedPlayer | user squad count <= 31 | 0x210318: `cmp r1,#0x1f; movgt r0,#0` 0x21032e [V] | Same 32-squad wall. |
| Per-team squad (TTeamPlayerLink) | **32** | Link struct 0x108 B: +0 team id, +4 count, +8 spec[32] x4, +0x88 id[32] x4 (GetTeamLink stride `r*33*8` 0x209378) [V]. `AddPlayerToLink` silently no-ops when count>31 (0x20a42e) [D]; `CanAddPlayer` 0x2102d4 returns 0 when count>31 [D]; `CanAddCreatedPlayer` 0x21032e [V]. | See section 1.1 for what must change together. |
| Minimum squad | 16 (CanRemovePlayer refuses if count < 17) + last GK | `CTransfers::CanRemovePlayer` 0x213070 [D] | Patch compare immediate (`cmp rX,#0x11`) inside 0x213070 to lower. Low risk (AI may field fewer subs). |
| Season player-state array | 32 x CSeasonPlayerState (stride 10) | `CTeamManagement::Serialize` 0x2f1f30 loop to 0x140 (0x2f1f44) [V] | Indexed parallel to squad slot. |
| Lineup id array | 32 x u16 (CTeamLineup+0..0x3f) | `CTeamLineup::Serialize` 0x2f0298 loop `cmp r7,#0x40` 0x2f02c8 [V] | XI = 11 slots, bench = 7 (slots 11..17), rest = reserves [D]. `SelectStartingEleven` uses `bool used[32]` on stack (0x2f085c) [D]. |
| Starting XI / bench | 11 / 7 | Formation table `FS_iFormationPlayerPos[formation*0x2c + j*4]` (11 ints per formation) [D]; match engine 11-a-side | Bench 7 is match-engine/UI coupled; do not change. |
| Player-development records | **64** (`ms_iPlayerCount` 0x75cca8 > 63 -> replace path) | `CPlayerDevelopment::AddPlayer` 0x20e130: `cmp r0,#0x3f; bgt 0x20e2ac` at 0x20e1b4; array `ms_pPlayerDevStats` heap (n+1)*0x20 + 8-byte header [V]. Serialize 0x20ef40 (`cmp r5,#0x40` for save ver < 0x68, 0x20f056) [V]. | Raise: change `cmp r0,#0x3f` imm (0x20e1b4, T1 `cmp rN,#imm8` -> up to 255). Storage is dynamic and count is serialized as int, so safe up to 255 with a 1-byte patch. Check the 0x20e2ac replacement branch semantics before relying on it. |
| Transfer search results | heap, = `ms_iValidPlayerCount` 0x75ce48 entries of TPlayerSearchInfo 0xB0 | `CTransfers::Search` 0x210a00 allocs `count*0xb0` via `_Znaj` (0x210a6c/0x210b4c/0x210c58/0x210f90); +0x34 of TAsyncPlayerSearchInfo is a 0..100 progress % (Clamp 0x210e4a), not a cap. [V] | No fixed cap. Sorted index arrays (forename/surname/nickname/overall/sort) also heap (CreateSortedArrays 0x211698). |
| Simple links | heap, `db+0x98` ptr / `db+0x9c` count; CTransfers copy `ms_pSimpleLinkRandomIndicies` 0x75cfdc = new int[count] | `GetActiveSimpleLinks` 0x2092ec, `SetupPlayerArrays` 0x211518 [V] | No fixed cap. |

### 1.1 Raising the 32-player squad cap (hard, high risk)
Every one of these is sized 32 and must grow together:
- `TTeamPlayerLink` 0x108 B (spec[32] at +8, id[32] at +0x88). Stride 0x108 is hard-coded in `GetTeamLink`
  (`add r6,r5,r5,lsl#5` then `lsl#3`, 0x209378/0x209396) and in link loaders/PopulateDefaultLinksArray, CalculateLinks,
  SetTeamLink, GetTeamSpecificData (0x20a620 uses `link+0x88` and `-0x80`).
- `cmp #0x1f` checks: AddPlayerToLink 0x20a42e, CanAddPlayer 0x2102d4, CanAddCreatedPlayer 0x21032e, mod `commit_transfer`.
- `CTeamLineup` id[32] (0x40 B), `CTeamManagement` player-state[32] (0x140 B), `bool used[32]` in SelectStartingEleven,
  `CTeam+0x148` squad byte, CTeam player array (CTeam is 0x1018 B, `PlayersLoad` 0x20baa0).
- Save format: `TTeamPlayerLink::Serialize` 0x207f06 writes 32+32 entries; lineup/team management serialize 32 -> needs a save-version bump
  or old saves break.
- UI squad screens assume <= 32 cards.
Recommendation: do **not** raise; keep 32 and manage rosters instead. A realistic alternative is a mod-side "reserve pool"
(players kept outside the link, re-added on demand).

---

## 2. Teams, leagues, links

| Limit | Value | Where | Notes |
|---|---|---|---|
| Team records | 232 (teams.dat v5060 rev 9), heap `CTeam[count]` (0x1018 B each) | `CDataBase::LoadTeams` 0x208c30: count -> `db+0x40` (GetTeamCount 0x20beb8), `new CTeam[]`, copies 0xFFC-byte TTeamROM (0x208da0 `movw r2,#0xffc`), `TTeamROMtoTTeam` [V] | No fixed cap. Team ROM record 4092 B. |
| Team id space | int in code, u16 in save tables (league table ids initialised 0xFFFF, 0x3616ec); stock max id 571; 0x102 (258) = user/dream team | [V] | Keep new ids < 0xFFFF and != 0x102. |
| Team links | 233 (teamplayerlinks_0 header), heap `db+0x28`/count `db+0x38`; override table `db+0x1c/0x20` | GetTeamLink 0x20934c binary search on team id (links must be **sorted by team id**) [V] | No cap. |
| CConfig custom links | heap, 12-byte `{pid, src, dst}` records, count from XML `<Link>` node count | `CLinksInfo::LoadInfo` 0x2007c4 (`new[count*0xc]` 0x20083c) ; `GetCustomLinkCount` 0x201510 [V] | No cap; each still subject to 16..32 squad rules. |
| Teams per league table | u8 count (serialized `SerializeInternal<u8>` 0x3616bc/0x3616cc) => max 255; stock 16 per league | `CLeagueTable::Serialize` 0x3616ac [V] | 16 is a data/schedule choice; see schedule below. |
| League trees | 6 (tree 0 Elite .. 5 Academy) | per-tree cup-finish arrays of 6 u8 (`cmp r7,#6` 0x36c54a/0x36c562) in `CSeasonAllTimeStats::Serialize` [V]; schedule trees [D] | Adding a 7th tree needs new schedule tables + all-time stat arrays + save bump. Hard. |
| Tournament team counts | u8 per tournament (CTournament::Serialize 0x35fde0 u8 fields); Elite Cup 32 teams, GC cup 8 rounds [D] | [V] | |
| Season schedule | **104** `TTurnInfo` (16 B) = 0x680 bytes at CSeason+8; turn index **u8** at +0x680 | `CSeasonSchedule::Serialize` 0x35e592 loop `cmp r6,#0x680` 0x35e5b2 [V] | League rounds = 15 for 16 teams, placed at even slots 18..46 [D]. A 20-team league (19 rounds, 38 slots if double) would need re-planned slot map; 104 is enough for single round robin of up to ~20 teams if cups are moved. Enlarging 104 changes CSeason layout (every field after +0x688 moves) - effectively impossible. |
| Per-turn match score info | 104 x TSeasonMatchScoreInfoBasic (10 B) at CSeason+0x3604 | CSeason::Serialize loop 0x36bc3c `cmp r6,#0x410` [V] | Parallel to schedule. |

---

## 3. Career / profile / save

| Limit | Value | Where | Notes |
|---|---|---|---|
| Save buffer | **none fixed**: `CFTTSaveFile::BeginSave` 0x3ff264 creates `CFTTFile_RAM(0x4000 initial)`; `CFTTFile_RAM::Write` 0x3fba52 grows it via `CFTTMem::Reallocate_Internal` [V] | Profile written by `CMyProfile::Save` 0x375b88 (order: header ints, CCreatePlayer, DreamTeam link, PlayerDevelopment, PreTrainedPlayers, CSeason, GameSettings, Stats, Achievements, Unlockables, DLO, MP memory, tutorial, credits, analytics, ..., IAP) [V] | Size is not the problem; the **versioned format** is: every serializer gates fields on `ser+0x18` (save version), thresholds up to 0xAE (e.g. 0x36ce0a). Adding fields = new version + gated read. Recommended: keep mod data in a separate file. |
| CMyProfile object | ~0x2A9xx bytes, static | `MP_cMyProfile` global; Save touches offsets up to +0x2a8b0 [V] | Fixed layout; can't append fields. Use mod-owned storage. |
| Season schedule / match history | 104 turns, u8 index | section 2 | |
| Season player state | 32 | section 1 | |
| Player-development records | 64 | section 1 (patch `cmp #0x3f` at 0x20e1b4) | |
| Created players | 32 ids / u8 count | section 1 | |
| Achievements | **63** bools (older saves 59/60) | `CProfileAchievements::Serialize` 0x3781d0: loops to 0x3b (ver <= 0x5f), 0x3c (ver < 0x6a), 0x3f (current, `cmp r6,#0x3f` 0x37824e) [V] | Fixed array in CMyProfile (+0x224b4). Adding achievements needs a new array and save version, plus platform ids. Not practical. |
| POTW (player of the week) roster | **16** entries x 12 B (`ms_tPOTWPlayers` 0x75ccc4, 0xC0 B) | `CPlayerDevelopment::LoadPOTWConfigInfo` 0x20e428 stops when `ms_iNumPOTWPlayers > 15` (`cmp r0,#0xf` 0x20e67a) [V] | To raise: move the array into a mod buffer (patch the GOT/literal refs to 0x75ccc4 in the POTW functions) and raise imm 0xf. Medium. |
| Scouting info | 16 x u16 (last session) + 64 x u16 (history) + u8 | `CScoutingInfo::Serialize` 0x36d08c, loops `cmp r7,#0x20` 0x36d0c6 and `cmp r7,#0x80` 0x36d0fc [V] | Players per session is also bounded by CConfig var 0x172 and the scout box's 64-slot card array [D]. |
| Ticker items | u8 count, heap | `CTickerItemList::Serialize` 0x2435bc [V] | <= 255. |
| League table / tournament team counts | u8 | 0x3616ac / 0x35fde0 [V] | <= 255 teams per competition (save format). |
| Custom dream team kit data | 4 x 0x58-byte u8 blocks + 10-u32 colour blocks | `CCustomDreamTeamData::Serialize` 0x207508 (`cmp r6,#0x58` 0x20761e.., `cmp r5,#0x28` 0x20773c) [V] | Fixed; user team only. |
| Stadium config | `TEnvConfig` 0xEBC B (`CFESDreamLeagueStadium::ms_tConfig` 0x762cb0) | symbol size [V] | Fixed struct. |

---

## 4. UI

| Limit | Value | Where | Notes / raise |
|---|---|---|---|
| Message box queue | **4** | `CFEMessageBoxQueue+0xe8..+0xf4`; `AddMessage` 0x24fd96 loop `cmp r7,#3` 0x24fdce; when full it drops the lowest-priority box [V][D] | Part of the object layout. Workaround: open the next box from the previous box's callback; never queue more than 2 mod boxes. |
| Option buttons per message box | **12** (`+0x418..+0x444`) | CFEMessageBoxOptions fields [D] | Fixed; use a settings table or several pages instead. |
| Setting-cell options | 255 (u8 count) | `CFETableSettingCellInt`, `InitOptions` 0x257640 [D] | |
| Box title | 0x100 u16 (256 incl. NUL), copied to `this+0xfc` | `CFEArea::SetTitle` 0x21766a [D] | The description is heap-copied, so no fixed limit [D]. |
| Keyboard text | 512 u16 (`box+0x8dc`); JNI GetText static buffer 0x400 B at 0x75f168 | `CFEMsgKeyboard` 0x24d784, `CFETextField::GetText` 0x241fb4 [D] | The maxChars argument limits it further. |
| Profile description | 0x100 u16 -> 0x100 bytes UTF-8 | `CMyProfile::GetDescription(buf,0x100)` 0x375f6e [V] | |
| Post-match reward table | **12 rows** x 0x208 B (`ms_tCreditAwardInfo` 0x75d7d4, size 0x1860 = 12*0x208); count in `ms_iCreditAwardCount` 0x75f034, which sits right after the array | `CFEPostMatchCreditAwards::SetupCreditAwardInfo` 0x239c58 [V size]; no explicit bound check seen, rows are appended per award source [G] | The screen shows about 10 rows comfortably [G]. An extra row type can overflow into the count variable. To add rows, move the array to a mod buffer (all refs go through the symbol's GOT slot) or reuse an existing row. |
| Scout results grid | 64 card + 64 coin-button slots (`this+0x4ec[64]`, `+0x5ec[64]`) | `CFEMsgScoutResults` 0x250558; Process polls all 64 [D] | Stock count = CConfig var 0x172. |
| Transfer search grid | dynamic (CFELayoutGrid; rows = results / column count) | `CFESDreamLeagueTransfers::SetupResults` 0x276bb4 [V partial] | No hard cap found; the data array is on the heap (section 1). |
| LOC strings | dynamic: `LOCstring(i)` 0x381aec returns "" when `i >= db+0x14` (count comes from the .xlc) [V] | 3 text databases (`new[0xec]` = 3 x CFTTLangDatabase 0x4c at 0x3818a4) loaded from `PKG:Data/Text/%s.xlc` (ftslang, ftsteamnames, ftscredits) [V] | Add strings by appending to ftslang.xlc (ids are indices); no code change needed. Keep ids the same across languages. |
| Fonts | gamefont*.fnt with one texture page each (`_00.ftc`); `FTTFont_GetNumTexturePages` 0x38f494 | [G] | Glyphs missing from the atlas render blank; new glyphs need a rebuilt atlas. U+0180 = coin glyph [D]. |

---

## 5. Kits / logos / customisation / graphics

| Item | Count / size | Where | Raise |
|---|---|---|---|
| TKitInfo (per kit) | 10 x u32 colours (0x28 B) + 2 x u8 style ids | `TKitInfo::Serialize` 0x2b3276 (`cmp r6,#0x28`) [V] | Style ids are u8, so up to 255 patterns if the textures exist. |
| Kit pattern masks | 16 (`kittemplate_mask_{check,dsplit,gradient,gradient2,halves,hband,hoops,plain,quarts,sash,sides,sleeves,stripes,topband,topdiagonal,vband}`) | assets `player/kits/templates` [V files]; name table used by GetKitTemplate 0x303968 [G] | Adding a pattern means extending the name table (string pointer array) and the editor UI list. Medium. |
| Kit trims | 4 (`kittemplate_trim_%i.png`, 0..3) | string 0x606ff9, used in GetKitTemplate 0x303a0a [V] | The index is formatted with %i, so more files work if the index source allows it (UI list bound [G]). |
| Kit template cache | 16 entries x 16 B (`CGfxKits::ms_tCachedKitTemplates` 0x81db30; LRU memmove 0xF0 at 0x303a7a) [V] | Only a cache; more templates just evict older ones. |
| Sponsors | 20 (`sponsor%i.png`, index = team byte - 1; `sponsor_dls` for team 0x102 / special ids) | `CGfxKits::LoadKitExtraTextures` 0x3038fe..0x30391e [V] | sprintf index with no bound in code: add sponsor20+.png and set the team byte (u8, max 255). Low risk. |
| Boots | **12** textures (`s_iBootsTextures` 0x7c46fc, 12 ints) | `CGfxCharacter::LoadDefaultModels` loop `cmp r4,#0xc` 0x2fb2a2 [V] | The next symbol starts at +0x30, so raising it means moving the array and patching the loop imm. Medium. |
| Skin / gloves textures | 6 skin (`cmp r4,#6` 0x2fb220, 0x18 B), 5 gloves (`cmp r4,#5` 0x2fb262, 0x14 B) [V] | Same approach as boots. |
| Head models | `s_pDefaultHeadModel` 0x7c41c0, 0x480 B (288 pointers) [V size] | Fixed static array. |
| Built-in logo generator | 10 shapes x 3 variants (`fe/teams/logos/logo_{1..10}{a,b,c}_mask.ftc`) [V files] | Procedural logo masks. |
| Custom image download | max dimension 0x200 (512 px) for all 7 ECustomDownloadType values | `CCustomData::GetImageMinMaxDimensions` 0x206d98 (`cmp r0,#7`, `mov.w r4,#0x200` at 0x206da4) [V] | Patch the immediate at 0x206da4 for bigger images; texture memory grows 4x per doubling. |
| Logo target size | `CGfxTeamLogo::ms_iTargetSize` 0x745b2c (.data) | [V symbol] | A data variable, patchable in .data. |
| CCustomLogoInfo / CCustomTextInfo / music list | heap, from server/config XML | 0x1ffa98 / 0x1ffba0 [V] | No cap. |
| League sleeve badges | 13 files (`league0..5` + cups) | assets `player/kits/league` [V] | Tied to ETournamentID and the 6 trees. |
| Stadium | one config (`TEnvConfig` 0xEBC); models loaded by name `ENV:/stadium/%s` | `CGfxEnv::StadiumConfigLoad` 0x32d158 [V] | |

---

## 6. Other small fixed constants a modder will hit

- **Squad 16..32, XI 11, bench 7** (sections 1 and 1.1): the main roster limit.
- **Created players: 32 ids** (0xFFDF..0xFFFE).
- **u16 player ids** in the lineup (0xFFDE = empty), scouting, POTW and league tables (0xFFFF = empty team). New DB ids must stay below 0xFFDE.
- **Sorted files are required**: players.dat sorted by id (binary search 0x20c710) and links sorted by team id (binary search 0x20934c). An unsorted append makes the record silently "missing".
- **User team id 0x102** is hard-coded (SerializeDreamTeam, LoadKitExtraTextures 0x3038fe, CustomDreamTeamData). Never give a DB team id 258.
- **6 league trees x 16 teams, 104-turn schedule**: changing league sizes needs a new schedule generator, not a constant patch.
- **CConfig vars** (e.g. 0x172 players per scouting session) come from the config XML and are the cheapest knobs (see ECONOMY_HOOKS.md).
- **Heap DB tables** (players, teams, links, custom links, search arrays, simple links, text) grow by editing the data files; no code change needed.

---

## 7. Ranked summary

| # | Limit | Value | Raise method | Difficulty / risk |
|---|---|---|---|---|
| 1 | Squad size per team | **user 64 (squad_caps.py, `--squad-max 32..64`), AI 32** (min 16) | v33: stock structures stay a 32-player view; user players 33..64 live in MODDATA overflow tables (link, season lineup, season states) behind 44 hooks + 24 site patches; save version 0xB6. Min: `--min-squad 11..31`. The market bridge caps the user at 62 when the hook is present | Done (v33, not device-tested) |
| 2 | Created players | **255 ids (created_caps.py, `--created-max 32..255`)**, u8 count | v33: ids moved to 0xFEDF..0xFFDD (LO = 0xFFDE-N, check `(id-LO) < N`); still bounded by the squad cap | Done (v33, not device-tested) |
| 3 | Player-development records | 64 | Patch `cmp r0,#0x3f` at 0x20e1b4 (<= 255); storage and count are dynamic | Easy / low-medium |
| 4 | Message box queue | 4 | Not practical (object layout); chain boxes instead | Design around it |
| 5 | Season schedule | 104 turns, 16-team leagues, 6 trees | Re-plan the slot map inside 104; enlarging is not feasible | Hard / high |
| 6 | Post-match reward rows | 12 (0x1860 array) | **Not raised (limits_v32).** All refs use GOT 0x730bac, but the consumers are fixed-size too: Reset/Setup memclr 0x1860 (0x239c38, 0x239c62), CFEMsgAchievements::GetNumberOfRows fills a 12-int (0x30 B) table (0x249ad2), CFESMatchSummary builds a 10-row CFERewardTable (0x27aed0). Stock code appends at most 9 award types + the bonus row, so 12 is never reached. Reuse a row instead | Medium-hard / medium |
| 7 | POTW roster | ~~16~~ **64 (limits_v32)** | Done: `build_mod.py --potw-max 16..255` (default 64). The only reference path is GOT 0x730790, which becomes R_ARM_RELATIVE -> MODDATA+0x8000. Also patched: init loop 0x20e462, `cmp #0xf` 0x20e67a, `cmp #0x10` 0x20e748, and CFEMsgPOTW::Init 0x24ebb2 (in-object 16-entry copy now stops at 16). The saved CSeasonPOTWInfo pool size is a u8, so total config weight must stay <= 255 | Done / low |
| 8 | Achievements | 63 | Save format + platform ids | Impractical |
| 9 | Boots / skin / gloves textures | 12 / 6 / 5 | **Not raised (limits_v32).** Refs are clean (GOTs 0x7319b4 / 0x7319ec / 0x7319a8), but the arrays are not the limit: the selectors are hard-coded (gloves `cmp #5` + `XSYS_Random(5)` 0x2fa598/0x2fa5ac; boots `Random(12)` + `Clamp(0,12)` 0x2faa48/0x2faa62/0x2faa6a and SetBootsColour 0x2fbae8), skin names come from a fixed 6-pointer table, and the extra texture files do not exist. Needs new assets + selector patches first | Medium / medium |
| 10 | Custom image size | 512 px | Patch `mov.w r4,#0x200` at 0x206da4 | Easy / low (memory) |
| 11 | Option buttons per box | 12 | Use a settings table (255 options) instead | n/a |
| 12 | Box title | 256 u16 | Fixed; the description has no limit | n/a |
| 13 | Player id space | u16, DB < 0xFFDE (stock max 16316) | Fixed | About 49k spare ids, enough |
| 14 | Players / teams / links / custom links / LOC strings | heap, sized from file counts (5816 / 232 / 233 / XML / xlc) | Edit the data files (keep them sorted) | Easy / low |
| 15 | Sponsors | 20 files, u8 index | Add `sponsorN.png` files | Easy / low |
| 16 | Save size | growable RAM file | Prefer a separate mod save file over changing the format | Easy with a separate file |
