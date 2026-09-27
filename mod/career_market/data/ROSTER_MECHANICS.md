# DLS18 roster mechanics (static analysis, libDLS18.so v5.064 armeabi-v7a)

Status: complete (static analysis only; sections 1-8). All addresses are stock-lib VAs (Thumb functions; add 1 when calling).
Evidence gathered with `analysis/armdis.py` / `analysis/xref.py`. Nothing here was runtime-tested.

## Calling convention note

The mangled names do not distinguish static from member functions. Disassembly shows:

- `SignPlayer`, `SellPlayer`: first argument is `TPlayerInfo*` in r0 (behave as static; no `this`).
- `AddPlayerToLink`, `RemovePlayerFromLink`, `VerifyLink`, `CalculateTeamRating`: have a dummy `this` in r0
  (callers pass whatever is in r0; the body ignores r0). The real team id is r1.
  That is why `native_bridge.c` passes `team, team, ...`.
- `GetTeamSpecificData(team, pid)`, `GetTeamLink(team)`, `GetTeamLinkByIndex(i)`, `GetLinkCount()`: no `this`.

## 1. TTeamPlayerLink layout (stride 0x108)

Evidence: `GetTeamLinkByIndex` 0x20bf6c computes `links + i*33*8` (0x108); `SetLink` 0x20ad24 memcpy's 0x108 bytes;
`GetTeamSpecificData` 0x20a620 scans `link+0x88` for `count` entries and returns `&ids[i] - 0x80`.

| off  | size    | meaning |
|------|---------|---------|
| 0x00 | int32   | team id (links array is sorted by team id; `GetTeamLink` 0x20934c binary-searches it) |
| 0x04 | int32   | player count (0..32) |
| 0x08 | 32 x 4  | `TTeamSpecificPlayerData[32]`, parallel to the id array |
| 0x88 | 32 x 4  | player id (int32; only low 16 bits meaningful, TPlayerInfo+0 is a u16 id) |

Max squad size = 32 (`AddPlayerToLink` returns silently without doing anything when `count > 31`, check at 0x20a42e).

`TTeamSpecificPlayerData` (4 bytes):
- byte 0: shirt number (see `GetFirstAvailableShirtNumber` 0x20cb1c usage in AddPlayerToLink).
- byte 1: the player's position *in this team* (EPlayerPosition, signed byte). `CTeamLineup::SelectStartingEleven`
  uses it as the player's position (0x2f08f2); `VerifyLink` compares it to `TPlayerInfo+0x80`.
- bytes 2..3 (u16 at link+0x0a+4*i): role flags. bit0 (0x01) = in the starting XI; bit1 (0x02) captain;
  bit2 (0x04) penalty taker (chosen by `PU_GetShootingStat`); bit3 (0x08) free-kick taker (`PU_GetFreeKickStat`);
  bit4 (0x10), bit5 (0x20) further set-piece roles (corners etc.). Evidence: `VerifyLink` 0x209e6c reassigns each bit,
  `FixLink` 0x208fec enforces exactly 11 x bit0 and exactly one holder of each role bit, `SetTeamLink` copies
  holders to `CTeam+0x134..0x144`.

`GetTeamLink` uses override links when `CDataBase+0x1c != 0` (ptr at +0x1c, count at +0x20, set by
`SetOverrideLinks` 0x20bf84), otherwise the main table (ptr at +0x28, count at +0x38).
`GetTeamLinkByIndex`/`GetLinkCount` always use the main table (+0x28/+0x38).

## 2. Low-level link editing

### `CDataBase::AddPlayerToLink(this*, int team, const TPlayerInfo& info, const TTeamSpecificPlayerData& spec, bool forceShirt, bool useDefaultXI)` 0x20a40c

(stack args: `forceShirt` = [sp+0], `useDefaultXI` = [sp+4] at call time)

1. `link = GetTeamLink(team)`; if `link->count > 31` **return silently** (no error, nothing added). Caller must check.
2. `shirt = GetFirstAvailableShirtNumber(link->team, info.pos(+0x80)==0 /*GK*/, spec.byte0)` (0x20cb1c).
3. Appends at index `n = count`: `ids[n] = info.id (u16)`, `spec[n].byte1 = spec.byte1`, `spec[n].u16@2 = 0`.
4. Shirt: scans existing players; if one already wears `spec.byte0` and (the new player's rating is higher **or**
   `forceShirt != 0`), the newcomer gets `spec.byte0` and the old holder is renumbered to `shirt`; otherwise the
   newcomer gets `shirt` (first free number).
5. `count++`.
6. If `useDefaultXI`: builds a temporary `CTeamManagement`, `SetDefaults(team, 0, NULL, NULL)` (default lineup/tactics
   for that team), and walks the 11 lineup slots (`CTeamLineup::GetID(j)`, slot position from
   `FS_iFormationPlayerPos[formation*0x2c + j*4]`). Otherwise walks all `count` players using each player's own position.
   It picks the lowest-rated player at a `PU_IsPositionEquivalent` position whose rating is below the newcomer's, and
   **swaps the u16 role-flag word (spec bytes 2..3) between that player and the newcomer** (newcomer inherits
   captain/set-piece flags; see SetTeamLink below for bit meanings).
7. `CSeason::UpdateStatsAddPlayer(season, team, pid, 0)`.

No money, no user CTeamManagement, no save flag, no min/max squad or position validation other than the 32 cap.

### `CDataBase::RemovePlayerFromLink(this*, int team, int pid, bool updateStats)` 0x209dd8

Finds `pid` in `ids[]`; if absent returns. Rebuilds both parallel arrays without that entry (order preserved,
`count--`). If `updateStats`, calls `CSeason::UpdateStatsRemovePlayer(season, team, pid, 0)`. No lower bound check:
a team can be emptied. The removed player's role flags (captain etc.) are simply dropped.

### Role-flag word (spec bytes 2..3), from `CDataBase::SetTeamLink(const TTeamPlayerLink*, bool recalcRating)` 0x2097e4

SetTeamLink copies ids/specs into the live table, writes `CTeam+0x148 = count`, then for each player sets:
bit1 (0x02) -> `CTeam+0x134`, bit2 (0x04) -> `+0x138`, bit3 (0x08) -> `+0x13c`, bit4 (0x10) -> `+0x140`,
bit5 (0x20) -> `+0x144` (player ids of captain / set-piece takers in the CTeam record). If `recalcRating` and a global
flag is set it tail-calls `CalculateTeamRating(team, NULL)`.

### `CTransfers::CanAddPlayer(int team, const TPlayerInfo&, int)` 0x2102d4

team == -1 -> 2 (ok); `count > 31` -> 0 (full); player already in team -> 1; else 2 (ok).

### `CTransfers::CanRemovePlayer(int team, TPlayerInfo*)` 0x213070  -- the stock squad floor

team == -1 -> 2. Calls `PU_GetPlayerPositionCounts(team, &gk, &def, &mid, &fwd)`.
`count < 17` -> 0 (refuse: a team may never drop below 16 players);
`gk > 1` -> 2 (ok); otherwise if the player is a GK (`info+0x80 == 0`) -> 1 (refuse: last goalkeeper); else 2.

**Stock limits: 16 <= squad <= 32, and at least one goalkeeper must remain.**
(The mod's `commit_transfer` uses `buyer < 32` and `seller > 18`, which is stricter than stock -> fine; but it does not
check the last-GK rule.)

## 3. `CDataBase::CalculateLinks(bool verifyLinks, bool updateStats, bool noRatingRecalc)` 0x2093b0 -- CRITICAL

Static, no `this` (r0 = first bool). Step by step:

1. Saves RNG seed, sets seed 0x102 (deterministic result).
2. If a live table exists (`db+0x28`): copies the **user team (0x102) link** to a local buffer, `delete[]`s the live table.
3. Allocates a new live table of `db+0x38` links and **copies every link from the default table `db+0x24`**
   (`GetDefaultLinks` 0x2092d4 returns `db+0x24`). => every AI team is reset to its database default roster.
4. Restores the saved user link via `SetTeamLink(&saved, !noRatingRecalc)`.
5. For each player in the user team: `GetSourceTeam(&src, &x, &spec, pid, 0x102, 0, 1)` finds the AI team that owns
   him in the default data. If `CanRemovePlayer(src) == 2` it removes him from `src` (`RemovePlayerFromLink(src,pid,updateStats)`),
   calls `VerifyLink(info, src, &spec, 0x102, -1, 0, 0, -1)`, `CalculateTeamRating(src)`.
   Otherwise it computes the player's slot in `src`'s default XI and uses `FindReplacementPlayer` to take a
   replacement from another team (Remove/VerifyLink/AddPlayerToLink(src, repl, spec, 0, 1)), then removes the user's
   player from `src`. It also patches cached `CTransfers::ms_tAsyncPlayerSearchInfo` entries (0xb0 stride) to the new team.
6. If `verifyLinks`: `CDataBase::VerifyLinks()` 0x20a64c.
7. Regenerates the "simple links" (`GenerateSimpleLinksFromLinks` -> `db+0x98/0x9c`, used by player search), sorts them,
   restores RNG seed, and sets up the all-star match if the active tournament index is 9.

**Consequence:** any AI roster change made directly in the live table (AddPlayerToLink/RemovePlayerFromLink on
AI teams, which is what `move_player`'s AI-to-AI branch does) is **erased** the next time CalculateLinks runs.
Callers of CalculateLinks: `CDataBase::Init` 0x20bc2c (every DB init/profile load, args 1,1,0),
`SignPlayer`/`SellPlayer` (when their `calcLinks` arg is true, args 1,0,0), `NewDreamTeam`,
`CFEMsgSignPlayer::CaptainSignedCB`. The mod itself calls `CalculateLinks(1,0,0)` in `commit_transfer`
(when `recalculate_links`) and near native_bridge.c:1437 -- both wipe earlier AI-to-AI moves, and a user-team
SignPlayer/SellPlayer with calcLinks=1 (as `move_player` passes) wipes them too.

Only the user link survives CalculateLinks. AI rosters are always *derived*: default file data + CConfig custom links
+ removal of players the user owns.

### How default links are built: `CDataBase::LoadDefaultLinks` 0x20a8fc -> `PopulateDefaultLinksArray` 0x20a984

Runs only when `db+0x24 == 0` (once per database load, from `CDataBase::Init`). Decompresses the links file into
`db+0x24` (count-1 links into `db+0x38`), generates default simple links (`db+0x90/0x94`), then applies
**CConfig custom links** (`CConfig::GetCustomLinkCount` 0x201510 / `GetCustomLink(i)` 0x201524; 12-byte records
`{PlayerID, SourceTeamID, DestTeamID}` parsed from `<Link>` nodes by `CLinksInfo::LoadInfo` 0x2007c4 - the server
config / live roster update). While applying them it temporarily points the override-links pointer at the default
table (`db+0x1c/0x20 = db+0x24/count`) so `GetTeamLink` edits the default table. Per record:
- src,dst both valid: if `CanRemovePlayer(src)==2 && CanAddPlayer(dst,info,-2)==2`:
  `RemovePlayerFromLink(src,pid,1)`, `VerifyLink(info,src,&spec,0x102,-1,0,0,-1)`, `AddPlayerToLink(dst,info,spec,0,1)`.
- dst == -1: remove from src (+VerifyLink) if CanRemovePlayer==2.  src == -1: add to dst if CanAddPlayer==2.
- src == dst == -2: remove from every team that has him.
- records whose src is 0x102, or whose player is currently in the user team, are skipped.
Afterwards override pointer reset to 0 and default simple links regenerated.

=> This is the stock, supported way to relocate an AI player *persistently for the session*: edit the **default**
table (db+0x24) the same way (override-pointer trick), then CalculateLinks re-derives the live table from it.


## 5. What is saved (career/profile save)

`CMyProfile::Save` 0x375b88 serializes, in order: header ints, `CCreatePlayer`, **`CDataBase::SerializeDreamTeam`**
0x209324, `CPlayerDevelopment`, `CPreTrainedPlayers`, `CSeason::Serialize` 0x36bb98, game settings, stats, ...
(`CMyProfile::LoadDiskData` 0x376c38.. / `LoadCloudData` call the same SerializeDreamTeam.)

- `SerializeDreamTeam` = `TTeamPlayerLink::Serialize(GetTeamLink(0x102))` 0x207f06: team id, count,
  32 x `TTeamSpecificPlayerData::Serialize`, 32 x int player id. **Only the user team link is saved.**
  When loading (serializer flag +0x1c == 0) it then tail-calls `CalculateLinks(1, 0, 1)` (veneer 0x5c1818), which
  rebuilds every AI team from the default table.
- `CSeason::Serialize` 0x36bb98 calls TSeasonMainInfo, TSeasonLeagueTreeInfo, CSeasonTournamentInfo,
  CSeasonTeamManagementInfo, CScoutingInfo, CSeasonPOTWInfo, TObjectiveInfo, TSeasonSummaryInfo, CSeasonStadiumInfo,
  CSeasonAllTimeStats, CCustomDreamTeamData, TSeasonMatchScoreInfoBasic -- **no team links**.
- Nothing in the stock save stores AI rosters. The default table (db+0x24) is rebuilt from the links file + CConfig
  custom links only when the DB is (re)loaded (`LoadDefaultLinks` runs when db+0x24 == 0).

=> **AI-to-AI roster changes do not persist across save/reload (or app restart) unless the mod re-applies them.**
The mod must keep its own list of AI moves (its `history[]` already records seller/buyer/player) and replay them into
the **default** table after `LoadDefaultLinks`/`PopulateDefaultLinksArray` (or feed them in as extra CConfig custom
links), before the `CalculateLinks` that follows profile load.

## 6. `SignPlayer` / `SellPlayer` step by step (question 1)

### `CDataBase::SignPlayer(const TPlayerInfo* info, int fromTeam, const TTeamSpecificPlayerData* spec, bool calcLinks, bool forceShirt, bool useDefaultXI)` 0x20cbd4

Hard-wired to the **user team** (`CSeason::GetUserTeamID()` on `MP_cMyProfile+0x14`); `fromTeam` is only the source.
1. `AddPlayerToLink(user, info, spec, forceShirt, useDefaultXI)` (user's live link; silently no-op if user has 32).
2. `RemovePlayerFromLink(fromTeam, info->id, false)` (no stats update; no VerifyLink/FixLink on the source team!).
3. `CSeason::GetTeamManagement()->AddPlayer(id)` (`CTeamManagement::AddPlayer` 0x2f29de -> `CTeamLineup::AddPlayer`
   + season player-state array of 32) -- user squad/lineup order.
4. if `calcLinks`: `CalculateLinks(1, 0, 0)` -> all AI links re-derived from the default table (section 3); the source
   team loses the player only because he is now in the user link (or a replacement is pulled in if the source team
   would drop below 17).
5. `CSeason::ValidateStats()`, `CSeason::VerifyTeamManagement()`, tail-call `CalculateTeamRating(user, NULL)`.
No credits, no player development, no save. Those are done by the caller:

`CTransfers::SignPlayerAttempt(const TPlayerInfo&, int fromTeam, int cost)` 0x21033c (stock buy flow):
`CanAddPlayer(user, info)`: 0 -> "squad full" message box and abort, 1 -> already owned, abort; 2 -> continue.
`SignPlayer(info, fromTeam, GetTeamSpecificData(fromTeam, id), 1, 0, 1)` (**forceShirt = 0**, useDefaultXI = 1),
`CPlayerDevelopment::AddPlayer(id, 0)`, `GetSeasonPlayerStateByID(id)->byte2 = 1`, dynamic-difficulty tweak,
re-`ExpandTeam` of the current fixture's `tGame` CTeam slot (so a same-turn user match sees the new squad),
`CCredits::SubtractCredits(cost)`, stats/achievements, `CMyProfile::Save(3)`.
No CanRemovePlayer check on the seller: the stock game relies on CalculateLinks' replacement logic.
Note: `native_bridge.c` passes `forceShirt = -2` (the initial value of r5 in SignPlayerAttempt, overwritten with 0 at
0x21042a before the call). With nonzero forceShirt the newcomer steals his old shirt number from any user player
who wears it. Harmless but not stock; use 0.

### `CDataBase::SellPlayer(TPlayerInfo* info, int buyerTeam, const TTeamSpecificPlayerData* spec, bool calcLinks)` 0x20cc68

**`buyerTeam` (r1) is ignored** -- never saved or read. Steps:
1. `RemovePlayerFromLink(user, info->id, false)`.
2. `VerifyLink(info, user, spec, -1, -1, NULL, 0, -1)`: hands the sold player's starter/captain/set-piece flags to the
   best remaining user player at the same position, FixLink; **if the user squad is now < 16 it loops
   FindReplacementPlayer + SignPlayer until the user has 16** (free replacement signings).
3. `CTeamManagement::RemovePlayerByID(id)` (lineup, roles, season player state).
4. if `calcLinks`: `CalculateLinks(1, 0, 0)` -> the player reappears at whatever team holds him in the **default**
   table (his original club), not at `buyerTeam`.
5. `ValidateStats`, `VerifyTeamManagement`, `CalculateTeamRating(user)`.
Stock caller `CFETeamManagement::SellPlayer` 0x23f7b4: price = `GetSellPlayerValue(info,-1,-1)`; picks a random
valid team whose rating is within +/-5 (widening) of the player rating and passes it as `buyerTeam` (cosmetic);
`SellPlayer(info, team, &spec, 1)`; `CCredits::AddCredits(price, ...)`; `CDreamLeagueStats::IncNumSales`.
Created players go through `DeleteCreatedPlayer` instead.

**Can they move a player between two AI teams?** No. SignPlayer always adds to the user; SellPlayer always removes from
the user and never adds anywhere. They do not update money of AI clubs (there is none) or any transfer list.

### Mod consequences (native_bridge.c `move_player`/`commit_transfer`)
- Buy (buyer == user): `SignPlayer(..., 1, -2, 1)` works; CalculateLinks inside it wipes earlier AI-to-AI moves.
- Sell (seller == user): `AddPlayerToLink(buyer)` then `SellPlayer(..., 1)`: SellPlayer's CalculateLinks erases the
  buyer addition, so the post-check `!get_specific(buyer)` fails and the function returns 0 **after** the player has
  already left the user team (he goes back to his default club). Expect "failed" user sales that still remove the
  player, unless the buyer happens to be his default club.
- AI to AI: live-table Add/Remove succeeds, but `commit_transfer(..., recalculate_links=1)` and the batch
  `calculate_links(1,0,0,0)` after AI transfers (native_bridge.c ~1611) immediately revert every AI move.
  The mod's finances/history then disagree with the real rosters.

## 7. How AI teams get their match-day XI (question 3)

- `CDataBase::ExpandTeam(CTeam*, TPlayerInfo*, ..., int team, CTeamManagement* tm, int)` 0x20b8bc refreshes
  `CTeam+0x148 = GetTeamPlayerCount(team)` and calls `PlayersLoad` 0x20baa0: with a `CTeamManagement` (user) it
  orders players by the user lineup/roles; with `tm == NULL` (AI) it reads the **live link directly** in link order
  (`GetPlayerInfo` per `ids[i]`). No separate per-AI-team roster cache exists; the only cached derived data are the
  team ratings in the CTeam record (`CalculateTeamRating` 0x20b71c writes `CTeam+0x08..0x14`: overall and per-line
  averages over the best 18 players) and the role holder ids at `CTeam+0x134..0x144` (SetTeamLink only).
- `CTeamLineup::SetSquad(CTeam*)` 0x2f03a4 -> `SelectStartingEleven(CTeam*, TPlayerInfo*, bool used[32])` 0x2f085c:
  takes, in link order, the first 11 link entries with **flag bit0 (starter)**, uses spec byte1 as each player's
  position, then fills formation slots 10..0 (`FS_iFormationPlayerPos[CTeam+0x12f formation]`) by
  `PlayerPositionSuitability`. Remaining players (squad byte `CTeam+0x148`) fill the 7 bench slots (11..17) by
  general position. Team 0x15c uses `RandomiseStarting11` (which calls FixLink).
- Requirements: exactly 11 starter flags. With fewer, SelectStartingEleven iterates over uninitialised scratch entries
  (garbage index/used flag on its stack buffer) -> wrong players or out-of-bounds copy. `RemovePlayerFromLink` alone
  drops a starter's bit0 without reassigning it, so every removal must be followed by `VerifyLink` (reassigns by
  position, then FixLink) or at least `FixLink(link)` 0x208fec (static, r0 = link). AddPlayerToLink with
  `useDefaultXI = 1` keeps the count at 11 by swapping flags with a weaker starter. No goalkeeper is enforced in the
  XI (an outfielder would be picked for the GK slot), but `CanRemovePlayer` refuses to remove a team's last GK.
- Cached ratings: after changing an AI roster call `CalculateTeamRating(team, NULL)` (member: pass
  `*CDataBase::ms_pInstance` in r0) for both teams, as CalculateLinks/PopulateDefaultLinks do; the stock sale flow and
  AI strength use `GetTeamRating`.

`CDataBase::VerifyLinks()` 0x20a64c (run by `CalculateLinks(verifyLinks=1, ...)` and `CMyProfile::Validate`): for every
live link counts bit0/role bits and calls `FixLink` when the counts are not 11 / exactly one. So a CalculateLinks(1,..)
also sanitises XI flags of every team (position-agnostic: FixLink promotes the first non-starters in link order).

## 8. Recommended safe move sequences (question 4)

Notation: `DB = *CDataBase::ms_pInstance` (GOT slot 0x73074c -> global 0x75cca4). Member functions take a dummy
`this` in r0 (pass DB or the team id). Thumb addresses: add 1.
Stock limits to enforce **before** any call: buyer count <= 31 (`CanAddPlayer(buyer, info, -2) == 2`),
seller `CanRemovePlayer(seller, info) == 2` (seller count >= 17 so it stays >= 16, and not its last GK),
player not already in buyer. For the user team the same checks apply (user < 16 after a sale triggers free
auto-signings in VerifyLink).

### A. AI team A -> AI team B (persistent within the session)
Edit the **default** table so CalculateLinks re-derives the live table with the move applied (this is exactly what
`PopulateDefaultLinksArray` does for server custom links):
1. `info` = `GetPlayerInfo`/`GetPlayerInfoSimple(pid)`; confirm `pid` is in A's live link and A != 0x102, B != 0x102.
2. `SetOverrideLinks(DB[0x24], DB[0x38])` 0x20bf84 (static: r0 = links ptr, r1 = count). From now on GetTeamLink,
   GetTeamSpecificData, CanAdd/CanRemove, Add/Remove/VerifyLink operate on the default table.
3. Check `GetTeamSpecificData(A, pid)` != NULL in the default table (if the player was already moved by an earlier
   mod move, his default owner is the earlier buyer -- use that as A). Copy `spec = *that` (4 bytes).
4. Check `CanRemovePlayer(A, &info) == 2` and `CanAddPlayer(B, &info, -2) == 2` (on the default table).
5. `RemovePlayerFromLink(this, A, pid, 1)`.
6. `VerifyLink(this, &info, A, &spec, 0x102, -1, NULL, 0, -1)` 0x209e6c (stack: -1, NULL, 0, -1 after the 0x102):
   passes the leaver's XI/captain/set-piece flags to a same-position team-mate and runs FixLink.
   Never call it on a team that is below 16 after the removal (its top-up loop signs players to the USER team).
7. `AddPlayerToLink(this, B, &info, &spec, 0 /*forceShirt*/, 1 /*useDefaultXI*/)`; re-check GetTeamSpecificData(B,pid).
8. `SetOverrideLinks(NULL, 0)`.
9. Regenerate the **default simple links** (GetSourceTeam uses them to find a user-owned player's AI owner):
   `operator delete[](DB[0x90])`; `DB[0x90] = GenerateSimpleLinksFromLinks(DB[0x24], DB[0x38], &DB[0x94])` 0x20a750;
   `InsertionSortTTeamPlayerLinkSimple(DB[0x90], DB[0x94], 1)` 0x20a856. (Same as the tail of PopulateDefaultLinksArray.)
10. Once per batch: `CalculateLinks(1, 0, 0)` -> live table = default (with the moves) + user link, simple links and
    XI flags refreshed. Then `CalculateTeamRating(DB, A, NULL)` and `(DB, B, NULL)` are already done for teams touched
    by the user-player step; call them yourself for A and B to refresh `GetTeamRating`.
Do not edit the live table for AI moves; it is thrown away by every CalculateLinks.

### B. AI team A -> user (0x102)
1. Checks: `CanAddPlayer(0x102, &info, -2) == 2` (user <= 31). Seller floor is handled by CalculateLinks
   (replacement), but for a clean market also require `CanRemovePlayer(A) == 2` on the live table.
2. `spec = *GetTeamSpecificData(A, pid)` (live table).
3. `SignPlayer(&info, A, &spec, 1, 0, 1)` 0x20cbd4 (stock args; use forceShirt 0, not -2).
4. `CPlayerDevelopment::AddPlayer(pid, 0)` 0x20e130; optionally
   `CSeason::GetTeamManagement()->GetSeasonPlayerStateByID(pid)->byte2 = 1` like SignPlayerAttempt.
5. If the user plays a match this turn, re-`ExpandTeam` the `tGame` CTeam slot like SignPlayerAttempt, or only allow
   transfers between turns. Save via the normal `CMyProfile::Save`.
The default table still lists the player at his default owner; CalculateLinks strips him while the user owns him.

### C. user -> AI team B
SellPlayer cannot target B. Sequence:
1. Checks: user count >= 17 (else VerifyLink auto-signs replacements); `CanAddPlayer(B, &info, -2) == 2` on the
   **default** table (B's derived squad must stay <= 32).
2. Find the player's default owner D: `GetSimpleLinkTeamIDsFromPlayerID(1, pid, teams, &n)` 0x20ce0c or scan the
   default table under the override. If D != B, move him D -> B inside the default table (steps A2-A9).
   D's default list still contains him (only the live table strips him), so D's live squad is unchanged by this;
   but `CanRemovePlayer(D) == 2` on the default table is still mandatory: if D's default count is 16, removal
   would make VerifyLink(D) run its top-up loop, which signs players to the USER team. In that case refuse the
   sale or let it go to D.
3. `spec = *GetTeamSpecificData(0x102, pid)`; `SellPlayer(&info, B, &spec, 1)` 0x20cc68 -> removes from user,
   VerifyLink(user), CTeamManagement::RemovePlayerByID, CalculateLinks(1,0,0) -> the player now appears at B.
4. `CalculateTeamRating(DB, B, NULL)`. Do **not** call AddPlayerToLink(B) on the live table first (current mod code),
   it is erased and makes the post-check fail.

### D. Persistence across save/reload (question 5)
Only the user link is saved (`SerializeDreamTeam`), and loading triggers `CalculateLinks(1,0,1)`. The default table is
built once per process (`LoadDefaultLinks` skips when DB[0x24] != 0) and freed only by `~CDataBase`.
Therefore the mod must:
- Keep a pristine snapshot of the default table (`DB[0x38] * 0x108` bytes from `DB[0x24]`) taken the first time the mod
  runs, before any edit.
- On every career load (after the profile load's CalculateLinks, e.g. when the market state is loaded), restore the
  snapshot, replay the saved AI-to-AI and user-sale destinations from `history[]` in chronological order via steps
  A2-A9 (skip entries whose player is now owned by the user or no longer at the recorded seller), regenerate default
  simple links, then `CalculateLinks(1, 0, 0)`.
- Restoring the snapshot first also prevents moves from one save leaking into another save loaded in the same process.
Alternative with the same effect: hook `CConfig::GetCustomLinkCount`/`GetCustomLink` (0x201510/0x201524) to append
`{pid, seller, buyer}` records -- but those are applied only inside PopulateDefaultLinksArray (once per process), so
the replay-into-default approach is still needed for loads after start-up.

## 9. Summary of risks in the current mod code

1. AI-to-AI moves are made in the live table and are reverted by the mod's own `CalculateLinks(1,0,0)` calls
   (commit_transfer with recalculate_links, and the batch call after AI transfers), by any user SignPlayer/SellPlayer,
   and by every profile load. Money/history then diverge from rosters.
2. User sales: `AddPlayerToLink(buyer)` + `SellPlayer(...,1)` returns failure after the player already left the user.
3. `SignPlayer` forceShirt should be 0 (stock), not -2.
4. Missing last-goalkeeper check (`CanRemovePlayer` returns 1 for the last GK).
5. Live-table removals without VerifyLink/FixLink can leave a team with fewer than 11 starter flags until the next
   VerifyLinks; SelectStartingEleven then reads uninitialised scratch entries.
