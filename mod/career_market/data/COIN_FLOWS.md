# DLS18 coin flows: sinks, income, shop pushes, hook points (static analysis, libDLS18.so v5.064 armeabi-v7a)

Status: COMPLETE (sections 0-6), 2026-09-25. Static analysis only. Tags: **[V]** verified by disassembly, **[I]** inferred,
**[G]** guess. VAs are stock-lib offsets (Thumb: call `base + va | 1`; PLT stubs ARM, even). Companion docs:
`ECONOMY_HOOKS.md`, `STOCK_TRANSFER_FLOW.md`, `mod/PATCHING_RULES.md`.

`CConfig::GetVar(v)` = `CConfig::ms_iVars[v]` (int[0x1b4] at 0x75ba54). Profile coins = `*(int*)(MP_cMyProfile + 0x2a7cc)`.

## 0. Primitive functions [V]

| function | VA | body |
|---|---|---|
| `CMyProfile::SubtractCredits(profile, n, bool)` | 0x377a18 | **leaf, no push**: `c = coins - n; if (c <= 0) c = 0; coins = c; if (n >= 0) *(profile+0x2a7d8) += n` (lifetime "credits spent"). bool unused |
| `CCredits::SubtractCredits(int n)` | 0x2633bc | `push {r4,lr}`; `ms_eAnimState = 2`; `CMyProfile::SubtractCredits(MP_cMyProfile, n, 1)` (blx at **0x2633d4**, returns to 0x2633d8); SetCredits(max(0,GetCredits)); anim; SFX 0xe; if `CREDITS_eSpendTrigger == 3` (stadium) `CDreamLeagueStats::CheckHighestCapacity(GetStadiumCapacity(season,0))` |
| `CMyProfile::AddCredits(profile, n, bPurchased, bNoSave)` | 0x377984 | `push {r7,lr}`; `coins += n`; bPurchased -> `IncPurchasedCredits(n)`, `g_eSaveMode = 3`; else `if (n >= 0) *(profile+0x2a7d4) += n` (lifetime earned); then `bNoSave ? g_eSaveMode = 1 : Save(1)` |
| `CCredits::AddCredits(int n, bool b1, bool b2, bool b3, bool b4)` | 0x2634c0 | `if (n < 1) return` (before any push); `push {r4,lr}`; `ms_eAnimState = 1`; `CMyProfile::AddCredits(MP_cMyProfile, n, b1, [sp+8]=b4)` (blx at **0x2634e2**, returns to 0x2634e6); anim; tail-call SFX 0x2d |
| `CMyProfile::SetCredits(profile, n)` | 0x377972 | `coins = n; Save(1)` (tail) |
| `CCredits::SetCredits(n)` | 0x26348c | `CMyProfile::SetCredits(MP_cMyProfile, n)` + anim; **no callers** |


Calls go through ARM PLT stubs (`blx #plt`), which do not touch sp/lr, so at a hooked callee entry `lr = callsite + 4 | 1`
(Thumb bit set; add the lib base at runtime). There are **no B.W tail calls** to any of the four primitives (full-.text scan) and
no pointer references to them (no GOT/literal refs), so the BL lists below are complete. `CMyProfile::SetCredits` has only
2 other callers: `CCredits::SubtractCredits` (clamp) and `MCU_Restart` 0x3649c2 (reset). No other code writes the coin field
through a named setter [V]; `CCredits::NotEnoughCreditsCallback` 0x2639a0 has no BL/literal refs (dead) [V].

## 1. Spend classification [V]

### 1.1 The globals

| global | VA | GOT slot | writers | readers |
|---|---|---|---|---|
| `CREDITS_eSpendTrigger` (int) | **0x760bec** | 0x7307e0 | 10 stores (below) | only `CCredits::SubtractCredits` 0x263410 (`== 3` -> CheckHighestCapacity) |
| `CREDITS_ePurchaseTrigger` (int) | **0x760be8** | 0x730c30 | 9 stores, each immediately before a `new CFEShopDialog` (shop push) | none in native code (read by shop/analytics path via symbol, not by any spend) |

Neither global is ever reset (no store of 0 anywhere); each keeps the last value written.

`CREDITS_eSpendTrigger` values (set just before the spend):

| value | meaning | store VA(s) |
|---|---|---|
| 1 | player signing (transfer buy) | 0x21054c (`str sl`, sl = 1 from 0x21049a) |
| 2 | player development / training | 0x24e3c8 |
| 3 | stadium (section upgrades, pitch pattern, section construction) | 0x2718ea, 0x27193a, 0x271962, 0x271cee (only if `!ms_bEndOfSeason`), 0x271f0c (after the spend) |
| 4 | heal injured / energy refill | 0x23e6c4 |
| 5 | friendly match entry | 0x26bfb8 |
| 12 | device credit reimbursement claw-back (negative reimburse) | 0x381270 |

`CREDITS_ePurchaseTrigger` values (set before the shop push; same numbering where both exist): 1 sign player 0x251ee0,
2 training 0x24e9f0, 3 stadium 0x271690 / 0x271d22, 4 heal 0x23e67e, 5 friendly 0x26bf74, 6 unlockable 0x37bc9a,
7 create player 0x24b6f8, 10 scouting 0x250462.

### 1.2 Is the trigger reliable? **No.**

Spends with **no** `eSpendTrigger` store: create player (0x24ab28), unlockables (0x37bc3e), scouting (0x2504a8, which also
bypasses CCredits entirely), and stadium section construction when `ms_bEndOfSeason` is set (0x271e6e). Stadium upgrades and
pitch both use 3 (not separable by trigger). So a trigger-only hook at 0x377a18 would mis-book those spends under whatever
category was used last. **Classify by caller lr instead** (trigger can be kept as a cross-check).

### 1.3 Recommended hook: `CMyProfile::SubtractCredits` 0x377a18 (leaf, no push; r0 = profile, r1 = n)

```
ret = lr & ~1 (minus base)
if ret == 0x2633d8:            // via CCredits::SubtractCredits (push {r4,lr} at 0x2633bc, nothing else pushed)
    ret = ([sp+4] & ~1) - base // original caller's return address
category = MAP[ret]
```

| return addr (key) | call VA | via | function | category | n (amount) | trigger at call |
|---|---|---|---|---|---|---|
| 0x210556 | 0x210552 | CCredits | `CTransfers::SignPlayerAttempt` | transfer buy | fee arg `r2` (`[sp+0x1c]`) | 1 |
| 0x23e6e2 | 0x23e6de | CCredits | `CFETeamManagement::HealSelected` | heal injury / energy refill | `CSeason::GetHealPlayerCredits(weeks, energy)` | 4 |
| 0x24ab2c | 0x24ab28 | CCredits | `CFEMsgCreatePlayer::SetMode(1)` | create player | `CFEMsgCreatePlayer::ms_iPlayerValue` | **stale** |
| 0x24e3d0 | 0x24e3cc | CCredits | `CFEMsgPlayerDevSelect::TrainingSelectCB` | training | `CPlayerDevelopment::GetTrainingCost(type)` | 2 |
| 0x2504ac | 0x2504a8 | **direct** (lr) | `CFEMsgBoxScoutPlayer::ScoutPlayersCB` | scouting | `CSeason::GetCurScoutSessionCost()` | **stale** |
| 0x26bfc0 | 0x26bfbc | CCredits | `CFESDreamLeagueObjectives::PlayFriendlyCallback` | friendly entry | `GetFriendlyInfo()->byte[3]` | 5 |
| 0x2718f6 | 0x2718f2 | CCredits | `CFESDreamLeagueStadium::CompleteStadiumUpgradesCB` | stadium sections (batch) | `[sp+0x30]` total - `ms_tStadiumStats+0xc` (pitch part) | 3 |
| 0x271974 | 0x271970 | CCredits | same | pitch pattern | `ms_tStadiumStats+0xc` (only if >= 1) | 3 |
| 0x271e72 | 0x271e6e | CCredits | `CFESDreamLeagueStadium::CompleteSectionConstructionCB` | stadium section (single) | `ms_iRelativeConstructionCost[ms_eCurrentStadiumSection]` | 3 / **stale at season end** |
| 0x37bc42 | 0x37bc3e | CCredits | `CProfileUnlockables::UnlockItemCallback` | unlockable (kit/badge/etc.) | `CProfileUnlockables::GetUnlockCost(e)` | **stale** |
| 0x381276 | 0x381272 | CCredits | `CDeviceCreditReimburse::CheckDownload` | reimburse claw-back (server) | `-reimb[+0x80]` when `<= -1` | 12 |

Notes: `CMyProfile::SubtractCredits` adds `n` to lifetime spent (`+0x2a7d8`) only if `n >= 0`, and clamps coins at 0 (so a
booked amount can exceed the real deduction if coins < n; all UI paths check `cost <= coins` first, except the stadium batch
call where the check at 0x271680 uses the same total). Scouting skips the coin animation/SFX and `SetCredits` re-save.
The mod can equally hook `CCredits::SubtractCredits` 0x2633bc (lr = caller) plus a separate case for scouting, but one hook at
0x377a18 covers all 11 sites.

## 2. Every coin sink: cost function, exact var indices, formula, visibility [V unless tagged]

Var indices were read from the immediate loaded into r0 before each `CConfig::GetVar` BL (all 201 GetVar call sites indexed;
the 11 computed-index sites were resolved by hand where relevant). Bundled values are from `dls_config.xml` in XML order
(NOT the `xml=` column of `vars.txt`, which is mis-mapped for duplicate names).

| # | sink | charge site (ret key) | cost function | ms_iVars indices | formula | shown before paying? |
|---|---|---|---|---|---|---|
| S1 | transfer buy | 0x210556 | `CFEMsgSignPlayer::ms_iPlayerValue` (0x7601dc) = `CTransfers::GetPlayerValue(info,-1,-1,1,1)` 0x212b3c | PlayerValues table (computed idx, see STOCK_TRANSFER_FLOW 5.3), scouted +0x179 % | see STOCK_TRANSFER_FLOW 4/5.3 | yes, `CFEMsgSignPlayer` confirm box price |
| S2 | heal injury / energy refill | 0x23e6e2 | `CSeason::GetHealPlayerCredits(weeks, energy)` 0x36e51c | **0x1a** HealPlayerCost, **0x1b** EnergyMaxCost | weeks>=1: `0x1a*weeks + 0x1b`; else `max(1, (1 - clamp((e-18375)/(37500-18375)))*0x1b)` | yes, heal button (`CFETeamManagement::SetShowHealButton` 0x2403a8 computes same cost); charged on tap, no confirm box |
| S3 | create player | 0x24ab2c | `CFEMsgCreatePlayer::ms_iPlayerValue` (0x75fdc0) = `CTransfers::GetCreatedPlayerValue()` 0x212f98 (set in ctor 0x24a760) | **0x18b + level** for level 1..3 (0x18c Elite, 0x18d WorldClass, 0x18e Legendary), else **0x18b** (Professional); base price from PlayerValues via `GetPlayerValue` | `RoundToNearest((int)(GetPlayerValue(base, CP_eCreatedPlayerGenPos[type], (rMin+rMax)/2, 0, 1) * (1 + pct/100)), 5)`; bundled pct 25/50/75/100 (note: ECONOMY_HOOKS' `CP_GetCost` is not what is charged) | yes, `CFEMessageCoinButton` (0x24bffc) shows `ms_iPlayerValue` |
| S4 | training | 0x24e3d0 | `CPlayerDevelopment::GetTrainingCost(type)` 0x20fb34 | **0x46, 0x47** exponent, **0x4c** IncStart, **0x4a** MinCost, **0x4b** MaxCost | see ECONOMY_HOOKS 4 (bundled 20..200) | yes, `CFEPlayerDevSelectButton` 0x2228f6 |
| S5 | scouting session | 0x2504ac (direct call) | `CSeason::GetCurScoutSessionCost` -> `CScoutingInfo::GetCurSessionCost` 0x36d2e6 | **0x177** InitialCost, **0x178** ExtraCost | `0x177 + 0x178*n` (n sessions used this turn; free-session roll 0x17b) | yes, `CFEMsgBoxScoutPlayer::SetupOptions` 0x2501c2 |
| S6 | friendly entry | 0x26bfc0 | `CSeason::GetFriendlyInfo()` (= season+0x6d6) byte[3], written in `CFESDreamLeagueObjectives::FriendlyQuestion` | **0x10d** Div2EntryCoins1 (bundled 30), **0x110** Div3EntryCoins1 (25), **0x113** AcademyEntryCoins1 (20); Elite/JE/Div1 have no entry fee (byte[3] = 0 -> no charge) | fee = var, **stored as a byte (strb) -> values > 255 are truncated** | yes, friendly question text (TXT_FRIENDLYTEXT4/6) |
| S7 | stadium section upgrades (batch "complete") | 0x2718f6 | total of `ms_iRelativeConstructionCost[]` minus the pitch part | per section type (from `CGfxEnv::GetSectionTypeFromSection`, table 0x63fac0 = {0,1,0,2,0,1,0,2,4,3}): **type 0 = Corners (sections 0,2,4,6): 0x51 Min, 0x52 Max, 0x53 Roof%**; **type 1 = Ends (1,5): 0x54, 0x55, 0x56**; **type 2 = Sides (3,7): 0x57, 0x58, 0x59**; types 3/4 (sections 8,9) cost -1 (not buildable) | `CSeason::GetStadiumSectionConstructionCost` 0x36d664: `c = XMATH_InterpolateClamp(capacity, typeInfo+0x2c88, typeInfo+0x2c8c, Min, Max); if model roofed (modelInfo->[0x24]->byte 0x4d) c += c*Roof/100`; construction weeks / complete-now outputs are always 0. Charged amount per section = new cost - current section cost [I] | yes, stadium screen construction cost + confirm (`CompleteStadiumUpgradesCB` is the confirm callback, ref 0x2734ac) |
| S8 | pitch pattern | 0x271974 | `ms_tStadiumStats+0xc`, set in `PitchPatternChangedCB` 0x27117e = `GetVar(tbl[pattern].var)` (tbl 0x63a038, {var, loc} x7); 0 if the pattern equals the current one | **0x27..0x2d** (HStripe, VStripe, Check, DiagonalA, DiagonalB, Diamond, Circles; bundled 200/200/250/250/250/300/350). 0x2e..0x30 are never read | flat | yes (stadium screen, same confirm as S7) |
| S9 | stadium section (single, "construct") | 0x271e72 | `ms_iRelativeConstructionCost[ms_eCurrentStadiumSection]` | as S7 | as S7 | yes, `ConfirmSectionConstruction` (ref 0x27381a) |
| S10 | kit design unlock | 0x37bc42 | `CProfileUnlockables::GetUnlockCost(e)` 0x37bc54 | **0x31 + e**, e = 0..14 (KitCostChecked..KitCostTopBand; bundled 150..500), else 0 | flat | yes, kit editor table (`CFESCustomDataEditKit::SetupTable` 0x2654a6 / `UpdateKitTypeCell`); the tap charges immediately via `ProcessUnlockItem` (0x265e84) -> `UnlockItemCallback(1)` |
| S11 | reimburse claw-back | 0x381276 | server `CDeviceCreditReimburse` record `+0x80` (when <= -1) | none | `-rec[+0x80]` | **silent** |

Setting prices at runtime via `ms_iVars[idx]` (0x75ba54 + 4*idx) works for all of S2..S10 because every cost function calls
`GetVar` at price time (no caching), with two caveats: S6 is latched into a byte when the friendly question is built; S7/S9/S8
are latched into the `CFESDreamLeagueStadium` statics when the stadium screen computes them (`UpdateStadiumCosts` 0x270a70,
`PitchPatternChangedCB`), so change vars before entering the screen.

## 3. Income callers [V]

`CMyProfile::AddCredits` 0x377984 has exactly one caller, `CCredits::AddCredits` (blx 0x2634e2). At 0x377984 entry:
`lr = 0x2634e6|1`, and the original caller's return address is at **[sp+4]** (CCredits pushed {r4,lr}, nothing else).
r1 = n (always >= 1: CCredits returns before the push when n < 1), r2 = bPurchased (= CCredits arg b1), r3 = bNoSave (= CCredits
5th arg, stack). CCredits args b2/b3 are ignored.

| key = [sp+4]&~1 (ret) | call VA | caller | category | amount (r0 to CCredits) | b1 purchased | 5th (noSave) |
|---|---|---|---|---|---|---|
| 0x203af6 | 0x203af2 | `CCore::Process` | **rewarded video (non-match)**: fires when `FE_bAddVideoCredits` && `FE_iVideoReward >= 1` (set by `CCore::VideoAdCurrencyRewardCallback` 0x203fbe; default `GetVar(7)` CreditsEarnRewardVideos 30) | `FE_iVideoReward` | 0 | 0 |
| 0x23f8c6 | 0x23f8c2 | `CFETeamManagement::SellPlayer` | **player sale** (also the only "refund" when releasing a created player: `IsCreatedPlayerID` -> `DeleteCreatedPlayer`, same path, same lr) | `CTransfers::GetSellPlayerValue(info,..)` 0x23f7ea | 0 | 0 |
| 0x249a96 | 0x249a92 | `CFEMsgAchievements::SetupAchievementRows` | **achievements + season objectives** (one call, one total): rows = newly completed achievements + an objectives row (LOC 0x43b) when `CFEMsgAchievements::ms_iObjectivesCoins >= 1`; in multiplayer mode rows come from `ms_tCreditAwardInfo` (types 6/8 skipped) | sum of `CREDITS_GetAchievementCredits(e)` + `ms_iObjectivesCoins` (= `CSeason::GetObjectivesAwards`, var 9 x completed, box opened from `CFE::Process` 0x298bfe..0x298c30) | 0 | 0 |
| 0x263a16 | 0x263a12 | `CCredits::ComeBackCredits` (from `CheckAwardFreeCredits` 0x264098) | **come-back / notification reward** | `ms_tNotifications[idx]` amount | 0 | 0 |
| 0x263b48 | 0x263b44 | `CCredits::WelcomeCreditsCallback` | **welcome bonus** (once) | `GetVar(4)` CreditsEarnWelcome 1000 | 0 | 0 |
| 0x264252 | 0x26424e | `GooglePostCallback` | **share (Google)** | `GetVar(8)` CreditsEarnShare 25 | 0 | 1 |
| 0x26429a | 0x264296 | `FacebookPostCallback` | **share (Facebook)** | `GetVar(8)` | 0 | 1 |
| 0x269234 | 0x269230 | `CFESCustomDataTeamName::EasterEggCheck` | **easter-egg team name** (once each) | egg table `[r6+8]` | 0 | 0 |
| 0x287564 | 0x287560 | `CShopHelper::CompletePurchaseCB` | **IAP purchase** | pack credits | **1** | 0 |
| 0x298cba | 0x298cb6 | `CFE::Process` | **match credits** (post-match award, pending `profile+0x2a7e8`), **plus the video doubler** `FE_iVideoReward` when `FE_bWatchedBonusVideo` (0x298c86..0x298caa adds it into r0, then clears both) | `*(profile+0x2a7e8)` (+ video bonus) | 0 | 0 |
| 0x37803c | 0x378038 | `CMyProfile::FacebookLoginComplete` | **Facebook login reward** (once; skipped if var = -1) | `GetVar(0x19)` DLOFBReward 25 | 0 | 1 |
| 0x381256 | 0x381252 | `CDeviceCreditReimburse::CheckDownload` | **server reimbursement** (positive) | `rec[+0x80]` (when > -1) | **1** | 0 |

To split achievements from objectives at the 0x249a96 key, read `CFEMsgAchievements::ms_iObjectivesCoins` at hook time
(objectives part) and book the remainder as achievements. League-position (season-end) coins are a row of the last league
match's award and arrive through the match-credits key 0x298cba (ECONOMY_HOOKS 3.1). There is no other refund path (releasing
a created player pays only the sale value above).

## 4. Coin-shop (IAP) pushes [V]

### 4.1 All `new CFEShopDialog` sites (12 BLs to `CFEShopDialog::CFEShopDialog(bool(*cb)(int), const wchar_t* text)` 0x254a9c)

Every "not enough coins" site has the same shape: `cost > GetCredits()` -> `CREDITS_ePurchaseTrigger = k` ->
`new(0x51c)` -> `CFEShopDialog(this, NULL, LOCstring(0x643 TXT_INSUFFICIENTCOINS "You do not have enough coins."))` ->
optional `SetPriority(2)` -> `CFE::AddMessageBox`; the spend branch is skipped (the purchase simply does not happen).

| # | site (ctor BL) | function | condition branch (taken = enough coins) | purchase trigger | text |
|---|---|---|---|---|---|
| P1 | 0x23e69a | `CFETeamManagement::HealSelected` | `ble` 0x23e672 | 4 | 0x643 |
| P2 | 0x24b714 | `CFEMsgCreatePlayer::Process` | `ble` 0x24b6ec | 7 | 0x643 |
| P3 | 0x24ea0c | `CFEMsgPlayerDevSelect::Process` | `ble` 0x24e9e4 | 2 | 0x643 |
| P4 | 0x25047e | `CFEMsgBoxScoutPlayer::ScoutPlayersCB` | `ble` 0x250456 | 10 | 0x643 |
| P5 | 0x251efa | `CFEMsgSignPlayer::PlayerSignedCB` | `ble` 0x251ed2 | 1 | 0x643 |
| P6 | 0x26bf90 | `CFESDreamLeagueObjectives::PlayFriendlyCallback` | `bge` 0x26bf66 | 5 | 0x643 |
| P7 | 0x2716ac | `CFESDreamLeagueStadium::CompleteStadiumUpgradesCB` | `ble` 0x271682 | 3 | 0x643 |
| P8 | 0x271d3e | `CFESDreamLeagueStadium::CompleteSectionConstructionCB` | `ble` 0x271d14 | 3 | 0x643 |
| P9 | 0x37bcb6 | `CProfileUnlockables::ProcessUnlockItem` (kit editor) | `ble` 0x37bc8e | 6 | 0x643 |
| P10 | 0x246c54 | `CFEHeaderMenu::Process` - **user taps the header coin button** (menu option 0) | `cbnz r0` 0x246c32 | - | NULL |
| P11 | 0x2639b6 | `CCredits::NotEnoughCreditsCallback` | - | - | NULL; **dead** (no refs) |
| P12 | 0x287634 | `CShopHelper::CompletePurchaseCB` (re-opens the shop after an IAP result) | - | - | NULL; unreachable once the shop is gone |

### 4.2 Recommended patch: turn CFEShopDialog into a plain OK message box (one place, covers P1-P9) [V encodings]

The ctor already builds a plain `CFEMessageBox` base (`CFEMessageBox(this, title=NULL, text=NULL, icon="fe_credit.png",
buttons=[sp]=0, cb=[sp+4], 1, 0, -1, 0x80)` at 0x254ac6) and then installs the shop vtable. The stock stadium error box at
0x271cdc uses the same ctor with `[sp] = 1` and cb NULL (an OK-able box). Patch (all in-place, no branches out):

| VA | stock bytes | new bytes | effect |
|---|---|---|---|
| 0x254ab4 | `cd e9 00 61` (`strd r6, r1, [sp]`) | `cd e9 00 01` (`strd r0, r1, [sp]`; r0 = 1 from 0x254ab2) | button arg = 1 (OK button like the stadium box) |
| 0x254ac4 | `00 22` (`movs r2, #0`) | `2a 46` (`mov r2, r5`) | description text = the caller's text ("You do not have enough coins.") |
| 0x254ad6 | `20 60` (`str r0, [r4]`) | `00 bf` (nop) | keep the `CFEMessageBox` vtable (no shop Init/Process/Render/prices) |
| 0x254ad8 | `40 f2 0a 30` (`movw r0, #0x30a` "Shop") | `40 f2 a1 60` (`movw r0, #0x6a1` "Coins") | optional: title "Coins" instead of "Shop" |
| 0x254ae8 | `0d b3` (`cbz r5, ...`) | `40 e0` (`b 0x254b6c` -> `mov r0,r4; add sp,#0x20; pop {r4-r6,pc}`) | skip the shop-only text/layout fields (+0x50c alloc, +0x470/+0x47c overrides) |

The object stays 0x51c bytes (>= 0x4dc), is added/prioritised/deleted through the `CFEMessageBox` vtable (deleting dtor
frees it normally), and all callers pass cb = NULL, so OK just closes it. P10 (header button, text NULL) would then show an
empty "Coins" box, so also apply one of:
- **remove the header coin button**: `CFEHeaderMenu::ShouldAddCreditsButton` 0x247284 `10 b5 0c 46` -> `00 20 70 47`
  (`movs r0,#0; bx lr`; only caller `CFEHeaderMenu::SetButtons` 0x247100) [V code; whether the coin balance display is part
  of that button is I - check on device], or
- **make the tap do nothing**: 0x246c32 `98 b9` (`cbnz r0, 0x246c5c`) -> `13 e0` (`b 0x246c5c`).

Per-site skipping is not recommended: each site allocates the object, constructs it and adds it in one straight-line
block, so a per-site skip needs a branch over the whole block to that function's exit (9 different exits). The ctor patch
above is smaller and keeps the player informed.

## 5. Video / ad coin grants [V]

### 5.1 Post-match doubler: `CConfig::GetMaxDoubler` 0x201678

Stock body is **not** a stub: `return ms_cUserTypeInfo[GetUserType(profile+0x6050, 1)].+0x198` (bundled DoublerMax 10000).
Patch `80 b5 08 48` -> `00 20 70 47` (`movs r0,#0; bx lr`) works as intended:
- `CFESMatchSummary::SetupCoinSummary` 0x27ae10: `cmp r0,#0; it ne; cmpne min,#0; beq 0x27ae4c` -> `sb = 0` -> the video
  button / footer doubler button (EFooterButton 0x27) is never created. **Confirmed.**
- `CFESMatchSummary::Process` 0x27ac04 (the doubler grant: `FE_iVideoForceReward = bonus`, `AddBonus`,
  `FE_bWatchedBonusVideo = 1`, `DisplayVideoAd(0,1)`) runs only when footer button 0x27 is pressed -> unreachable [I].
  Note: if it were reached with max = 0, the bonus would fall back to MinDoubler (30), not 0.
- `CFEFooterMenu::CreateFooterButton` 0x245f4c only formats "+N" for button 0x27 when it is created.
So the match-credit AddCredits (key 0x298cba) then never contains a video bonus.

### 5.2 Other rewarded-video coin grants

All go `CCore::DisplayVideoAd(bool, zone)` 0x204000 -> `CFTTRewardedVideos::PlayVideo(zone, .., VideoAdCurrencyRewardCallback)`
-> `CCore::VideoAdCurrencyRewardCallback(amount, ...)` 0x203f44: `FE_iVideoReward = FE_iVideoForceReward >= 1 ?
FE_iVideoForceReward : amount` (amount = the ad network's currency amount; UI labels show `GetVar(7)` 30),
`FE_bAddVideoCredits = 1` -> paid by `CCore::Process` at **0x203af2** (income key 0x203af6) when the current screen type is
not 1/5/8 (the match summary's doubler is instead added by CFE::Process, see 3).

| DisplayVideoAd caller | what it is |
|---|---|
| 0x21d284 `CFEFooterVideoAdButton::Process` | footer "watch video for coins" button (label `GetVar(7)` 0x21d1ea) |
| 0x223dd8 `CFEShopButton::Process` | free-coins video tile inside the shop (gone with the shop patch) |
| 0x226ed2 `CFEVideoButton::Process` | generic video button (label `GetVar(7)` 0x22718a) |
| 0x26a272 `CFESDreamLeagueHub::DisplayMessage` | hub pop-up offering a video |
| 0x27e8d2 `CFESPauseMenu::Process` | pause-menu video |
| 0x27ac62 `CFESMatchSummary::Process` | the post-match doubler (5.1) |
| 0x2040b4 (inside DisplayVideoAd) | `CFEMsgWatchVideo` prompt (label `GetVar(7)` 0x253c9e) |

To stop every non-match video payout with one patch: 0x203ae6 `10 db` (`blt 0x203b0a`) -> `10 e0` (`b 0x203b0a`), i.e.
CCore::Process never credits `FE_iVideoReward` [V encoding]; or book/zero it in the income hook (key 0x203af6). The offline
tablet has no ad fill, so these most likely never fire anyway [G].

## 6. Stadium / attendance [V unless tagged]

- **Attendance is valid at `SetupCreditAwardInfo`.** `CMatchSetup::MatchReset` 0x2eeb30 memclr's all of `ms_tInfo`
  (0xfc8 bytes, so +0xf64 = 0); it is called in `CFlow::Process` step 2 at 0x298e26 **immediately before**
  `MCU_SetupTournamentMatch(user fixture)` 0x298e42, which for a user home match (`home == GetUserTeamID`) sets attendance
  = `CSeason::CalculateAttendance(season, home, away)` (call 0x36427c) via `SetMatchEnvironment` (`SetAttendance` 0x2ef7fc =
  `ms_tInfo+0xf64 = r0`). The other MatchReset callers (StartInitialFlow 0x2050b8, MyClub::Process 0x26b060, replay viewer,
  NIS test, network flow) are not between kick-off and `MCU_TournamentEndOfMatchCallback`, which calls
  `SetupCreditAwardInfo` 0x239c58 right after `PlayTurn`. Played and simulated user matches both go through step 2 [I for
  the simulate path]. Away: `(75 + Random(25))%` of the opponent's capacity; neutral: team 0xd4's stadium (85,479).
- Attendance formula (ECONOMY_HOOKS 2): `cap * (fan*20 + homeStars*8 + awayStars*8) / 100`, fan = approval 0..1, stars 0..5,
  `cap = GetStadiumCapacity(season, 0)` (built sections only).
- **New-career starting stadium**: `MCU_InitFreshProfile` 0x3648d4 -> user team id **0x102** -> `CSeason::NewSeason` ->
  `ResetStadiumInfo` 0x36d762 copies team 0x102's DB env config (teams.dat record index 119): sections
  corner_x_0_e (0) / end_e_1_a (1323) / corner_x_0_l (0) / side_e_1_a (1908) / x2 -> **capacity 6,462** (no corners built)
  [V data, capacity table below]. Typical first-season attendance at fan 0.5 and 1-3 star teams: ~26-58% -> ~1,700-3,700.
- **Section capacities** (static table `CGfxEnv` model descriptions at 0x745b70, 0x81 entries x 0x50: name +4, capacity
  +0x48, roof flag byte +0x4d): corners (sections 0,2,4,6) **722 .. 10,692** (x_0 placeholders = 0); ends (1,5) **1,323 ..
  9,722**; sides (3,7) **1,908 .. 14,122**. The per-type min/max capacity (`typeInfo+0x2c88/+0x2c8c`, used by the section
  cost interpolation) are the smallest non-zero and the largest model capacity of that type. Max stadium =
  4x10,692 + 2x9,722 + 2x14,122 = **90,456**. Promotion minimums (vars 0x40..0x44, bundled): Elite 80,000, Prestige
  60,000, Div1 45,000, Div2 30,000, Div3 15,000 (ECONOMY_HOOKS 4). Stock stadium bonus at start = round(6462/1750) = 4 coins.
