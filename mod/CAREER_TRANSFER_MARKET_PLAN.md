# DLS18 Dynamic Career and Transfer Market

> **Status (2026-09-26):** this file is the original design record. The implemented economy and release state are in
> `career_market/BALANCE.md` and `career_market/README.md`. Current artifact: `mod/build/DLS18_career_market_v27.apk`.
> The AI-to-AI “live link” approach described below does not persist in the game.


## Goal and design

Add a persistent club career market to DLS18's existing Dream League season. The user club and AI clubs buy and sell players in two short windows each season. Every club has its own finances, and transfers pass through a club fee negotiation and a player wage negotiation before roster ownership changes.

The design keeps DLS18's match engine, player database, and season progression. FIFA Manager 12 provides the deeper career cues: clubs rank squad needs, list surplus players, negotiate a fee with the current club before a player contract, and operate two transfer windows each season ([EA manual](https://eaassets-a.akamaihd.net/eahelp/manuals/FIFAM12pc.pdf)). The FTS 15 mobile Manager Mode listing highlights signing players, negotiating contracts, and loans ([listing reproduced by TouchArcade](https://toucharcade.com/games/first-touch-soccer-2015)). This mod adapts those ideas to DLS18's short menu dialogs and native roster/save systems; it does not use FTS assets or database files.

## Game data and native surfaces

- The base game contains 232 team records and 5,816 player records. The `teamplayerlinks_0.dat` roster-link payload is opaque; the mod uses game roster helpers and does not rewrite that file.
- Native `CTransfers` provides player and sale valuation, availability checks, and user signing: `GetPlayerValue` at `0x212b3c`, `GetSellPlayerValue` at `0x21316c`, and `SignPlayerAttempt` at `0x21033c`.
- `CDataBase::GetPlayerInfoSimple` (`0x20da0c`) fills the `0xb0`-byte `TPlayerInfo`. Its ROM converter stores the detailed position at `+0x80` and writes `PU_GetGeneralPosFromPos`'s four-way group to `+0x7f`; the market cache reads `+0x7f` for squad coverage and goalkeeper/defender/midfielder/forward labels. This keeps AI needs in the same general-position groups used by the market UI.
- The Dream League transfer screen is `CFESDreamLeagueTransfers` (`0x276430`); its entry hook routes to the market menu. `CSeason::PlayTurn` (`0x369fa2`) is the AI market tick, and `CSeason::Serialize` (`0x36bb98`) is the save extension point.
- Moves into the user club use `CDataBase::SignPlayer` (`0x20cbd4`) and `CPlayerDevelopment::AddPlayer` (`0x20e130`); moves out use `CDataBase::SellPlayer` (`0x20cc68`). Their argument flow is statically traced from the game's transfer caller. AI-to-AI moves use `AddPlayerToLink` (`0x20a40c`) and `RemovePlayerFromLink` (`0x209dd8`), then recalculate links. All roster paths still need runtime confirmation.
- `CSeason::GetUserTeamID` returns `0x102` in this DLS18 build. The mod's current user-club constant matches that function.
- There are no per-club cash or wage accounts in the identified team/player records. Market finances live in the career save extension; native team and player databases remain unchanged.

These addresses and layouts are for DLS18 v5.064, `armeabi-v7a`.

## Career rules

### Transfer windows

Use season turns, since the career scheduler exposes turns rather than FIFA-style dates.

1. **Preseason:** turns 0 and 1.
2. **Midseason:** two turns starting at `season_turn_count / 2 - 1`. For a 38-turn season, these are turns 18 and 19.
3. A fee, contract, or roster move can commit only while a window is open. Unresolved offers expire when the window closes.
4. The first version has no free-agent exception; every move uses one of the two windows.

### Club economy

Each active club has persistent cash, a transfer budget below cash, an annual wage budget, payroll, squad value, and strength. Opening values are seeded from the native team valuation and current roster. All amounts use DLS18 value units.

The current native alpha accrues provisional turn income from squad value and subtracts a payroll-based wage cost. It reinvests 35% of positive operating surplus into the transfer budget and caps that budget at available cash, so a club running an operating loss cannot grow its transfer funds. Buyers must reserve cash, stay within their transfer budget, and have wage room. Sellers receive the fee, and buyers receive the player's contract wage in payroll. Transfer history records fee, annual wage, contract length, and clubs.

The alpha's concrete starting and cashflow rules are:

- Let `V` be the native DLS total value for a club when its account is created, with a floor of 1,000. Opening cash is `V / 2 + 500`; the opening transfer budget is `V / 8`, capped at cash.
- A player's estimated annual wage is `max(player_value / 20, 1)`. Initial payroll is the sum of those estimates for the club's roster. After that roster is read, the annual wage budget is `floor(payroll × 1.25) + 100`.
- Each season, revenue is `floor(current_squad_value / 16) + 500`. That amount is spread evenly across the season's turns, with a minimum of one unit per turn. Each turn's wage cost is `ceil(payroll / turns_per_season)`.
- The market adds `floor(35% × positive_turn_surplus)` to the transfer budget and caps the result at cash. A buyer must leave cash equal to at least 5% of its squad value and have both transfer budget and wage room. A completed purchase subtracts the fee from buyer cash and transfer budget; the seller receives the full fee as cash and 70% of the fee as additional transfer budget.
- Squad value and payroll are recalculated from the live roster at initialization, turn 0, season rollover, when a transfer window opens, and when the market screen opens. Transfers update both clubs immediately. This makes values relative to DLS's own valuation units, while the absolute revenue rate remains a synthetic pacing choice.

With only the base wage estimate, annual payroll is approximately 5% of roster value and annual revenue is approximately 6.25% plus 500 units. The resulting operating surplus is therefore about 1.25% of roster value plus the fixed amount, before integer rounding; tracked wages can move it up or down. This makes the baseline budget growth explicit, but does not establish whether the pace feels right over a long career.

The current income formula is synthetic. A saved per-club ledger records the resulting revenue, wage costs, transfer-budget allocations, and transfer transactions. The native market refreshes roster values and tracked wages at turn 0 and on season rollover before applying the new season's costs and wage budget, so an expired tracked contract does not keep its prior wage through the next season's early turns. Static tracing found `CSeason::CalculateAttendance`, which estimates attendees from fan approval, two clubs' star ratings, and the current stadium capacity; it does not produce money or per-club cash flow. Tracing the post-match award setup (`CFEPostMatchCreditAwards::SetupCreditAwardInfo`, `0x239c58`) found win, draw, loss, goal, clean-sheet, and tournament rewards. `CCredits::AddCredits` (`0x2634c0`) forwards those awards into `CMyProfile::AddCredits` (`0x377984`), the user's profile credit balance. This is player/profile currency, not club cash, so it is not counted as club operating income. The base game path therefore does not supply per-club match revenue; simulated club income remains necessary and needs pacing calibration.

### Club policy and player value

Native player value is the starting point. Seller minimums respond to squad strength; AI bids consider the buying club's scaled positional coverage gaps, group ratings, seller surplus, fee room, and wage room. The largest squad coverage gap is the first priority; when all groups meet their scaled targets, the lowest-rated group leads. A buyer can fall back to another weak area when no suitable affordable player is available at the top need. Before spending its single attempt for a window, each AI buyer runs the same bounded fee and wage negotiation calculations used by transfer settlement, so it skips targets that cannot fit its budget after counters. AI clubs keep a minimum squad size and positional cover.

Each AI club makes at most one purchase attempt per window and may complete at most one sale in that window. AI clubs can sell surplus players when another club has a matching need. User-club players become AI sale targets only after the user lists them; different AI clubs can have offers pending on different user listings in the same window. The user can list or remove a listing during either window. The user club can complete one sale per window; after one sale settles, the other incoming offers close.

Age and contract length are represented in the Python reference engine. Since the base DLS18 roster data does not expose player contract dates, the native market assigns each original player a stable estimated two-to-five-season term, anchored to the season when the market is first added to that career. This anchor uses a reserved word already present in the `0xaf` save block, so the save layout does not grow. Estimated remaining terms influence seller minimums and AI offers and appear in market and squad views. At season rollover, an AI club can renew up to three core players whose modeled or tracked term has one season or less remaining, provided the new wage fits the club's wage budget; each renewal records a three-season term and updates payroll and finance activity. The user can renew an original player's estimated contract or a mod-recorded contract by negotiating a wage and choosing a one-to-five-season extension; renewal writes an explicit saved contract and updates payroll and finance activity. After an explicit tracked contract expires, the wage falls back to the value-based estimate. These terms are market estimates, not DLS18 contract data. The native alpha does not model player aging, bonuses, or release clauses.

### Negotiation flow

1. A buyer offers a transfer fee to the current club.
2. The seller accepts, rejects, or counters. The native user flow allows a limited number of exchanges and a walk-away choice.
3. After the fee is agreed, the player evaluates the proposed wage and contract. If the player counters, the user can accept, walk away, or counter once with a preset midpoint or exact wage. The player accepts at 90% of demand or better, makes one final counter when the user offers at least 85%, and rejects lower offers. The final player counter can be accepted or declined.
4. Only after both stages succeed does the mod move the player and update both clubs' finances and history.
5. AI-to-AI transactions use the same valuation, fee counter, and wage response rules through a bounded automatic negotiation. AI offers for listed user players appear in the inbox for the user to accept, counter, or reject; opening bids are capped to the buyer's available transfer room and all counters recheck the live buyer budget.

This follows the separate club and player stages, counters, and window timing described in EA's [FIFA Manager 12 manual](https://eaassets-a.akamaihd.net/eahelp/manuals/FIFAM12pc.pdf). EA's [Career Mode guide](https://help.ea.com/en/articles/ea-sports-fc/career-mode/) also treats wages and squad decisions as connected career choices.

### Market interface

The current v27 implementation routes into a full-screen seven-tab market (Market, My Squad, Sales, Inbox, Finances,
History, and Shortlist). It identifies the active window or explains when the next transfer period opens. It rebuilds the
live roster cache when the screen opens, so club-squad browsing still has player data after loading outside a window:

V19 keeps position, club, and name-search controls in a persistent toolbar above the Market cards. Position cycles in
place, active filters are highlighted, and every filter change returns to page one. Previous/Next remain in the page
footer, leaving the selected player's action pane focused on shortlist and offer actions.

- **Browse Market:** browse four-player pages, with previous/next controls and a visible item range/total. Filter by player name, position, or club; changing filters or search resets to page one. Each card shows club, position, rating, value, and wage. A single full-screen Offer Terms page adjusts fees and annual wages in 10% steps, changes contract length from 1–5 years, or opens DLS18's built-in keyboard for exact whole-number entry; the user can see available fee and wage room before submission.
- **Player Sales:** cycle through the user's roster and list or delist a player.
- **Quick sale and shortlist:** preview the buyer and fee before confirming a quick sale; save up to 16 players in the
  persistent shortlist and create a bid from a shortlisted player.
- **Player card UI:** Market, My Squad, Sales, Inbox, and Shortlist show up to four native player cards in a two-column
  grid inside the dedicated screen. All seven lists have Previous/Next page controls and a range/total counter in a
  footer that stays clear of cards and descriptions. Tapping a card opens the existing full-screen player/offer details
  and actions. Club and fee/status lines appear below each card. Host page-boundary tests and static wiring checks pass;
  rendering and touch still need a device check.
- **Offers / Inbox:** review simultaneous offers from different clubs on different listed user players and resolve them with accept, preset counter, exact whole-number fee counter, or reject choices. Completing one sale closes the other bids for that window. In user signings, the player wage stage also supports a preset or exact user counter, budget validation, and one final player counter. When the user is selling, each AI buyer answers according to its budget and the player's valuation.
- **Club Finances:** cycle through clubs to view cash, transfer budget, wage budget, payroll, squad value, strength, and recent finance activity.
- **Club Squad:** cycle through players and clubs to inspect position, rating, market value, wage, and user sale-list status.
- **Transfer History:** review completed deals, fees, wages, and contract lengths.

Player-name search and exact amount entry use DLS18's existing keyboard dialog. Search can be combined with position and club filters; clearing the text restores all names. The page counter updates after filtering. These flows are statically traced but still need on-device confirmation.

## Save format and transaction safety

The market state is stored in the existing career save, alongside DLS18's season data. The base market block is serialized as version `0xaf`; AI purchase-attempt markers and up to 32 user player listings use `0xb0`; the per-club finance ledger uses `0xb1`; and the appended contract extension uses `0xb2`. The contract-season anchor occupies a word that was reserved in the existing `0xaf` block, so this feature adds no field to that block and does not change its size. It retains the original 512 contract slots there and appends 7,680 more, for a total capacity of 8,192 player records. A derived player-ID index is rebuilt after loading and is not serialized. The mod raises the current save version to `0xb2` at startup while keeping the minimum supported version `0x50`. Static disassembly of `CFTTSerialize::SerializeInternal<uint64_t>` at `0x3ffafd` confirms that loading skips fields newer than the stored save version without advancing the stream; `clear_state()` supplies empty defaults for skipped additions. This supports loading `0xaf` market data with later fields empty, `0xb0` listings with later fields empty, and `0xb1` finance data with the new contract extension empty, as well as initializing an unmodded `0xae` save before new fields are skipped. These paths still need an end-to-end disposable-career load/save check.

Each active club retains its 16 most recent finance entries. Entries track opening account values, operating revenue, wage costs, transfer-budget allocations, and named transfer purchases or sales, including cash, transfer-budget, and payroll deltas. The finance screen can browse this activity for every active club.

Before a move, the bridge checks that the player still belongs to the seller, both clubs exist, the window is open, squad limits hold, and the buyer has fee and wage room. Transfers into the user club call DLS18's `CDataBase::SignPlayer`, followed by `CPlayerDevelopment::AddPlayer`; transfers out call `CDataBase::SellPlayer`. These stock routines update user team management and perform the game's roster validation. AI-to-AI transfers use the lower-level roster links and recalculate links directly. The bridge checks ownership after each path and attempts to undo partial lower-level moves.

The user lifecycle calls and addresses are statically traced from DLS18's callers and implementations. The transaction path is not runtime-confirmed; a disposable career is required to confirm roster/UI behavior and save reload before treating the mod as complete.

## Implementation status

### Implemented features

- Native ARMv7 bridge loaded through the existing DLS18 code cave.
- Hooks for season serialization, the end of `CSeason::PlayTurn`, and transfer-screen entry.
- Versioned career accounts for all active clubs, provisional per-turn finances with surplus-funded transfer budgets, up to 8,192 explicit player-contract records, one transfer window per season, and completed transfer history.
- A saved 16-entry-per-club finance ledger and club activity browser; operating revenue and wage costs are visible separately from transfer transactions.
- AI club targeting and bounded AI-to-AI purchases; candidates must clear the final negotiated fee and wage affordability calculations, and saved per-window purchase-attempt markers prevent repeated failed bids.
- User market browsing by player name, position, and club, a single full-screen offer-terms page with fee and wage steps and 1–5 year contract controls, exact amount entry, preset and exact user buyer/seller fee counters, one-step user wage counters with a final player counter, offers inbox, club finance and squad browsers, and transfer history.
- Dedicated seven-tab market UI with native player cards, selectable-row fallback, and bounded four-item pagination on all seven lists; range counts and page controls occupy a reserved footer.
- Exact whole-number entry for user bid fees, annual wages, contract renewal wages, and buyer/seller fee and wage counters. Input requires a positive whole number; buyer fees and wages are checked against available budgets, and seller counters are answered under the AI buyer's fee ceiling.
- Original roster players receive stable estimated 2–5 season contract terms; the user can negotiate renewals for original and mod-recorded contracts, choose a 1–5 season extension, and record accepted renewals in payroll and finance history.
- AI clubs can automatically renew up to three affordable core players at each season rollover; the saved contract table, payroll, and club finance history record each renewal.
- User player listing and delisting; only listed user players can receive AI bids. Listing and AI attempt state are saved in the `0xb0` extension.
- Saved 16-player shortlist, player-sale confirmation with buyer/fee preview, specific refusal messages for broken talks
  and thin seller squads, negotiation-event descriptions, and rival valuation details in contested transfer history.
- Dynamic-difficulty values `0x13`–`0x15` are set to zero through the market price schedule.
- Current APK packaged from `DLS18_career_market_v26.apk` as `DLS18_career_market_v27.apk`; Squad club navigation and Sales filtering now stay inline; Market and Sales retain independent position filters; the Market toolbar sorts
  matching players by rating, value, or wage with deterministic O(n log n) ordering.

The Python engine in `engine.py` remains an offline reference model. Its `counter_player_wage` flow now mirrors the native one-step user counter and final player counter, but the engine is not called by Android; the native bridge is the code included in the APK.

### Remaining work and limits

1. Confirm the CFE message-box callback behavior and roster updates in a disposable career. Static disassembly cannot prove these runtime effects.
2. Confirm on device that saved `0xaf`/`0xb0`/`0xb1` careers and unmodded `0xae` careers load, save, and reload correctly with the appended `0xb1` finance and `0xb2` contract fields. Static serializer evidence supports the version gating but cannot prove game-level career behavior.
3. Calibrate synthetic revenue and initial budgets. The traced post-match rewards update the user's profile credits rather than club accounts, so they cannot directly supply per-club match income.
4. Validate name search and exact whole-number fee and wage entry on device; both are built into the market and renewal editors.
5. Add season player aging, bonuses, and release clauses. The decoded roster has no confirmed age or birth-year field, and the inspected native simple-player conversion exposes no verified age input; identify a trustworthy source or define a market-only starting-age source before implementing aging. Do not assign fabricated ages. Original contract terms remain estimates because DLS18 does not expose the underlying dates through the inspected roster data.

On 2026-09-25, `DLS18_career_market_ui_alpha.apk` installed successfully on the Xiaomi Pad 6 (`pipa`, model
`23043RP34I`); that build was not opened. V24 is built but not installed, so the current UI and save behavior remain
unconfirmed on-device.
