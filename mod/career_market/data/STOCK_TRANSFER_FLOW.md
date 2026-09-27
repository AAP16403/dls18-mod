# DLS18 stock transfer flow (static analysis, libDLS18.so v5.064 armeabi-v7a)

Status: complete (sections 1-7). Static analysis only (plus Unicorn emulation of two pure functions); nothing runtime-tested.
All addresses are stock-lib VAs of Thumb code (add 1 when calling). Tools: `analysis/armdis.py`, `analysis/xref.py`,
plus a scratch GOT-resolving disassembler (resolves `ldr rX,[pc,#]; add rX,pc` to symbol names).

NOTE: the bundled `unpacked/apk/lib/armeabi-v7a/libDLS18.so` is itself patched at `CMyProfile::GetCredits` 0x3769c0
(`mvn r0,#0xff000000; bx lr` = always 16,777,215 coins). `build_mod.py` (GET_CREDITS) restores the stock body
`movw r1,#0xa7cc; movt r1,#2; ldr r0,[r0,r1]; bx lr` => real balance = `*(int*)(MP_cMyProfile + 0x2A7CC)`.
Every coin check below goes through GetCredits, so the coin economy only works with that restore in place.

## 1. Buy flow (stock Dream League transfer UI)

### 1.1 Screens / classes involved

| class | role | key functions |
|---|---|---|
| `CFESDreamLeagueTransfers` (FE screen, vtable 0x720a20) | the big transfer-search screen (player table + filter) | `OnScreenEnter` 0x27646c, `Init` 0x2765bc, `Process` 0x276954, `ProcessResults` 0x276d90, `SetupResults` 0x276bb4, `CurrentPlayerBid(CFEPlayerCard*)` 0x276fb8, `SellPlayer` 0x276f4c, `RefreshResults` 0x276518 |
| `CFETablePlayerCellTransfers` | one row/card of the result table | ctor 0x256c3c (from `TPlayerSearchInfo`) |
| `CFETransferOptionsMenu` / `CFETransferFilter` / `CFETransferSearchMinMax` | search filter UI | 0x24868c / 0x2445cc / 0x244dc8 |
| `CFETransfersButton` | hub carousel of random transfer cards | `Init` 0x226b18, `CFEPlayerCardCarousel::GetRandomTransferCards` 0x238718 |
| `CFEMsgSignPlayer` (message box, vtable 0x71d758) | the buy confirmation dialog (card, price, "Sign") | ctor 0x251300, `PlayerSignedCB(int)` 0x251dd8, `CaptainSignedCB(int)` 0x2518b4, `SetMode` 0x2516b8 |
| `CFEMsgBoxScoutPlayer` / `CFEMsgScoutResults` | scouting purchase and results (sign a scouted player) | 0x25003c / 0x250558, `CFEMsgScoutResults::Process` 0x2508ec |
| `CFESSelectCaptain` | new-career captain pick (free signing) | `Process` -> CFEMsgSignPlayer at 0x28713e |
| `CFETeamManagement` / `CFEMsgSellPlayer` | squad screen / sell confirmation | see section 2 |

### 1.2 Callers (complete, from BL/BLX scan)

- `CFEMsgSignPlayer::CFEMsgSignPlayer(TPlayerInfo*, int teamID, int srcTeamID, bool, ESignPlayerMode, bool(*)(int))`:
  `CFESDreamLeagueTransfers::CurrentPlayerBid` 0x277042, `CFEMsgScoutResults::Process` 0x250a68,
  `CFESSelectCaptain::Process` 0x28713e.
- `CTransfers::SignPlayerAttempt(const TPlayerInfo&, int fromTeam, int cost)` 0x21033c: **only**
  `CFEMsgSignPlayer::PlayerSignedCB` 0x251f24.
- `CDataBase::SignPlayer` 0x20cbd4: `SignPlayerAttempt` 0x2104a8, `CFEMsgSignPlayer::CaptainSignedCB` 0x251ae6
  (captain, free), `CDataBase::NewDreamTeam` 0x20d474, `CDataBase::VerifyLink` 0x209edc (free top-up to 16).
- `CCredits::SubtractCredits(int)` 0x2633bc: `SignPlayerAttempt` 0x210552 (the only transfer-related one); others are
  heal 0x23e6de, create player 0x24ab28, player dev 0x24e3cc, friendly 0x26bfbc, stadium 0x2718f2/0x271970/0x271e6e,
  unlockables 0x37bc3e, reimburse 0x381272. (Scouting pays with `CMyProfile::SubtractCredits` directly in `CFEMsgBoxScoutPlayer::ScoutPlayersCB`, see 1.3 B.)

### 1.3 Button press -> SignPlayerAttempt, step by step

A. **Transfer search screen** (`CFESDreamLeagueTransfers`):
1. `Process` 0x276954 -> `ProcessResults` 0x276d90 walks the result grid (`CFELayoutTable::GetCell`), calls the card's
   vtable+0x98 (pressed) and on a press calls `CurrentPlayerBid(card)` at 0x276f22.
2. `CurrentPlayerBid(CFEPlayerCard* card)` 0x276fb8 (r4 = card):
   - memcpy 0xaf bytes `card+0x294` (the card's `TPlayerInfo`) -> `CFESDreamLeagueTransfers::m_tSelectedPlayer` 0x763bac.
   - `r5 = CFEPlayerCard::GetAvailable(card)` (= byte `card+0x285`).
   - created player id -> `CFEMsgCreatePlayer` path (0x276fe4..0x277002), not a transfer.
   - **not available** (0x277004 `cbz r5`) -> message box LOC 0x7f9 with the player's name (0x277048..).
   - available -> 0x277006: `new(0x4e0)`; `CFEMsgSignPlayer(box, &m_tSelectedPlayer, card->GetTeamID() /*card+0x344*/,
     card->GetTeamID(), (bool)card[0x35c], mode 0, CFEMsgSignPlayer::PlayerSignedCB)`; `CFE::AddMessageBox` 0x2770dc.
3. `CFEMsgSignPlayer` ctor 0x251300 stores statics: `ms_pPlayerInfo` 0x7601d8 = info, `ms_iTeamID` 0x741d7c = teamID,
   `ms_iSourceTeamID` 0x741d80 = srcTeam, `ms_eMode` 0x741d84 = mode, `m_bCreatePlayer` 0x7601ec. Price:
   - `this+0x4dc` (secret flag) = 1 if `info->id == CTransfers::GetSecretPlayerInfo()->+0x1c` (secret player id), or if it
     is the secret-player turn, the player is not scouted and he is the POTW current-turn id (0x2513a2..0x2513e2).
   - if the current FE screen id (+0xec) == 0x10 (captain select) -> value 0; else
     `ms_iPlayerValue` 0x7601dc = `CTransfers::GetPlayerValue(info, -1 /*genPos*/, -1 /*rating*/, 1 /*bRandom*/, 1 /*bSecretTurnDiscount*/)` (0x251430).
   - secret flag and not the secret-player turn -> `ms_iPlayerValue = secretInfo->price (+0xc)` (0x25144e).
   - created player -> `GetCreatedPlayerValue()`. Copied to `this+0x47c`; text "cost: %s" LOC 0x4a5.
4. User presses Sign -> `CFEMsgSignPlayer::PlayerSignedCB(int button)` 0x251dd8:
   - button -1 (cancel) -> return 0. Mode 2 (post-sign share screen) -> social share handling.
   - button 1 && mode 0 (0x251e98): created player -> CFEMsgCreatePlayer. Otherwise the **coin check**:

```
0x251ebe  ldr r0,=&ms_iPlayerValue ; ldr r1,=&MP_cMyProfile   (pc-relative literals, then add rX,pc)
0x251ec8  ldr r4,[ms_iPlayerValue]
0x251ecc  blx CMyProfile::GetCredits(MP_cMyProfile)
0x251ed0  cmp r4, r0
0x251ed2  ble 0x251f0c                   ; enough coins
          CREDITS_ePurchaseTrigger = 1; new CFEShopDialog(NULL, LOC 0x643), SetPriority(2), AddMessageBox; return 1
0x251f0c  r0 = *ms_pPlayerInfo, r1 = *ms_iTeamID, r2 = *ms_iPlayerValue
0x251f24  blx CTransfers::SignPlayerAttempt          ; <- only caller
0x251f28  movs r5,#1 ; cmp r0,#0 ; beq 0x25201c      ; failure -> return 1 (dialog closes)
          success: RemovePlayerFromSearch(id), CFESDreamLeagueTransfers::ms_bSetupResults = 1,
          CSeason::AdjustFanApprovalRating(0.005f), CFESDreamLeagueHub::ms_bReInitTransfersButton = 1,
          ms_eNewMode = 2 (share screen), return 0 (dialog stays open; CFEMsgSignPlayer::Process switches mode)
```

   The "fromTeam" passed to SignPlayerAttempt is `ms_iTeamID` (== card team id); `ms_iSourceTeamID` is unused here.
   Callback return value: 1 = the message box closes, 0 = it stays (cancel returns 0 because the box handles cancel itself).

B. **Scouting**: footer button 0x2a in `CFESDreamLeagueTransfers::Process` -> `CFEMsgBoxScoutPlayer(ScoutPlayersCB)`.
   `ScoutPlayersCB` 0x250428: `cost = CSeason::GetCurScoutSessionCost()` (`CScoutingInfo::GetCurSessionCost` 0x36d2e6 =
   `InitialCost(var 0x177) + ExtraCost(0x178) * sessionIndex`, 0 when a free session is pending); `cost > GetCredits()` ->
   shop dialog; else `CSeason::ScoutPlayers(pos)` and **`CMyProfile::SubtractCredits(cost,1)` at 0x2504a8** (direct, not via
   CCredits), then `CFEMsgScoutResults`. `CFEMsgScoutResults::Process` 0x2508ec: for the pressed result card
   `CanAddPlayer(user, card+0x294, -2)`; 0 -> "squad full"; else `CFEMsgSignPlayer(&card->info, card->GetTeamID(),
   card->GetTeamID(), 0, mode 0, PlayerSignedCB)` (0x250a68) -> same PlayerSignedCB path as A.

C. **Captain pick** (new career, `CFESSelectCaptain::Process` 0x2870c0): `CFEMsgSignPlayer(&m_tCaptains[i], 0x102, team, 0,
   mode 1, CaptainSignedCB)`, price 0. `CaptainSignedCB` 0x2518b4 calls `CDataBase::SignPlayer` directly (0x251ae6). Not a
   market purchase; leave alone.

D. Hub carousel (`CFETransfersButton`, `CFEPlayerCardCarousel::GetRandomTransferCards` 0x238718) only displays cards (it
   reads GetCredits at 0x23883a to choose cards); it never creates CFEMsgSignPlayer (only the 3 ctor callers above).
   The POTW footer button (0x2c) opens `CFEMsgPOTW`, not a direct buy. `CFESDreamLeagueTransfers::SellPlayer` 0x276f4c
   (footer option 9) only opens the team-management screen in sell mode (`CFE::Forward(4, 1, 2, ...)`), see section 2.

### 1.4 `CTransfers::SignPlayerAttempt(const TPlayerInfo& info /*r0*/, int fromTeam /*r1*/, int cost /*r2*/)` 0x21033c

Prologue `push.w {r4-r11,lr}; sub sp,#4; vpush {d8}; sub.w sp,sp,#0x2e0`; r8 = info, `[sp+0x18]` = fromTeam,
`[sp+0x1c]` = cost. **No coin check inside**; the only check is in PlayerSignedCB (above).
Order: re-ExpandTeam of the current fixture slot (0x2103ee) -> `CanAddPlayer(user, info)` (0 -> "squad full" box
LOC 0x49b/0x3fc, return 0; 1 -> return 0) -> `SignPlayer(info, fromTeam, GetTeamSpecificData(fromTeam,id), 1, 0, 1)` 0x2104a8
-> `CPlayerDevelopment::AddPlayer` -> season player state byte2 = 1 -> dynamic difficulty -> ExpandTeam again ->
`GetPlayerInfo` -> **`CCredits::SubtractCredits(cost)` at 0x210552** (r0 = r5 = `[sp+0x1c]`) ->
`CDreamLeagueStats::CheckMostExpensiveTransfer / IncNumSignings` -> achievements (cost >= 1501 -> 0xe; team value % >= 50/80)
-> social score -> **`CMyProfile::Save(3)` 0x21062e** -> analytics -> return 1 (0x210696).
Epilogue at 0x21069a: `add.w sp,sp,#0x2e0; vpop {d8}; add sp,#4; pop.w {r4-r11,pc}`.

`CCredits::SubtractCredits(int)` 0x2633bc = `CMyProfile::SubtractCredits(MP_cMyProfile, n, 1)`, clamps the balance to >= 0 via
`SetCredits`, animates the coin counter, SFX 0xe. `CCredits::AddCredits(int, bool, bool, bool, bool)` 0x2634c0: returns at once
if n < 1; `CMyProfile::AddCredits(MP_cMyProfile, n, b1, b3)` + counter animation.

### 1.5 Price sources

`CTransfers::GetPlayerValue(TPlayerInfo* info, int genPos, int rating, bool bRandom, bool bSecretTurnDiscount)` 0x212b3c
(verified: r1 = genPos, r2 = rating; -1 = take from info):
1. `base = MinValue + (int)(powf((clamp(r,MinR,MaxR)-MinR)/(MaxR-MinR), ExpWhole + ExpTenths/10) * (MaxValue-MinValue))`,
   CConfig vars 0x15b+4*genPos.. (Gk/Def/Mid/Att Min/MaxRating, Min/MaxValue), exponent 0x16b/0x16c.
2. **Scouted players cost more**: if `CSeason::IsPlayerScouted(id)`: `v = base * (1 + PricePercentIncrement(var 0x179)/100)`
   (bundled 20 -> +20%). There is no scouting discount.
3. `bRandom`: seed = `id + seasonCount + matchesPlayed*100`; `v += -base/100 + Random(base/50)` (about +/-1%, re-rolled every match).
4. `RoundToNearest(v, 5)`.
5. if `CTransfers::ms_bSecretPlayerTurn` && bSecretTurnDiscount: `v -= v * secretInfo.discountSPW(+0x14) / 100`, round to 5.

`CTransfers::SetupTurn` 0x21098c: `ms_bSecretPlayerTurn = (active tournament index == 0 /*league*/ &&
league->GetCurRound() == CConfig var 0x199 "Messages/SecretPlayer")` (bundled 9 -> league round 9 only).
`CTransfers::SetupSecretPlayerInfo` 0x211188 fills `ms_tSecretPlayerInfo` 0x75ce50: +0 genPos, +4 min rating, +8 max rating,
+0x10 discount% in [MinPercentageDiscount 0x182, Max 0x183] (bundled 5..15), +0x14 SPW discount% in [0x184, 0x185]
(bundled 15..25), +0x18 exists flag, +0x1c player id. `SetupSecretPlayer` 0x212df8 sets +0xc price =
`value - value*disc(+0x10)/100`, rounded to 5.

Card display: `CFEPlayerCard::GetPlayerValue` 0x2343ac: buy cards (flag `card+0x288` bit4) show `GetPlayerValue(info,-1,-1,1,1)`
(same as the dialog), other cards show `GetSellPlayerValue(info,-1,-1)`; secret cards (`+0x288` bit7) show `secretInfo+0xc`
outside the secret turn.

**Charged price = `CFEMsgSignPlayer::ms_iPlayerValue` (0x7601dc) = the price shown in the dialog.**

## 2. Sell flow (stock)

Only one code path sells a player for coins: the team-management screen in sell mode. (`CDataBase::SellPlayer` 0x20cc68 has
a single caller, `CFETeamManagement::SellPlayer` 0x23f8e2. `CFESDreamLeagueTransfers::SellPlayer` 0x276f4c just opens that
screen: `ExpandTeam(user)`, `CFESTeamManagement::SetTeam`, `CFE::Forward(4, 1, 2 /*mode*/, 0, 1, 0)`.) There is no release /
free-transfer feature. Created players are "sold" through the same path (`DeleteCreatedPlayer` 0x20cd88) and also pay out.
No other `CCredits::AddCredits` caller is transfer-related (the rest are IAP, rewards, achievements, welcome, social, easter egg).

Step by step:
1. `CFETeamManagement::Process` 0x23e3b8, `tbb` on `this+0xfc` (screen mode): case 2 = **sell mode** (0x23e3ee).
   `ProcessSelect`, then if the selected card `this+0x100` reports pressed (vtable+0x98) ->
   0x23e404 `new(0x4e0)`; `CFEMsgSellPlayer(box, card+0x294 /*TPlayerInfo*/, CFETeamManagement::SellPlayerCB)` 0x23e41e; AddMessageBox.
2. `CFEMsgSellPlayer` ctor 0x251160: title LOC 0x67b, `GetSellPlayerValue(info,-1,-1)` (0x251182) shown as "cost: %s"
   (0x2511e0), button flags 0x100008, player card. Static `CFEMsgSellPlayer::ms_pPlayerInfo` 0x7601d4 = info.
3. `SellPlayerCB(int button)` 0x23edb4: button 1 -> `CFETeamManagement::m_bSellSelectedPlayer = 1`; returns 1 (box closes).
4. Next `Process` frame: flag set -> `SellSelection()` 0x23edcc (call at 0x23e436), then flag cleared.
5. `SellSelection` 0x23edcc: **`CanSellPlayer()` first** (call at 0x23edf2; 0 -> return at 0x23f050, nothing changed).
   Then it swaps the best same-position bench player into the leaver's lineup slot (`AttemptSwap`), calls
   **`CFETeamManagement::SellPlayer()` 0x23efe8**, `DeletePlayerCard`, `CTeam::SetTeamMan`, `CTeam+0x148--`, `ExpandTeam`,
   `CFESDreamLeagueTransfers::ms_bSetupResults = 1`.
6. `CanSellPlayer` 0x23f3e8 (bool, r0 = this): info = `[this+0x100]+0x294`; created player -> `CanDeleteCreatedPlayer`, else
   `CTransfers::CanRemovePlayer(user, info)`; result 0 -> message LOC 0x1bd (squad would drop below 16), 1 -> LOC 0x3e2
   (last goalkeeper); shows `CFEMessageBox(title LOC 0x564, text, 0, 1, NULL, 0, 0, -1, 0x80)` and returns 0; 2 -> returns 1.
7. `CFETeamManagement::SellPlayer` 0x23f7b4 (r0 = this):
   ```
   0x23f7bc  r5 = card = [this+0x100]
   0x23f7d0  spec = *GetTeamSpecificData(user, card->info.id)  -> [sp+8]
   0x23f7ea  r8 = price = CTransfers::GetSellPlayerValue(sb = card+0x294, -1, -1)
   0x23f7f6  created player -> DeleteCreatedPlayer(info, spec, 1) -> skip to 0x23f8aa
   0x23f804  sl = PU_GetPlayerRating(id); random team order (XMATH_CreateRandomIndexArray over GetTeamCount())
             find team r6: IsValidSearchTeam && TeamExists && |GetTeamRating(r6) - sl| < r5 (r5 starts 5, +1 per full pass)
   0x23f8da  found: CDataBase::SellPlayer(info=sb, buyer=r6, &spec, 1)  (call at 0x23f8e2; buyer is cosmetic, see ROSTER_MECHANICS)
   0x23f886  CDLSAnalytics::LogCreditSpend(7, price, id); CDreamLeagueStats::CheckMostExpensiveSale(id, price)
   0x23f8b2  CMyProfile::BeginTransaction
   0x23f8c2  CCredits::AddCredits(price, 0, 0, 1, [sp]=0)          <- coins added here
   0x23f8ca  CDreamLeagueStats::IncNumSales ; 0x23f8d0 CMyProfile::EndTransaction ; return
   ```
   (No `CMyProfile::Save` call here; the transaction end / later saves persist it.)

Price: `CTransfers::GetSellPlayerValue(TPlayerInfo*, int genPos, int rating)` 0x21316c =
`RoundToNearest((int)(GetPlayerValue(info, genPos, rating, 1 /*bRandom*/, 0 /*no secret discount*/) * SellPlayerPercent(var 0x16d)/100), 5)`
(bundled 50 -> half the buy value; the scouted +20% surcharge also applies to the sale value if the player was scouted).
Callers: card display `CFEPlayerCard::GetPlayerValue` 0x2343c6, `CFETeamManagement::SellPlayer` 0x23f7ea,
`CFEMsgSellPlayer` ctor 0x251182 / 0x2511e0.

## 3. Hook points

Conventions (same as `build_mod.py`): overwrite 4 bytes with `b.w cave` (or a 4-byte `blx` call with `bl cave`), re-execute
the displaced instructions in the cave, return with `b.w <hook+4>` or an unconditional `b.w` to a stock label. Follow
`mod/PATCHING_RULES.md`: only PC-relative `b.w`/`bl` out of the cave, no far conditional branches, the market dispatch
(`cave_market_dispatch`, dlopen/dlsym per call) must preserve whatever registers the stock code still needs (it clobbers
r0-r3, r12 and restores r4-r7). None of the windows below contains a branch target (checked: no branch in the host function
lands on hook+2), and every displaced instruction listed is position-independent unless noted.

### 3.1 Buy: gate before the confirmation dialog (block with a message)

(a) Search screen, `CFESDreamLeagueTransfers::CurrentPlayerBid` **0x277006** (bytes `4f f4 9c 60` = `mov.w r0,#0x4e0`;
    next `movs r1,#0` 0x27700a, `movs r2,#0` 0x27700c). Reached only for an available, non-created player.
    - Registers: r4 = `CFEPlayerCard*`; TPlayerInfo* = r4+0x294 (already copied to `m_tSelectedPlayer` 0x763bac);
      source team = `[r4+0x344]` (`CFEPlayerCard::GetTeamID`); r5 = available flag (nonzero); r6-r8 free.
      Price is not computed yet: call `GetPlayerValue(info,-1,-1,1,1)` (0x212b3c|1) yourself if needed (secret player:
      `CTransfers::ms_tSecretPlayerInfo+0xc` 0x75ce5c when the card is secret and it is not the secret turn).
    - Stock continue: `mov.w r0,#0x4e0` then `b.w 0x27700a`.
    - Block: show the mod message box, then `b.w 0x2770e0` (stock epilogue `add.w sp,sp,#0x418; pop.w {r4-r8,pc}`; the
      stack is untouched at this point).
(b) Scouting results, `CFEMsgScoutResults::Process` **0x250a3c** (same bytes `mov.w r0,#0x4e0`; next `movs r1,#0; movs r2,#0`).
    - Registers: r6 = `CFEPlayerCard*`, r7 = TPlayerInfo* (r6+0x294), source team = `[r6+0x344]`, sb = card index,
      sl = the CFEMsgScoutResults box, r4 = &cards[sb], r8 = 0.
    - Stock continue: `mov.w r0,#0x4e0` then `b.w 0x250a40`. Block: `b.w 0x250ac2` (continue the card loop; no stack change).
      To also close the results box call `CFE::DeleteActiveMessageBox` like the squad-full path at 0x250a6e.
Both gates run before any money check, so the IAP shop dialog never appears for a blocked purchase.

### 3.2 Buy: divert the purchase (one hook covers search, scouting and secret player)

**`CFEMsgSignPlayer::PlayerSignedCB` 0x251f24** (bytes `76 f7 5c ea` = `blx CTransfers::SignPlayerAttempt` PLT) -> `bl cave_buy`.
- Registers at the call: **r0 = const TPlayerInfo*** (search screen: `&m_tSelectedPlayer`; scouting: result card+0x294),
  **r1 = source team id** (`ms_iTeamID` 0x741d7c), **r2 = price** (`ms_iPlayerValue` 0x7601dc, already known to be <= coins),
  r4 = 1 (button), r5 = 0, r6 = &ms_pSparkleAnim slot; lr is set by the `bl` to 0x251f28|1.
- The cave is a normal function (push/pop what it uses). Return r0:
  - 0 -> PlayerSignedCB returns 1: the sign dialog closes, nothing else happens (use this for "blocked" or "deferred to
    mod negotiation"; queue the mod's own box with AddMessageBox before returning, as the stock code does at 0x251f06).
  - nonzero -> stock success tail: `RemovePlayerFromSearch(id)`, `ms_bSetupResults = 1` (table refresh), fan approval +0.005,
    hub transfer button re-init, dialog switches to the "signed / share" mode. Use it when the mod completed the signing itself.
  - stock behaviour: tail-call `b.w 0x21033c` (SignPlayerAttempt, Thumb, reachable with b.w) with r0-r2 intact.
- If the mod completes the purchase itself it must reproduce what SignPlayerAttempt does: `CanAddPlayer(user, info, -2)`,
  `CDataBase::SignPlayer(info, from, GetTeamSpecificData(from,id), 1, 0, 1)` 0x20cbd4, `CPlayerDevelopment::AddPlayer(id,0)`
  0x20e130, season player state byte2 = 1, re-ExpandTeam of the current fixture slot if a user match is pending,
  `CCredits::SubtractCredits(price)` 0x2633bc (or its own amount), `CMyProfile::Save(3)` 0x375b88. Calling stock
  SignPlayerAttempt with a modified r2 is the simplest way to charge a negotiated price.
- Limitation: the stock coin check (0x251ebe..0x251ed2) runs before this hook with the stock price; if the user cannot afford
  it the IAP `CFEShopDialog` opens instead. To replace the check too, hook **0x251ebe** instead (bytes `6a 48 6a 49` =
  two pc-relative `ldr` literals; do not replay them): the cave loads the three statics itself (PC-relative from the cave:
  `adr` anchor + link-time delta to the GOT slots, or read `base + 0x7601d8/0x741d7c/0x7601dc` in C), then
  `b.w 0x25201a` (return 1, close) to block, or sets `r0 = *ms_pPlayerInfo, r1 = *ms_iTeamID, r2 = price` and `b.w 0x251f24`
  to continue into SignPlayerAttempt (r4/r5/r6 are not needed after 0x251ebe except r5, which 0x251f28 sets).

Alternative single point: `CTransfers::SignPlayerAttempt` entry **0x21033c** (bytes `2d e9 f0 4f` = `push.w {r4-r11,lr}`),
same r0/r1/r2; stock: `push.w {r4-r11,lr}; b.w 0x210340`; block: `movs r0,#0; bx lr` (nothing pushed yet). Equivalent to
3.2 because PlayerSignedCB is its only caller.

### 3.3 Sell: gate before the confirmation dialog (block with a message)

**`CFETeamManagement::Process` 0x23e404** (bytes `4f f4 9c 60` = `mov.w r0,#0x4e0`; next `movs r1,#0` 0x23e408, `movs r2,#0` 0x23e40a).
- Registers: r4 = `CFETeamManagement*` this; card = `[r4+0x100]`; **TPlayerInfo* = card+0x294**; seller = user (0x102);
  price = `GetSellPlayerValue(info,-1,-1)` (0x21316c|1) if the mod needs it; r0 = vcall result (nonzero), r1-r3 scratch.
- Stock continue: `mov.w r0,#0x4e0` then `b.w 0x23e408` (only the first 4 bytes are displaced).
- Block / divert to the mod's own negotiation UI: `b.w 0x23e426` (skips the sell dialog; `m_bSellSelectedPlayer` is 0 so
  Process falls through to its epilogue at 0x23e574).

### 3.4 Sell: veto or divert at confirmation time

(a) `CFETeamManagement::SellSelection` **0x23edf2** (bytes `87 f7 7e ee` = `blx CanSellPlayer` PLT) -> `bl cave_cansell`.
    r0 = this; info = `[r0+0x100]+0x294`. Return 0 -> SellSelection returns without touching anything (clean veto after the
    user confirmed); stock: tail-call `b.w 0x23f3e8` (CanSellPlayer) with r0 intact. Do not remove the player yourself and
    return 0: the screen would keep a stale card (SellSelection does the card/lineup bookkeeping after SellPlayer).
(b) Price only: `CFETeamManagement::SellPlayer` **0x23f7ea** (`blx GetSellPlayerValue`) -> `bl cave_sellprice`:
    r0 = info (also in sb), r1 = r2 = -1, fp = player id after 0x23f7e6. Return the mod's price in r0 (it becomes r8, the
    amount added at 0x23f8c2). For a consistent display also patch the two dialog calls 0x251182 / 0x2511e0 and the card
    call 0x2343c6, or hook `GetSellPlayerValue` entry 0x21316c (bytes `10 b5 82 b0` = `push {r4,lr}; sub sp,#8`; stock resume:
    replay both, `b.w 0x213170`) to change all four at once (it is called while rendering cards; keep the cave cheap, i.e. do not
    go through the dlopen/dlsym dispatch on every call).
(c) Coins: `CFETeamManagement::SellPlayer` **0x23f8c2** (`blx CCredits::AddCredits`) -> `bl cave_sellcoins`:
    r0 = price (= r8), r1 = 0, r2 = 0, r3 = 1, `[sp]` = 0 (5th arg), sb = TPlayerInfo* (card memory, still valid), fp = player id,
    r4 = MP_cMyProfile, r6 = buyer team picked by the rating loop (undefined for created players), r7 = team index array
    (freed already, do not use). Push/pop around the mod call, restore r1-r3 and sp, then `b.w 0x2634c0` (AddCredits tail call;
    the `[sp]` argument is then still in place) with the amount to credit; amount < 1 credits nothing (AddCredits returns early),
    which lets the mod route the money into its own finance instead.
(d) Destination club: `CFETeamManagement::SellPlayer` **0x23f8e2** (`blx CDataBase::SellPlayer`) -> `bl cave_selldb`:
    r0 = TPlayerInfo*, r1 = buyer team (stock random pick, r6), r2 = &spec (`sp+8`), r3 = 1. The mod can move the player to its
    chosen buyer in the default links table (ROSTER_MECHANICS.md section 8C) and then tail-call `b.w 0x20cc68`. Not reached for
    created players.

## 4. Showing a simple message box from native code

Stock pattern (CurrentPlayerBid 0x27707a..0x2770dc, SignPlayerAttempt 0x21044a..0x210482, CanSellPlayer 0x23f440..0x23f47c):
```
box = operator new(0x4dc, 0, 0)                  // _Znwj13EFTTMemHeapIDi, PLT 0x1c06ac (ARM stub: even address)
CFEMessageBox::CFEMessageBox(box,                 // C1/C2 0x248aa8 (Thumb: |1)
    const u16* title,                            // r1  (stock: LOCstring(id), 0x1c06a0 PLT)
    const u16* text,                             // r2  -> SetDescriptionText
    const char* r3 = NULL,                       // r3  (passed to CFEArea)
    int  buttons    = 1,                         // [sp+0x00] -> box+0x448, button flags; 1 = the single OK button of stock notices
    bool (*cb)(int) = NULL,                      // [sp+0x04] -> box+0x40c
    bool b1 = 0,                                 // [sp+0x08] -> box+0x45d
    bool b2 = 0,                                 // [sp+0x0c] -> box+0x478
    int  i1 = -1,                                // [sp+0x10] -> box+0x47c
    int  i2 = 0x80);                             // [sp+0x14] -> CFEArea int arg
CFE::AddMessageBox(box);                          // 0x298608 (|1): CFEEntityManager::GetMessageBoxQueue()->AddMessage(box)
```
The queue owns and deletes the box. Strings are 16-bit wide strings (the mod already uses `uint16_t` buffers); they must stay
alive while the box is shown (use static buffers, as `native_bridge.c` does).

The mod already has an equivalent: `ui_show_notice()` in `native_bridge.c` builds a `CFEMessageBoxOptions` (size 0x4e0,
ctor 0x24dd58: `(box, u16* options, int count, int stride, cb, u16* title, u16* desc, bool)`) with one "Continue" option and
calls AddMessageBox. Either works from inside another box's callback (the stock code adds boxes from callbacks, e.g.
PlayerSignedCB 0x251f06); the new box is queued and shown after the current one closes.

## 5. Coin income (post-match awards) and stock prices

### 5.1 How the post-match coins are computed and credited

`CFEPostMatchCreditAwards::SetupCreditAwardInfo` 0x239c58 builds `ms_tCreditAwardInfo` rows (stride 0x208: +0 row type,
+0x204 coins). Sources:
- Result: `CREDITS_GetMatchWinCredits` 0x263bac / `...DrawCredits` 0x263c40 / `...LossCredits` 0x263cd4 -> per tournament id
  `CREDITS_GetTournMatchWin/Draw/LossCredits(tid)` 0x263b84 / 0x263c18 / 0x263cac -> `CConfig::GetVar(table[tid])`
  (tables 0x639dd0 / 0x639e20 / 0x639e70). Tid 6 (playoffs) uses the league's tid. Online (DLO) matches use
  `CMultiplayerInfo::GetCoinReward` instead.
- Goals: `CREDITS_GetMatchGoalCredits(tid)` 0x263d40 (table 0x639ec0) x user goals.
- Clean sheet: `CREDITS_GetMatchCleanSheetCredits(tid)` 0x263d88 (table 0x639f10).
- Stadium bonus: `CSeason::GetStadiumBonus(cap)` 0x36d548 = `round(capacity / StadiumBonusDivisor(var 0x50))`.
- Achievements (`CREDITS_GetAchievementCredits` 0x263b58, table 0x639cd0), DLO season completion, and tournament
  credits `CSeason::GetTournamentCredits` / `CREDITS_GetTournamentCredits` 0x263dd0: league final position
  (`CREDITS_GetLeaguePosCredits(tid,pos)` 0x263b70 = var `0xa7 + tid*16 + pos`, paid once, `GivenLeagueAwards` flag),
  cup win (`CTournament::DidUserWin` -> WinComp vars 0xa0..0xa6), friendly bonus (FriendlyInfo vars).
- Total = sum of row coins (type 8 rows excluded) + pending `profile+0x2a7e8`; stored by
  **`CMyProfile::SetMatchCredits` at 0x23a274** (r0 = MP_cMyProfile, r1 = total). The coins are actually added later by
  **`CFE::Process` 0x298cb6 `CCredits::AddCredits(matchCredits (+ video reward if the bonus video was watched), 0,0,1,0)`**.
  Either call is a single point to scale match income for the mod economy.

### 5.2 Amounts (CConfig default in the lib / bundled dls_config.dat value)

Tournament ids: 0 Elite, 1 Junior Elite (Prestige), 2 Div1, 3 Div2, 4 Div3, 5 Academy, 6 Playoffs, 7-9 GC Cup, 10 Elite Cup,
11 Friendly, 12 International, 13 All-Star Classic, 14 First Touch Legends, 15 Ultimate Challenge, 16 All-Star match.

| tid | win | draw | loss | per goal | clean sheet | league pos 1 / 2 / 16 |
|---|---|---|---|---|---|---|
| 0 Elite | 16 / **20** | 8 / **10** | 4 / 4 | 3 / 3 | 4 / 4 | 80/78/35 / **120/114/60** |
| 1 Junior Elite | 14 / **18** | 7 / **8** | 3 / 3 | 3 / 3 | 4 / 4 | 70/68/25 / **105/99/50** |
| 2 Div1 | 12 / **16** | 6 / **7** | 3 / 3 | 3 / **2** | 4 / **3** | 60/58/15 / **90/84/35** |
| 3 Div2 | 10 / **14** | 5 / **6** | 2 / 2 | 3 / **2** | 4 / **3** | 50/48/5 / **80/74/30** |
| 4 Div3 | 8 / **12** | 4 / **5** | 2 / 2 | 3 / **1** | 4 / **2** | 40/38/2 / **70/64/20** |
| 5 Academy | 6 / **10** | 3 / **4** | 1 / 1 | 3 / **1** | 4 / **2** | 30/28/1 / **60/54/10** |
| 7-9 GC Cup | 10 / **14** | 3 / **6** | 3 / **2** | 3 / **2** | 4 / **3** | cup win 50 / 50 |
| 10 Elite Cup | 10 / **18** | 3 / **8** | 2 / **3** | 3 / 3 | 4 / 4 | cup win 50 / 50 |
| 11 Friendly | 10 / 10 | 3 / **4** | 1 / 1 | 3 / **1** | 4 / **2** | |
| 12 International | 16 / **20** | 8 / **10** | 4 / 4 | 3 / 3 | 4 / 4 | cup win 50 / 50 |
| 15 Ultimate Chall. | 16 / **20** | 8 / **10** | 4 / 4 | 3 / 3 | 4 / 4 | cup win 30 / 30 |
| 16 All-Star match | 6 / **20** | 6 / **10** | 3 / **4** | 3 / **2** | 4 / **3** | 10 / **20** |

Other income/cost vars (default / bundled): CreditsEarnWelcome 1000 / 1000 (one-off), StartCredits 0 / 0,
StadiumBonusDivisor 2000 / **1750**, CreditsEarnRewardVideos 30, CreditsEarnShare 25, CreditsEarnObjectiveSeason 25,
objectives easy/medium/hard 5/10/15, HealPlayerCost 50 / **30**, scouting InitialCost 100 / **50** + ExtraCost 100 / 100 per
extra session, MaxSessions 5 / **3**, PricePercentIncrement (scouted surcharge) 20 / 20, SellPlayerPercent 50 / 50,
TransfersMinCredits 500 / 500, friendly entry coins Div2 30, Div3 25, Academy 20.
Several bundled XML tags do not match the lib's variable names (e.g. `TournAllstarClassicVictoryCoins` vs var
`TournAllStarClassicVictoryCoins`, `TournFirstTouchLegendseVictoryCoins`), so the defaults most likely apply for those
(All-Star Classic / First Touch Legends: win 16, draw 8, loss 4). A server-downloaded config can override everything at
runtime; only the bundled file can be checked offline.

Rough stock season income (bundled, Div1, 15 league games, ~50% wins, 1.5 goals/game, 5 clean sheets): about
120 (wins) + 25 (draws) + 10 (losses) + 45 (goals) + 15 (clean sheets) + 35..90 (final position) + cup games
(8 GC Cup slots, ~14 per win) + stadium bonus (capacity/1750 per match) => roughly 300-450 coins per season, plus the
1000 welcome coins.

### 5.3 Stock transfer prices in coins (bundled PlayerValues, `GetPlayerValue(info,-1,-1,0,1)`, before +/-1% noise)

| rating | GK | DEF | MID | ATT | sell (50%) GK/DEF/MID/ATT |
|---|---|---|---|---|---|
| 60 | 115 | 140 | 160 | 215 | 60 / 70 / 80 / 110 |
| 65 | 225 | 260 | 290 | 350 | 115 / 130 / 145 / 175 |
| 70 | 400 | 450 | 495 | 565 | 200 / 225 / 250 / 285 |
| 75 | 645 | 715 | 795 | 880 | 325 / 360 / 400 / 440 |
| 80 | 970 | 1065 | 1195 | 1300 | 485 / 535 / 600 / 650 |
| 85 | 1375 | 1500 | 1695 | 1830 | 690 / 750 / 850 / 915 |
| 90 | 1870 | 2030 | 2305 | 2475 | 935 / 1015 / 1155 / 1240 |

Scouted players +20%. Dataset (`dataset_summary.json`, club league players): median 545, p25 360, p75 745, max 2915;
median club squad value 11,400. So one 80-rated signing costs about 3 stock seasons of match income.

## 6. Season / turn schedule

`CSeason` holds a `CSeasonSchedule` at `CSeason+8`: 104 (0x68) `TTurnInfo` slots of 16 bytes (+0 ESeasonMatchType mask,
+8 "setup action" code run when the turn is entered, +0xc bought-secret-player flag, +0xd bought-created-player flag) and the
current slot index as a **byte at schedule+0x680 (= CSeason+0x688)**.
- `CSeason::GetCurrentTurn` 0x36a67c -> `CSeasonSchedule::GetCurrentTurn` 0x35eb4a: returns the **slot index** (0..103),
  not a match counter. `CSeason::IncTurn` 0x36a682 increments it (`CSeasonSchedule::IncTurn` 0x35eb3e) and resets
  per-turn scouting.
- `GetStartTurn` 0x36b02a / `GetEndTurn` 0x36b030 = first / last non-empty slot; `GetStartLeagueTurn` 0x36b036 /
  `GetEndLeagueTurn` 0x36b03c = first / last slot of type 1 (league).
- League match day index: `CSeason::GetSpecificTournament(0)->GetCurRound()` (0x360a74, 0-based); this is what
  `CTransfers::SetupTurn` uses for the secret-player turn.
- Match types: 1 league, 2 playoffs, 4 GC Cup, 0x20 Elite Cup, 0x40 friendly, 0x80 International, 0x100 All-Star cup,
  0x200 First Touch Legends, 0x400 Ultimate Challenge, 0x800 All-Star match (`MCU_GetMatchType` table 0x6420a0).

`CSeasonSchedule::Init(ELeagueTreeIndex)` 0x35e6d8, emulated with Unicorn for all 6 league trees (tournament sizes from the
lib: leagues 16 teams, single round robin => **15 league rounds**; GC Cup 8 slots; Elite Cup 32 teams => 5 rounds; playoffs
4 teams => 2; International 16 => 4; All-Star cup 8 => 3):

```
tree 0 (Elite):  L18 L20 G21 L22 E23 L24 G25 L26 E27 L28 G29 L30 L32 G33 L34 E35 L36 G37 L38 E39 L40 G41 L42 L44 G45 L46
                 E48 G50 AllStarMatch52 Intl54,56,58,60 AllStarCup62,64,66 FTL68 UC70
tree 1 (Junior Elite): same league/GC/Elite Cup slots, then G50, Playoffs 52,54, AllStarMatch 56
trees 2-5 (Div1..Academy): L18..L46 (even slots), G21 G25 G29 G33 G37 G41 G45 G50, Playoffs 52,54, AllStarMatch 56
```
(L = league round, G = GC Cup round, E = Elite Cup round.) Setup actions: slot 26 GC Cup part 2, slot 30 GC Cup part 3,
slot 47 all-star match setup, slot 51 playoffs setup (trees 1-5), 53/61/67/69 bonus-cup setups (tree 0).

**Pre-season:** Init resets the slot index to 0; slots 0..17 are empty. `CSeason::NewSeason` 0x369d20 (and NextSeason)
calls `AdvanceToNextActiveTurn` 0x369e48, which steps over empty slots and over turns without a user fixture (AI results are
simulated in `IncTurn` -> `GenerateCurrentTurnScores` + `PlayTurn`) until a turn with a user match, then `CMyProfile::Save(1)`.
So the season starts **at slot 18 = league round 1; there is no pre-season turn**, except the optional friendly:
`CSeason::InsertFriendly` -> `CSeasonSchedule::InsertFriendly` 0x35eb10 writes type 0x40 into the slot just before the first
used slot (slot 17 at season start).
GC Cup rounds after the user is knocked out are simulated and skipped automatically, so from the player's view one "turn"
is one match.

Suggested transfer windows in slot terms (league round r is slot 16 + 2r):
- Summer window: slots <= 22 (before league round 1 up to round 3), i.e. `GetCurrentTurn() <= GetStartLeagueTurn() + 4`.
- Mid-season window: league rounds 7-9 = slots 28..34 (round 8 = slot 32 is the exact middle of the 15-round league).
  The stock secret-player turn is `league->GetCurRound() == var 0x199 (9)`; `CTournament::GetCurRound` 0x360a74 is a
  0-based counter incremented by `IncRound` 0x3602f4 after each played round, so that is most likely the 10th league match
  (slot 36). Not runtime-verified.
- Closed after the last league slot (46) through the cups/playoffs; the market mod already runs per turn from
  `MARKET_TURN_HOOK` 0x36a078 inside `CSeason::PlayTurn`.

## 7. Recommended hook set for the redesign

| purpose | address | patch | key registers | resume / exit |
|---|---|---|---|---|
| buy gate, search screen | 0x277006 | `b.w cave` over `mov.w r0,#0x4e0` | r4 = card, info = r4+0x294, team = [r4+0x344] | stock: replay + `b.w 0x27700a`; block: `b.w 0x2770e0` |
| buy gate, scouting results | 0x250a3c | `b.w cave` over `mov.w r0,#0x4e0` | r6 = card, r7 = info, team = [r6+0x344] | stock: replay + `b.w 0x250a40`; block: `b.w 0x250ac2` |
| buy divert (all buys) | 0x251f24 | `bl cave` over `blx SignPlayerAttempt` | r0 = info, r1 = source team, r2 = price | return 0 = close dialog, 1 = stock success UI; stock: `b.w 0x21033c` |
| buy: replace coin check too | 0x251ebe | `b.w cave` (do not replay the 2 pc-rel ldr) | statics 0x7601d8 / 0x741d7c / 0x7601dc | block `b.w 0x25201a`; continue r0-r2 set + `b.w 0x251f24` |
| sell gate | 0x23e404 | `b.w cave` over `mov.w r0,#0x4e0` | r4 = CFETeamManagement, info = [r4+0x100]+0x294 | stock: replay + `b.w 0x23e408`; block: `b.w 0x23e426` |
| sell veto after confirm | 0x23edf2 | `bl cave` over `blx CanSellPlayer` | r0 = this | return 0 = abort; stock: `b.w 0x23f3e8` |
| sell price | 0x23f7ea | `bl cave` over `blx GetSellPlayerValue` | r0 = info, r1 = r2 = -1 | return price in r0 |
| sell coins | 0x23f8c2 | `bl cave` over `blx AddCredits` | r0 = price, sb = info, fp = id, [sp] = 5th arg | `b.w 0x2634c0` with amount (<1 = nothing) |
| sell destination | 0x23f8e2 | `bl cave` over `blx CDataBase::SellPlayer` | r0 = info, r1 = buyer, r2 = &spec, r3 = 1 | `b.w 0x20cc68` |
| match income scale | 0x23a274 | `bl cave` over `blx SetMatchCredits` | r0 = MP_cMyProfile, r1 = total | `b.w 0x3776a4` (SetMatchCredits) with the new r1 |

All four `blx` sites above call PLT stubs (ARM); replacing them with a Thumb `bl cave` is the same size. When the cave
must call the original PLT function, either branch to the Thumb implementation in `.text` with `b.w`/`bl`
(SignPlayerAttempt 0x21033c, CanSellPlayer 0x23f3e8, GetSellPlayerValue 0x21316c, AddCredits 0x2634c0, CDataBase::SellPlayer
0x20cc68, CMyProfile::SetMatchCredits 0x3776a4), or use the `adr` anchor pattern for PLT stubs.
Window-closed message: build it in the mod (`ui_show_notice` in native_bridge.c, or `new(0x4dc)` + `CFEMessageBox` ctor
0x248aa8 + `CFE::AddMessageBox` 0x298608 as in section 4).
