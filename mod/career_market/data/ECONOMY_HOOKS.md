# DLS18 career economy: hook points, data sources, coin sinks (static analysis, libDLS18.so v5.064 armeabi-v7a)

Static analysis only; nothing here was run on the device. All addresses are stock-lib VAs (Thumb code: call `base + addr | 1`;
PLT stubs are ARM: even address). Tags: **[V]** = verified by disassembly, **[I]** = inferred from surrounding code / names,
**[G]** = guess. Companion docs: `STOCK_TRANSFER_FLOW.md` (sections 5/6), `ROSTER_MECHANICS.md`, `mod/PATCHING_RULES.md`.
Scratch tools used: `F:\android modding\tmp\econ\gdis.py` (GOT/literal-resolving disassembler), `refs.py` (PC-relative data
xrefs), decoded bundled config `tmp\econ\dls_config.xml`, CConfig var dump `tmp\econ\vars.txt`.

Key globals (data VAs, add `base`):

| symbol | VA | GOT slot | note |
|---|---|---|---|
| `MP_cMyProfile` (object) | 0x84a260 | 0x73052c | CSeason = profile+0x14; coins = profile+0x2a7cc; pending match coins = profile+0x2a7e8 |
| `CMatchSetup::ms_tInfo` | 0x7c2aac | 0x7306c0 | size 0xfc8 |
| `tGame` | 0x78c5f8 | 0x73063c | size 0xa9d8 |
| `CFEPostMatchCreditAwards::ms_tCreditAwardInfo` | 0x75d7d4 | 0x730bac | 12 rows x 0x208 = 0x1860 |
| `CFEPostMatchCreditAwards::ms_iCreditAwardCount` | 0x75f034 | | **directly after the row array** |
| `ms_iSharedStadiumBonusCredits` / `ms_bAddTournamentAchievement` | 0x75f038 / 0x75f03c | | |
| `FE_iVideoForceReward` / `FE_iVideoReward` / `FE_bWatchedBonusVideo` | 0x741030 / 0x74102c / 0x75c342 | | video coin doubler |
| `CDataBase::ms_pInstance` | 0x75cca4 | 0x73074c | +0x30 player ROM base (records at +0xc), +0x44 count |
| `CConfig::ms_cUserTypeInfo` | 0x759c2c | | DoublerMin at +0x184+4*type, DoublerMax at +0x198+4*type |
| `MC_bInPostMatchCallback` | 0x83f250 | | 1 while `MCU_TournamentEndOfMatchCallback` runs |
| `CFlow::ms_iFlowStep` | 0x741054 | | |
| `MC_tSeasonInfo` | 0x640bd8 | | {6, tid for tree 0..5 = 0,1,2,3,4,5} |

---------------------------------------------------------------------------------------------------------------------

## 1. Post-match award screen

### 1.1 Call chain [V]

`MCU_TournamentEndOfMatchCallback` 0x3643c4 (career matches, played or simulated):
`MC_bInPostMatchCallback=1` -> `SetPostMatchDisplayTurn(GetCurrentTurn())` -> `CCore::EndOfMatchProcessCommon` ->
`ProcessPostMatchTeamMan` -> `CSeason::PlayTurn(1)` (league table updated; the market turn hook 0x36a078 runs in here) ->
`IncMatchesPlayed` -> **`CFEPostMatchCreditAwards::SetupCreditAwardInfo` 0x239c58** (skipped only if `tGame+0x9ebc` (match
forfeited/quit, set by `GL_ForfeitGameSetScore`/pause-quit) is set and the tournament is not over) -> `CMyProfile::Save(1)` ->
`CFE::Forward(5)` (match summary screen) unless simulated.
`CCore::GenericEndOfMatchCallback` 0x204f24 calls SetupCreditAwardInfo only for online DLO matches (`ms_tInfo+0xfb0 != -1`).
Rows are cleared by `CFEPostMatchCreditAwards::Reset` 0x239c34 from `CSeason::IncTurn` 0x36a682.

### 1.2 Row layout `TCreditAwardInfo` (stride 0x208) [V]

| off | type | meaning |
|---|---|---|
| +0x000 | int32 | row type (enum below) |
| +0x004 | u16[256] | label, NUL-terminated wide string (written with `xsprintf`/`xstrlcpy(...,0x100)`; achievement titles `xsnprintf(...,0x80)`) |
| +0x204 | int32 | coins |

Capacity 12 rows (0x1860 bytes). No bounds check anywhere; row 12 would overwrite `ms_iCreditAwardCount` (0x75f034).
Each loop iteration `memclr`s row[count] and writes type/label/coins, but `count++` only if coins != 0 (or the row is forced,
i.e. the Total row).

Row types (loop `fp = 0..8`, jump tables 0x239fbe / 0x239fa6) [V]:

| type | row | label (LOC id -> English) | coins |
|---|---|---|---|
| 0 | result | 0x471 "Win Match" / 0x472 "Draw Match" / 0x587 "Lost Match" (fmt `"%s"` @0x5e0b86). Equal score -> penalties `tGame+0xa7f8[side]` decide win/draw/loss | `CREDITS_GetMatchWin/Draw/LossCredits()` (tid table; DLO: `CMultiplayerInfo::GetCoinReward`) |
| 1 | goals | 0x1d8 "Goals Scored" with fmt `"%s (%i)"` @0x5e0e5c, arg = user goals | goals x `CREDITS_GetMatchGoalCredits(tid)` (goals capped at 10 only in DLO) |
| 2 | clean sheet | 0x45e "Clean sheet" (only if opponent goals == 0) | `CREDITS_GetMatchCleanSheetCredits(tid)` |
| 3 | stadium | career: 0x45b "Home Stadium Bonus" only if `ms_tInfo+0xf6c` (home team) == 0x102, `ms_tInfo+0x14` (neutral) == 0 and `ms_tInfo+0x10` (match subtype) != 4; DLO: 0x8c7 "Shared Stadium Bonus" | `CSeason::GetStadiumBonus(season,-1)` = round(capacity / var 0x50); DLO: `ms_iSharedStadiumBonusCredits` |
| 4 | achievements | one row per `CFEMsgAchievements::ms_eAchievements[i]`, label `FESU_GetAchievementTitle` | `CREDITS_GetAchievementCredits(ach)` (then `ResetAchievements`) |
| 5 | tournament award | career: `CSeason::GetTournamentCredits(&isLeague)` 0x36b598 -> 0x45d "Final league position" if league, else tid 11 0x6df "Friendly Bonus", tid 16 0x6fa "Allstar Bonus", else 0x45c "Cup Win". DLO: season completion label from profile | `CREDITS_GetTournamentCredits` 0x263dd0 (league pos paid once per season, flag `CSeason+0x695`) |
| 6 | video bonus | 0x8c6 "Bonus" - never created by Setup, only by `AddBonus` 0x23a36c | `FE_iVideoForceReward` |
| 7 | tournament achievement | tid 12..15 -> achievement from table 0x638088 | `CREDITS_GetAchievementCredits` |
| 8 | **Total** | 0x7a6 "Total" (fmt `"%s"`), always added (forced) | sum of all rows with type != 8 |

If `tGame+0x9ebc` is set only types 4..8 are processed (0-3 skipped).

LOC ids index the key list of `assets/data/text/ftslang.xlc` (`LOCstring` 0x381aec -> `CTextDatabase::ms_pTextDbs[0]`);
parse with `build_dataset.load_xlc`. Useful: 0x7a6 "Total", 0x8c6 "Bonus", 0x835 "Sponsor", 0x4eb "Attendance",
0x4ef "Promoted", 0x4f0 "Relegated", 0x43d "Fan Approval Rating", 0x56d "Summary". There is no "prize money", "gate
receipts" or "TV" string: the mod writes its own wide strings into +0x004.

### 1.3 How the stock total is formed and credited [V]

End of Setup (0x23a236..0x23a274): `total = sum(rows[i].coins where rows[i].type != 8) + *(int*)(profile+0x2a7e8)` ->
**`blx CMyProfile::SetMatchCredits` at 0x23a274** (PLT; body 0x3776a4: `*(profile+0x2a7e8) = r1; CMyProfile::Save(1)`).
Credit happens later in `CFE::Process` 0x298c34..0x298cc4 once the current screen type is not 1/5/8 and no message box is up:
`AddCredits(*(profile+0x2a7e8) + (FE_bWatchedBonusVideo && FE_iVideoReward>0 ? FE_iVideoReward : 0), 0,0,1,0)` at
**0x298cb6**, then `SetMatchCredits(0)`, `Save(1)`. So **the game credits exactly the value passed to SetMatchCredits**
(+ the video bonus); it does not re-read the rows.
`GetTotalCreditAward` 0x23a32c = sum of rows with type != 8 (used by the video doubler, footer coin button, analytics).

### 1.4 How the screen renders the rows [V]

`CFESMatchSummary::SetupCoinSummary` 0x27ad6c builds a `CFERewardTable(4,10,0)` (10 rows x 3 cols: label text cell,
`fe_credit.png` icon, number via `FESU_GetNumberString`). Loop r5 = 0..9:
- `r5 < count-1` -> detail row r5 = `rows[r5]` (label +4, coins +0x204), whatever its type;
- `r5 == 9` (and count >= 1) -> **row 9 = `rows[count-1]` drawn as the highlighted Total** (plus the optional video button),
  and `CFERewardTable::SetCoinCount(rows[count-1].coins)` drives the animated coin counter;
- other rows empty.

Consequences: **at most 9 detail rows + 1 total (count <= 10)**. With count >= 11 the total row is never drawn and the coin
counter shows 0. The total shown is simply the last row, so the mod must keep its Total row last. `AddBonus` (after watching
the video) appends one row, so keep **count <= 9 before the bonus** if the video button can appear. The table cells copy
the strings when the screen is set up (labels live in `ms_tCreditAwardInfo` anyway until the next `IncTurn`).

### 1.5 Video "doubler" [V]

`SetupCoinSummary` (0x27b1c0) and `CFESMatchSummary::Process` (0x27ac04, on button press) compute
`bonus = GetTotalCreditAward()` if `MinDoubler <= total <= MaxDoubler` (and `MinDoubler != -2`), otherwise
`bonus = MinDoubler` (`MinDoubler == -1` -> `GetVar(7)` CreditsEarnRewardVideos = 30). Bundled `UserTypeInfo` DoublerMin 30,
DoublerMax 10000 for every user type -> **the bonus equals the whole match total (a doubling) for totals 30..10000**.
`AddBonus` turns the old Total row into type 6 "Bonus" (coins = bonus) and appends a new Total = old total + bonus;
`FE_bWatchedBonusVideo=1`; the rewarded-video callback copies `FE_iVideoForceReward` into `FE_iVideoReward`, which CFE::Process
adds on top of the SetMatchCredits value.
Options for the mod: (a) patch `CConfig::GetMaxDoubler` 0x201678 to `movs r0,#0; bx lr` - SetupCoinSummary then never shows
the video button (`cmp max,#0 / cmpne min,#0 / beq -> no ads`, 0x27ae14) [V]; (b) give the mod's income rows type 8: they are
still displayed (display ignores the type) but excluded from `GetTotalCreditAward`, so only the stock-sized part is doubled
(footer/analytics totals then also exclude them - cosmetic) [V by the sum loops]. Ads need network/Play services, so on
the offline tablet the button most likely never appears anyway [G].

### 1.6 Recommended hook (a): per-match income with native breakdown

**Site 0x23a274**, bytes `8c f7 0a e8` (`blx CMyProfile::SetMatchCredits` PLT 0x1c628c) -> `bl cave_econ_match`.
Registers at the site [V]: `r0 = MP_cMyProfile` (base+0x84a260), `r1 = stock total (row sum + pending)`,
`r2 = old pending *(profile+0x2a7e8)`, r3 = scratch, lr (after the bl) = 0x23a279. r4-r11 are dead here (the function's own
epilogue `add sp,#0x8c; pop.w {r4-r11,pc}` restores them from the stack), only sp must be balanced. All rows, the count,
`ms_tInfo`, `tGame` scores and the league table (PlayTurn already ran) are valid.

Cave sketch (follow PATCHING_RULES: PC-relative only):
```
cave_econ_match:            @ entered by bl from 0x23a274
    push  {r0, r1, r4, lr}
    ... market-style dispatch: econ_post_match(0, 0, base)   @ C side does everything below
    cmp   r0, #0            @ C returns 1 = handled (it called SetMatchCredits itself)
    pop   {r0, r1, r4, lr}
    bne   done
    b.w   0x3776a4          @ fallback / lib missing: stock SetMatchCredits(profile, stock total)
done:
    bx    lr                @ back to 0x23a278
```
C side (`econ_post_match`), all offsets [V]:
1. Gate: `*(int*)(tinfo+0xfb0) == -1` (career) and `*(u8*)(base+0x83f250)` (MC_bInPostMatchCallback) == 1.
2. Read the breakdown inputs (section 2), compute the mod's lines.
3. Rewrite `rows[i].coins`/labels in place, drop rows (compact), append rows **before** the Total row, then write the Total
   row last: `type = 8`, label "Total", coins = sum of displayed non-total rows. Keep `count <= 9` (<= 10 if the doubler is
   disabled) and never exceed 12. Update `ms_iCreditAwardCount`.
4. `pending = *(int*)(profile+0x2a7e8); SetMatchCredits(profile, total + pending)` (`base+0x3776a5`, signature
   `void (CMyProfile*, int)`). Return 1.
The screen then shows the mod's lines and the total, and `CFE::Process` credits that total. [V for data flow; the whole thing
is not runtime-tested.]

Stock alternative for scaling only: 0x298cb6 (`blx AddCredits`, r0 = amount incl. video bonus).

---------------------------------------------------------------------------------------------------------------------

## 2. Match result data available at the hook

| item | how | tag |
|---|---|---|
| season | `season = profile + 0x14` | V |
| tournament of the match | `t = CSeason::GetSpecificTournament(season, CSeason::GetPostMatchDisplayTurn(season))` (0x36a8fc, 0x36b608 = `*(int*)(season+0x6030)`); `tid = CTournament::GetID(t)` 0x360a30. tid 6 = playoffs (stock pays the league's tid). Tid list in STOCK_TRANSFER_FLOW 5.2 | V |
| main league | `L = CSeason::GetSpecificTournament(season, ETournamentIndex 0)` 0x36a118 (`*(season+0x6ac + 4*idx)`) | V |
| is league match / league over | `MCU_IsTournamentLeague(tid)` 0x3646a0 (hardcoded table +0x18); `CTournament::IsOver(t)` 0x360d06 (`t[6] >= t[4]`) | V |
| user side | `side = CMatchSetup::GetUserSide(0)` 0x2ee6c0 (`ms_tInfo[0x40+12*i]`, 2 -> -1) (career path passes 0) | V |
| goals | `sw = tGame[0x9ed4]`; `us = tGame[0x9edc + (side ^ sw)]`, `them = tGame[0x9edc + ((1-side) ^ sw)]` (u8) | V |
| penalties | same indexing at `tGame+0xa7f8` | V |
| win/draw/loss | us > them win; equal -> penalties; as the stock row 0 | V |
| forfeit/quit | `tGame[0x9ebc]` != 0 | V (set in pause-quit / `GL_ForfeitGameSetScore`) |
| home / away teams | `ms_tInfo+0xf6c` = team 0 (home), `+0xf70` = team 1 (away) (`CMatchSetup::SetMatchTeams(home, away, ...)` 0x2efe90); home match = `+0xf6c == 0x102` | V (home semantics I, matches the "Home Stadium Bonus" check) |
| neutral venue | `ms_tInfo+0x14` (u8, `SetMatchProperties` last arg); `MCU_IsNeutralMatch()` 0x364514 | V |
| match subtype | `ms_tInfo+0x10` (4 = no stadium bonus) | V (meaning of 4 G) |
| DLO/online | `ms_tInfo+0xfb0` (-1 = career/offline) | V |
| **attendance** | `*(int*)(ms_tInfo+0xf64)`, written by `CMatchSetup::SetMatchEnvironment` (r3) / `SetAttendance` 0x2ef7fc. For a user home match `MCU_SetupTournamentMatch` passes `CSeason::CalculateAttendance(season, home, away)` 0x36d858 = `(int)(cap * (fan*20 + homeStars*40/5 + awayStars*40/5) / 100)` with `cap = GetStadiumCapacity(season,0)`, `fan` = approval 0..1, stars = `CDataBase::GetStarRatingByID` 0x20bf98 (team+8, 0..5). Away: `(75+Random(25))% x opponent stadium capacity`; neutral: team 0xd4's stadium | V (persistence until `CMatchSetup::MatchReset` I) |
| stadium capacity | `CSeason::GetStadiumCapacity(season, bIncludePlanned)` 0x36d590: sum over 8 sections of model capacity (`CGfxEnv::FindModelInfoDescription(envcfg+0xd34+32*i)->+0x48`) for built sections (`CSeasonStadiumInfo` at season+0x960: `int state[8]`, then `TEnvConfig` (0xebb bytes) at season+0x980) | V |
| stadium bonus (stock) | `GetStadiumBonus(season, cap)` 0x36d548 = `RoundFloatToNearestInt(cap / GetVar(0x50))` (cap = -1 -> current capacity); bundled divisor 1750 | V |
| league position | `CTournament::GetTeamLeaguePos(L, 0x102)` 0x36202e -> `CLeagueTable::GetTeamLeaguePos` 0x361a38; **0-based** (0 = top), -1 if no table | V |
| points etc. | `CTournament::GetTeamLeagueTableStat(L, team)` 0x36203e -> `CLeagueTableStat*`: `GetNumPoints` 0x3615fc, `GetNumDraws` 0x3615f0, `GetGoalDifference` 0x36160e; league round `CTournament::GetCurRound` 0x360a74 | V addresses, field layout not traced |
| division | `CSeason::GetUserLeagueInTree(season)` 0x36cc40 -> `GetTeamLeagueInTree(season, 0x102)` 0x36cd44: **0 Elite, 1 Junior Elite (Prestige), 2 Div1, 3 Div2, 4 Div3, 5 Academy** (league tid == tree index, `MC_tSeasonInfo`). Tree data: `season+0x69c` u8 league count (6), `+0x6a0` int* league tids, `+0x6a4` u8* team counts, `+0x6a8` u16** team lists | V |
| user team id | constant 0x102 (`CSeason::GetUserTeamID` 0x36a128) | V |
| expected finish | `CTournament::GetTeamExpectedFinishIndex(L, team)` 0x36103c (used by the fan approval formula) | V |
| season counters | `CSeason+3` u8 season count (`GetSeasonCount` 0x36aa88), `+4` u8 matches played (`GetMatchesPlayed` 0x36aa8c), current slot `GetCurrentTurn` 0x36a67c | V |

Stadium upgrade requirement for promotion: `MCU_GetMinStadiumCapacity(tree)` 0x3648c4 = `GetVar(0x40+tree)` for tree 0..4:
Elite 80000, Junior Elite 60000, Div1 45000, Div2 30000, Div3 15000 (bundled) [V].

---------------------------------------------------------------------------------------------------------------------

## 3. Season end

### 3.1 Flow [V]

`CFlow::Process` 0x298d50, step table 0x298d7a (steps 0..6 -> 0x298d88, 0x298de8, 0x298e1e, 0x298e9a, 0x298ea8, 0x298f24,
0x298fc2):
- step 4 (0x298ea8): `AdvanceToNextActiveTurn`; when it returns 0 (no turn left) -> `SetSeasonSummaryInfo` 0x36aaf0.
- step 5 (0x298f24): season summary, all-time records, "all objectives" achievement, forward to screen 0x13.
- **step 6 (0x298fc2)**: if `CSeason::IsOver` 0x36b4f0: `tree = GetUserLeagueInTree`; `block = tree >= 1 &&
  GetStadiumCapacity(season,1) < MCU_GetMinStadiumCapacity(tree-1)`; **`CSeason::NextSeason(season, block)` at 0x29900a**.
- `NextSeason` 0x36aa08: new seed, **season count +1 (`+3`), matches played = 0 (`+4`)**, `CTeamManagement::NextSeason`,
  `SetupNextSeasonTournaments(block)` 0x36ec0c -> **`DoPromotionRelegation(block)` 0x36e894** (swaps teams between
  adjacent league lists by final table / playoffs winner `CTournament::DidUserWin(season+0x6b0)`; the user is not moved up
  when `block`), then `SetupNextSeasonMainLeague/Playoffs/GcCup/EliteCup/BonusCups/AllstarMatch`; objectives; clears
  `+0x692..0x695` (**GivenSeasonAwards/GivenLeagueAwards reset**); `CSeasonSchedule::Init(newTree)`.
- `CSeason::NewSeason` 0x369d20 is only the brand-new career (fan approval 0.5, league tree, stadium reset); not per season.

Stock season-end coins: (1) league final position `TournXxxPosN` (var `0xa7 + tid*16 + pos`) is paid as row type 5 on the
post-match screen of the **last league match** (`GetTournamentCredits` requires the league `IsOver`; once per season via
`CSeason+0x695`); (2) season objectives: `CSeason::GetObjectivesAwards` 0x35d6c8 = 25 (var 9) per completed objective (4 slots
at `season+0x920`, stride 0xc, byte==1), once (`CSeason+0x694`), shown via `CFEMsgAchievements` from `CFE::Process` 0x298bfe.

### 3.2 Recommended hook (b): season-end payouts

**Site 0x29900a**, bytes `33 f7 76 ef` (`blx CSeason::NextSeason` PLT) -> `bl cave_econ_season`.
Registers [V]: `r0 = season (profile+0x14)`, `r1 = block` (1 = promotion refused for stadium capacity); `r4 = CFlow*`
(callee-saved, needed at 0x299028); r5-r7 scratch for stock (reloaded); lr (after bl) = 0x29900f; returns to 0x29900e,
which reloads r0 itself.
```
cave_econ_season:
    push  {r0, r1, r4, lr}
    ... dispatch econ_season_pre(season, block, base)   @ record old tree, final pos, points; may return new block
    pop   {r0, r1}            @ (or r1 = returned block if the mod overrides the stadium rule)
    bl    0x36aa08            @ CSeason::NextSeason (Thumb, in range of bl from the cave)
    ... dispatch econ_season_post(0, 0, base)            @ new tree -> promoted/relegated; pay + message box
    pop   {r4, pc}
```
In `econ_season_pre` (before the rollover, tables still final):
`oldTree = GetUserLeagueInTree(season)`; `L = GetSpecificTournament(season, 0)`; `pos = CTournament::GetTeamLeaguePos(L,0x102)`
(0-based); points via `GetTeamLeagueTableStat`; `seasonNo = *(u8*)(season+3)`; `block` tells whether a promotion was
vetoed. In `econ_season_post`: `newTree = GetUserLeagueInTree(season)`; `newTree < oldTree` promoted, `>` relegated, `==`
stayed (robust for playoffs too); `*(u8*)(season+3)` is now +1. Pay with `CCredits::AddCredits(amount,0,0,1,0)` 0x2634c0
(5th arg on the stack) or the mod's own finance, and queue a box with `ui_show_notice()` / `CFEMessageBox` +
`CFE::AddMessageBox` 0x298608 (STOCK_TRANSFER_FLOW section 4). The box is queued and shown over the next screen [I: the flow
calls `CFlow::Forward` right after; boxes are a global queue, but not runtime-tested].

Alternative (native display of the prize money): at hook (a), when `t == L && CTournament::IsOver(L)` (final league match),
replace/extend the stock type-5 "Final league position" row with the mod's prize/TV/sponsor lines on the post-match screen.
Promotion is not decided yet at that point (playoffs, stadium veto), so promotion/relegation money still belongs to hook (b).

---------------------------------------------------------------------------------------------------------------------

## 4. Coin sinks and income in the bundled config

`CConfig::GetVar(EConfigGameVariables)` (PLT 0x1c1300); var table `s_tConfigVarInfo` @0x61b7d8, 0x1b8 entries of 0x108 bytes
(`char name[0x100]; int default` at +0x100). Bundled config `assets/data/x_android/dls_config.dat`: whole file XOR u32 0x53d392af,
then zlib -> XML (decoded copy `F:\android modding\tmp\econ\dls_config.xml`). The XML is sectioned; vars with duplicate names
(MinCost, RegularityWeeks, MinRating...) are listed in XML section order = var order. A downloaded config can override at
runtime. Several bundled tags do not match var names (e.g. `PlayerDevBreakthroughMinCost` vs var `PlayerDevBreakthroughMinPoints`,
`TournAllstarClassicVictoryCoins`) -> the lib default applies for those [V names, parser behaviour I].

| sink / source | code | var (idx) default / **bundled** |
|---|---|---|
| Stadium section build | `CSeason::GetStadiumSectionConstructionCost(sect, capArg, &cost, &o2, &o3)` 0x36d664: `cost = InterpolateClamp(capArg, typeInfo+0x2c88, +0x2c8c, MinCost, MaxCost)`, `+= cost*Roof%/100` if the model has a roof; charged as `SubtractCredits(sum ms_iCurrentSectionCost[8] (0x761d38) ...)` in `CFESDreamLeagueStadium::CompleteStadiumUpgradesCB` 0x2718f2; +0.02 fan approval 0x2719ee | Corners 0x51/0x52/0x53 100/1000/10% -> **50/500/10%**; Ends 0x54-0x56 200/1500/15% -> **75/1000/15%**; Sides 0x57-0x59 500/2000/25% -> **125/1200/20%** |
| Stadium bonus income | per home match | StadiumBonusDivisor 0x50 2000 -> **1750** |
| Promotion capacity | `MCU_GetMinStadiumCapacity` | 0x40..0x44: 75000/60000/45000/30000/15000 -> **80000/60000/45000/30000/15000** |
| Heal injured | `CSeason::GetHealPlayerCredits(season, weeks, energy)` 0x36e51c: weeks>=1: `HealPlayerCost*weeks + EnergyMaxCost`; else energy refill `max(1,(1-clamp((e-18375)/(37500-18375)))*EnergyMaxCost)` | HealPlayerCost 0x1a 50 -> **30**; EnergyMaxCost 0x1b **50** |
| Scouting session | `CScoutingInfo::GetCurSessionCost` 0x36d2e6 = `InitialCost + ExtraCost*n` (n = sessions used this turn, first free if a free session was rolled) | 0x177 100 -> **50**; 0x178 **100**; MaxSessions 0x17a 5 -> **3**; FreeSessionPercentChance 0x17b 20 -> **10**; scouted players +20% price (0x179) |
| Player development (training) | `CPlayerDevelopment::GetTrainingCost(type)` 0x20fb34: avg of 3 stats (x10 scale) for the type (tables 0x638500/20/40) -> `x = InterpolateClamp(avg, PlayerDevIncStart, 1000, 0, 1000)`; `cost = MinCost + (int)(pow(x/1000, Whole+Tenths/10)*(MaxCost-MinCost))`, rounded up to 5 (2 if <50) | MinCost 0x4a 50 -> **20**, MaxCost 0x4b 750 -> **200**, exp 0x46/0x47 **2.1**, IncStart 0x4c **500**; points 0x48/0x49 150/200 -> **100/150**; breakthrough 250/300 (default), chance 0x4f **10**; sessions/turn 0x45 3 -> **-2** |
| -> resulting training cost (bundled) | avg stat 60/70/75/80/85/90/100 | ~25 / 45 / 60 / 80 / 105 / 130 / 200 coins |
| Create player | `CP_GetCost(genPos, level)` 0x20512c = `RoundToNearest(GetPlayerValue(NULL, genPos, r, 0, 1), 100)`, `r = (81+5*level + {84,89,94,100}[level])/2` = 82/87/92/98 | bundled PlayerValues -> GK/DEF/MID/ATT: L0 1100/1200/1400/1500, L1 1600/1700/1900/2100, L2 2100/2300/2600/2800, L3 2800/3100/3500/3800. CostPercentIncrease 0x18b-0x18e 25/50/75/100% and MaxNum 0x187-0x18a applied by the caller (not traced) [G]; charged `SubtractCredits(CFEMsgCreatePlayer::ms_iPlayerValue)` 0x24ab28 |
| Kits / pitch patterns | customisation | KitCost* 0x31-0x3f **150..500**; Pitch*Cost 0x27-0x30 500000 -> **200..400** |
| Transfers | GetPlayerValue / sell 50% | see STOCK_TRANSFER_FLOW 5.3; TransfersMinCredits 0x19c 500 |
| Match income | tables | STOCK_TRANSFER_FLOW 5.2 (Win 0x5a-0x67, Draw 0x68-0x75, Loss 0x76-0x83, Goals 0x84-0x91, CleanSheet 0x92-0x9f, CupWin 0xa0-0xa6, LeaguePos 0xa7-0x106, FriendlyInfo 0x107-0x115) |
| Season objectives | `GetObjectivesAwards` | CreditsEarnObjectiveSeason 0x9 **25** each (4 objectives) |
| Other | | CreditsEarnWelcome 0x4 **1000**, StartCredits 0x3 **0**, CreditsEarnRewardVideos 0x7 **30**, Share 0x8 **25**; DynamicDifficulty{BuyPlayer,StadiumPurchase,PlayerDev}Adjust 0x13-0x15 10/10/10 -> **4/1/2** (spending raises AI difficulty via `CProfileGameSettings::IncDynamicDifficulty`) |

---------------------------------------------------------------------------------------------------------------------

## 5. Player age, ratings, development

### 5.1 ROM / age [V]
- `CDataBase::LoadPlayerROM(TPlayerROM* out /*0xb4 bytes or NULL*/, int id)` 0x20c710 (static): binary search over
  `ms_pInstance->+0x30 + 0xc`, record 0xb4, u16 id at +0; returns 1 if found (copies when out != NULL). Created players
  (ids 0xffdf..0xfffe) have no ROM -> 0.
- `CDataBase::GetPlayerInfo(TPlayerInfo& info, int id, int team, bool bApplyDev, TPlayerROM* romOut, int, TTeamPlayerLink*, int)`
  0x20805c (static; r0-r3 + 4 stack args): loads the ROM into `romOut` (or a local if NULL) and converts it; created players
  come from `CSeason::GetCreatedPlayer`. So one call yields both the info and the birth date (ROM +0xa8 day, +0xac month,
  +0xb0 year, u32).
- `CDataBase::LoadPlayerROMByIndex(out, index)` 0x20ddd0.
- **No age function exists and the UI never shows age** (no age string in ftslang.xlc; no Age/Birth/Year symbols).
- **Data caveat: 4440 of 5816 ROM records have birth date 1900-01-01 (unknown)**; real dates exist for 1376 players (mostly
  rated 70+: e.g. 80-84: 151 real vs 127 unknown). Real years 1972..1997 (+1 bogus 2012). The mod needs a fallback age
  (e.g. deterministic from player id and rating).
- In-game date: there is no calendar year. Use `CSeason+3` (season count, 0 for the first season, +1 per `NextSeason`) plus a
  base year; the DB is 2017/18 data, so **year = 2017 + seasonCount** (or 2018 for season end) is the natural choice [G].
  (`XSYS_GetCurrentMatchDateTime`/`TMatchDateTime` are real-clock/weather related, not a season calendar [I].)

### 5.2 Ratings and development
- `PU_GetPlayerRating(TPlayerInfo*)` 0x2b35d0 (weights model, see NOTES_dataset). **`PU_GetPlayerRating(int id)` 0x2b379c**
  = `GetPlayerInfo(info, id, -2, bApplyDev=1, NULL, -1, NULL, 0)` then rating; returns 0 if the id is unknown [V].
  This is the "current overall by id" function, including user development deltas.
- Development is **user-squad only** [V]: `CPlayerDevelopment::AddPlayer(id,bool)` 0x20e130 is called from sign/create/new
  dream team; `RemoveNonUserPlayers` 0x20e928; stats in `CPlayerDevelopment::ms_pPlayerDevStats` (records of 0x20 bytes, +0 player
  id; `GetPlayerStats(id)` 0x20ed8c), applied by `CDataBase::ApplyStatDeltas` inside `PlayerROMtoInfo(rom, info, team, bApplyDev)`
  0x20b298 and for created players in GetPlayerInfo. Training: `ApplyTraining` 0x20fa48 (from `CFEMsgPlayerDevSelect`),
  `GetStatInc` 0x20f440 = random in [PlayerDevMinPoints, MaxPoints] (breakthrough: 0x4d/0x4e), rounded to 5.
  `GetPlayerDevPercent(cur, target)` 0x20f8f4. There is no potential value for AI players.
- **No AI aging, retirement or regen** found [I: no symbols, `CTeamManagement::NextSeason` 0x2f2002 only resets per-player
  season state (energy 37500, byte +4) of the user squad, `NextSeason` touches no player stats]. AI ratings are the static
  ROM values forever; the mod has to implement any aging itself (e.g. via its own rating offsets).

---------------------------------------------------------------------------------------------------------------------

## 6. Fan approval and other finance-like data

- `CSeason+0x68c` float 0..1 (`GetFanApprovalRating` 0x36af8c; 0.5 at career start). `AdjustFanApprovalRating(season, float
  delta)` 0x36af94: clamp to [0,1], records `CProfileStats::CheckRecordFanRating`, reaching 1.0 unlocks achievement 0x29
  (Fan Favourite) [V]. Float passed in r1 (softfp).
- Deltas in stock code [V]: sign player +0.005 (0x251f74), create player +0.005 (0x24ab1c/0x24aca4), stadium upgrade +0.02
  (0x2719ee), tournament won +0.05 (`CTournament::Update` 0x3605cc), per match `CTeam::ProcessPostMatch` 0x2b2828:
  win `+lerp(fan: 0 -> 0.025, 0.9 -> 0.005) * (1 + expectedFinishIdx/numTeams)`, loss `-lerp(fan: 0.1 -> 0.005, 1.0 -> 0.025) /
  (1 + expectedFinishIdx/numTeams)`, draw 0 (which branch is win vs loss inferred from the sign) [V formula, I labels].
- Uses: attendance (`CalculateAttendance`, 20% weight of capacity), so a fan-approval-driven gate receipt =
  `attendance * ticketPrice` ties the mod economy to stock mechanics. Nothing else in the lib models club money (no wages,
  sponsors, TV); "Sponsor" (LOC 0x835) is an ad label.

---------------------------------------------------------------------------------------------------------------------

## 7. Summary: recommended hook set

| purpose | site | patch | live registers | exit |
|---|---|---|---|---|
| (a) per-match income, native breakdown | **0x23a274** (`8c f7 0a e8`) | `bl cave` over `blx SetMatchCredits` | r0 = profile, r1 = stock total, r2 = old pending; rows/count final; r4-r11 dead | C rewrites rows + calls `SetMatchCredits(profile, total+pending)` (0x3776a5) then `bx lr`; fallback `b.w 0x3776a4` with r0/r1 |
| (a') disable video doubler (optional) | 0x201678 `CConfig::GetMaxDoubler` | `movs r0,#0; bx lr` | - | - |
| (b) season-end payouts + message | **0x29900a** (`33 f7 76 ef`) | `bl cave` over `blx NextSeason` | r0 = season, r1 = promotion-veto flag, r4 = CFlow* | pre-dispatch, `bl 0x36aa08`, post-dispatch, return to 0x29900e |
| scale-only alternative | 0x298cb6 | `bl cave` over `blx AddCredits` | r0 = match total (+video), r1=r2=0, r3=1, [sp]=0 | `b.w 0x2634c0` |

Neither 0x23a274 nor 0x29900a is used by the current `build_mod.py` hooks (market 0x36bc42/0x36a078/0x276490, stock
buy/sell 0x277006/0x250a3c/0x23e404).
