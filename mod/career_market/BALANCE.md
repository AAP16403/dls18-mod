# Career market: balance model, roster handling, UI, and test evidence

This document records the career market rules, constants, and test evidence. Money is in DLS player-value units: a player
is worth 50–4,125 and a club squad 3,400–30,000. Positions are 0 GK, 1 DEF, 2 MID, 3 FWD.

## 0o. v27: inline Squad club navigation (2026-09-26)

V27 moves My Squad's previous/next club controls into a persistent toolbar above the player cards, with the selected
club named between them. The right pane now contains only the selected player's relevant action: contract renewal for
the user club or a link to the selected club's finances. The Finances pane likewise removes redundant previous/next
club and menu actions because its paged club rows and top tabs already provide those routes.

The host harness reserves and bounds toolbar space for Market, Sales, and Squad at 768 px and 300 px heights.
`test_hooks.py` reports 8/8 Squad-navigation checks and `ALL OK`. The 12-season host run completed with 0 violations
(`api_digest=ba4f50439bcba846`); ARM and matching host runs both produced `api_digest=b0d31a38190d3683`.

Artifact: `mod/build/DLS18_career_market_v27.apk` (61,797,454 bytes; 1,014 entries + v1 signature), SHA-256
`D0ED942881E65A3FD001D6CADCF17BFE22CCE08D65FAFF3EB9B7BF85E5D1DEAF`. The v27 career bridge SHA-256 is
`8114D147F492327CC32157334417E698A27A697EC691533DC426C8835CF684FD`. V27 has not been installed or launched.

## 0n. v26: inline Sales filter toolbar (2026-09-26)

V26 moves the Sales position control out of the player action pane and into a persistent full-width toolbar above the
Sales card grid. The button cycles All, GK, DEF, MID, and FWD in place, highlights active filters, resets pagination,
and immediately selects the first matching player. Listing and quick-sale remain as the selected player's only actions.

The host harness reserves and bounds filter-toolbar space for Market and Sales at 768 px and 300 px heights.
`test_hooks.py` reports 9/9 Sales-filter checks and `ALL OK`. The 12-season host run completed with 0 violations
(`api_digest=ba4f50439bcba846`); ARM and matching host runs both produced `api_digest=b0d31a38190d3683`.

Artifact: `mod/build/DLS18_career_market_v26.apk` (61,797,460 bytes; 1,014 entries + v1 signature), SHA-256
`2EA1AB2FFBB9D8FAB9037DE86FF2A6FFCBF806D40D74FF40D51DBC81904E0912`. The v26 career bridge SHA-256 is
`C87CC8AF080C9FD011178897E1DBE87B3E696C6B77DF4CF700DEC7967AD83EC7`. V26 has not been installed or launched.

## 0m. v25: independent persistent position filters (2026-09-26)

The v25 bridge gives Market and Sales separate position-filter state. Switching tabs preserves each tab's filter,
and the Sales card collector now applies the same filter as the selected-player navigator. The Sales position chooser
keeps the Sales tab and Sales card list visible instead of temporarily presenting the Market list.

The host harness verifies exact filtered Sales-card collection and confirms that changing the Sales filter does not
alter the Market filter. `test_hooks.py` reports 7/7 Sales-filter wiring checks and `ALL OK`. The 12-season host run
completed with 0 violations (`api_digest=ba4f50439bcba846`); ARM and matching host runs both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v25.apk` (61,797,454 bytes; 1,014 entries + v1 signature), SHA-256
`A59A0191C0E324C27388CE062997C25FCDE63E673F11FF74A1534761B614B255`. The v25 career bridge SHA-256 is
`718BD89564097F69DCDEBCFB275BBA2EA4D55A5D9E719103EAB78DF018294970`. V25 has not been installed or launched.

## 0l. v24: sorted market results (2026-09-26)

The v24 bridge adds a fourth persistent Market toolbar control for sorting. It cycles through rating high-to-low,
market value low-to-high, and wage low-to-high. Changing the order returns to page one and selects the first sorted
player. An iterative merge sort handles the full player database in O(n log n) time with deterministic player-ID
tie-breaking, avoiding quadratic work when thousands of players match.

The host harness verifies exact rating, value, and wage orders on shuffled input. `test_hooks.py` reports 6/6 sort
wiring checks and `ALL OK`. The 12-season host run completed with 0 violations
(`api_digest=ba4f50439bcba846`); the ARM and matching host runs both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v24.apk` (61,797,450 bytes; 1,014 entries + v1 signature), SHA-256
`9016D763011765038A9CF979CD453F4E73457E254E4FCD8AF51DCC1C08719A04`. The v24 career bridge SHA-256 is
`FE4ED34B491906DD13D2C74E6A47FA577D508D9DC9460C86A8F5C11AE63746CD`. V24 has not been installed or launched.

## 0k. v23: visible selected-card outline (2026-09-26)

The v23 bridge draws a three-pixel teal outline around the selected native player card. The outline is rendered after
the card, so it remains visible above the card art and connects the selected item clearly to the details and actions in
the right panel. Text-row fallback selection continues to use the highlighted button scheme.

The host harness verifies all four outline edges, exact geometry, accent colour, and invalid bounds. `test_hooks.py`
reports 5/5 outline wiring checks and `ALL OK`. The 12-season host run completed with 0 violations
(`api_digest=ba4f50439bcba846`); the ARM and matching host runs both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v23.apk` (61,797,452 bytes; 1,014 entries + v1 signature), SHA-256
`ACE5D2319FDBA56C90F6689166CF2B8B1585EE62644B2A290CA5EEA1AFCD1621`. The v23 career bridge SHA-256 is
`244DC8366C64B2541417B404F1F732C345FCE56C4EC84334E71439F34DCE0A34`. V23 has not been installed or launched.

## 0j. v22: contextual Back navigation (2026-09-26)

The v22 bridge makes the dedicated screen's top Back button context-aware. On the seven root lists it returns to the
game's transfer screen. From offer terms, quick-sale confirmation, club filters, finance activity, season books,
contract renewal, negotiation pages, and notices it returns to the owning market tab while preserving the current
player or club. Pending quick-sale and renewal-editor state is cleared when leaving those child pages. Menu/close
notices still exit cleanly instead of reopening themselves.

The host harness classifies all seven roots, six representative child pages, and notice routing correctly.
`test_hooks.py` reports 6/6 contextual-back wiring checks and `ALL OK`. The 12-season host run completed with 0
violations (`api_digest=ba4f50439bcba846`); the ARM and matching host runs both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v22.apk` (61,797,451 bytes; 1,014 entries + v1 signature), SHA-256
`5CC9D48E9ACB46D27001892B80E8CD78077DDBFD46CC579F7556FA9F5BF90228`. The v22 career bridge SHA-256 is
`3223653EA78CEAAC1CDD5DBA9108F9285C71437D7C0D92A0C3C3477F109FA09E`. V22 has not been installed or launched.

## 0i. v21: primary-action hierarchy (2026-09-26)

The v21 bridge uses the game's verified highlighted button scheme for primary actions in the dedicated market. Continue,
Make Offer, Confirm Sale, Submit Offer, fee/wage acceptance, contract renewal, and Get Offers Now are highlighted;
Back, Walk Away, Reject, Remove, listing, and filter controls remain neutral. The mapping is centralized so the same
action has the same visual priority across market, inbox, sales, and contract flows.

The host harness classified all 11 primary and 6 neutral labels correctly. `test_hooks.py` reports 5/5 scheme wiring
checks and `ALL OK`. The 12-season host run completed with 0 violations (`api_digest=ba4f50439bcba846`); the ARM and
matching host 2-season runs both produced `api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v21.apk` (61,797,450 bytes; 1,014 entries + v1 signature), SHA-256
`8FF528EE06B09B41444AA1A309824D0DAF9B5289B49E7C4EFE064911D4D10B4C`. The v21 career bridge SHA-256 is
`0173EC9B2476B277A2252AB4169D013D21D21086E9D3EAD590B47E6863743958`. V21 has not been installed or launched.

## 0h. v20: focused card-list actions (2026-09-26)

The v20 bridge removes the remaining one-player-at-a-time navigation buttons from the Sales, Shortlist, and Squad
action panes. Players are selected directly from cards and moved through the common page footer. Sales now exposes
Position Filter, List/Unlist, and Quick Sale; Shortlist exposes Remove and Make Offer; Squad retains club navigation
plus the selected player's contextual renewal or finance action. Club changes, shortlist removals, and completed quick
sales reset the affected page so the selected item cannot remain off screen.

`test_hooks.py` reports all 4 focused-action checks, 7/7 filter-toolbar checks, 13/13 pagination checks, and 5/5
selected-detail checks passing. The 12-season host run completed with 0 violations
(`api_digest=ba4f50439bcba846`). The 2-season ARM cross-check and matching host run both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v20.apk` (61,797,455 bytes; 1,014 entries + v1 signature), SHA-256
`F11663B9B29F59DD99390B0224A93CBFAD2C51E1E09E35B5238A29C3ED73707D`. The v20 career bridge SHA-256 is
`4CC401638C27ECE4A00A5F8963BDCA2CF1F772BD68931DB3F168B01EF334AB7A`; the game library remains SHA-256
`05DDE1F7AAA0E7D591D7A7207721FCF30E3CA04255726E63686173B363FA3B64`. V20 has not been installed or launched.

## 0g. v19: persistent Market filters (2026-09-26)

The v19 bridge moves the Market tab's position, club, and name-search controls into a persistent toolbar above the
player cards. Position cycles in place through All/GK/DEF/MID/FWD, active filters use the highlighted button scheme,
and changes reset the Market list to page one. Previous/Next navigation remains in the reserved page footer, so the
selected player's action pane now contains only shortlist and offer actions.

The host harness confirms the toolbar reserves 9% of screen height only on the Market tab at 768px and 300px heights.
`test_hooks.py` reports all 7 toolbar wiring checks, all 13 pagination checks, and all 5 selected-detail checks passing.
The 12-season host run completed with 0 violations (`api_digest=ba4f50439bcba846`); the 2-season ARM cross-check and
matching host run both produced `api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v19.apk` (61,797,450 bytes; 1,014 entries + v1 signature), SHA-256
`17C47056D84F7D8BB5FF029AE9360BF2C7864ACCD4FC6D833990D032D93E27E6`. The game library remains SHA-256
`05DDE1F7AAA0E7D591D7A7207721FCF30E3CA04255726E63686173B363FA3B64`; the v19 career bridge SHA-256 is
`B708E3F2197E585C85A5A81D043A01745AA9008528E76C9E7BB2D1E746A1F510`. V19 has not been installed or launched;
rendering, filter touch behavior, negotiation callbacks, and save/reload remain unconfirmed in-game.

## 0f. v18: selected details and compact actions (2026-09-26)

The v18 bridge adds a selected-details pane above the action buttons on the right side of every card-list screen.
It displays the current selection's existing description, including the reason when a list is empty. Action buttons
now use a shared responsive layout: the pane reserves up to 20% of screen height, buttons keep a minimum height of
5.5% of the screen, and screens with four or more card actions use two columns when needed. This keeps player details
visible while retaining every action on tablet and minimum-height layouts.

The host harness checks 1–10 action layouts at 768px and 300px heights, including button bounds, minimum size,
details/action separation, and empty-list space. `test_hooks.py` reports all 5 selected-detail wiring checks and all
13 pagination checks passing. Stat curves and patch emulation pass. The 12-season host run completed with 0
violations (`api_digest=ba4f50439bcba846`); the 2-season ARM cross-check and matching host run both produced
`api_digest=b0d31a38190d3683`, with 0 host violations.

Artifact: `mod/build/DLS18_career_market_v18.apk` (61,797,451 bytes; 1,014 entries + v1 signature), SHA-256
`C2B6E26430DC3AAB1007DEAC16B4569E48B2EB5871FAF606D324E0E6AA0155A1`. The game library is byte-identical to v14
(SHA-256 `05DDE1F7AAA0E7D591D7A7207721FCF30E3CA04255726E63686173B363FA3B64`); the v18 career bridge SHA-256 is
`844B94D9C5487C3D41E7B5EA61B12F6634FFFAD6852BEBED28CD825B0C3731F9`. The v18 APK has not been installed or
launched. Card rendering, details layout, action touch behavior, and save behavior remain unconfirmed in-game.

## 0e. v17: complete paged browsing in the full-screen market (2026-09-26)

The v17 bridge adds stable four-item pages to all seven dedicated market lists: Market, My Squad, Sales, Inbox,
Finances, History, and Shortlist. The left panel shows Previous/Next controls when available and a visible range/total
counter (for example, `1-4 / 5816`). Changing tabs starts that list at page one; changing Market/Sales filters or the
player search resets the affected list to page one. Moving pages selects the first item on the new page so its details
and actions stay visible on the right. The footer is reserved from the card grid and text description layout. Native
player cards retain the selectable text-row fallback.

The host harness checks page alignment, empty lists, final partial pages, and both navigation bounds. `test_hooks.py`
checks the page controls, count display, footer, and all seven list modes. The 12-season host run completed with 0
violations (`api_digest=ba4f50439bcba846`); the 2-season ARM cross-check and matching host run both produced
`api_digest=b0d31a38190d3683`, with 0 host violations. These checks do not render the UI or simulate touch input.

Artifact: `mod/build/DLS18_career_market_v17.apk` (61,797,449 bytes; 1,014 entries + v1 signature), SHA-256
`5BF8D170AF90406EC6DEDDA417A0E0977399F524D4A289A125828C902E906EB1`. The game library is byte-identical to v14
(SHA-256 `05DDE1F7AAA0E7D591D7A7207721FCF30E3CA04255726E63686173B363FA3B64`); the v17 career bridge SHA-256 is
`9465A4B872E528B26AC74C4920D9442774300DB9D81E10897A13AF7B1B67E6A9`. ADB lists no connected devices, so v17 is
built but not installed or launched; card rendering, paging, and touch behavior remain unconfirmed in-game.

## 0d. v16: player cards across the market (2026-09-26)

The v16 bridge builds up to four native `CFEPlayerCard` children in a two-column grid on Market, My Squad, Sales,
Inbox, and Shortlist. Tapping a card uses each tab's existing full-screen detail and action flow. Club, position,
rating, value, wage, listing, and offer-response summaries remain visible below the cards. If card construction fails,
the screen deletes any partial cards and uses selectable text rows. Finances, history, and books retain their existing
full-screen text layouts.

The v16 game library is byte-identical to v14. `analysis/stat_curves.py`, `test_patch.py`, and `test_hooks.py` report
`ALL PASS` / `ALL OK`; the 12-season host run has 0 violations and `api_digest=ba4f50439bcba846`. On `small.txt`,
the 2-season ARM and matching host runs both report `api_digest=b0d31a38190d3683`; host violations are 0. These checks
do not exercise card rendering or touch input. ADB lists no device, so runtime behavior is unconfirmed.

Artifact: `mod/build/DLS18_career_market_v16.apk` (61,793,355 bytes), SHA-256
`9C50E4369F4AB38BF7122F37658B6DC41D5DD702BDB4CF78004A6AD643BF77D6`.

## 0c. v15: full-screen Inbox and Shortlist cards (2026-09-26)

The v15 bridge uses native `CFEPlayerCard` children in a two-column grid for the Inbox and Shortlist tabs inside the
existing dedicated full-screen market. A card tap follows the existing offer/player detail and action flow; club and
fee/status lines sit below each card. If card construction fails, the screen falls back to its selectable text rows.
The game-library file is byte-identical to v14; only the career bridge and packaged APK changed.

The v15 host run (`12` seasons, seed `1`) completed with 0 violations and `api_digest=ba4f50439bcba846`. On
`small.txt`, ARM and matching host runs over 2 seasons both produced `api_digest=b0d31a38190d3683`; host violations
were 0. `test_hooks.py` reports `ALL OK`. These checks do not execute the new card rendering or touch path. ADB lists no
device, so the v15 screen has not been run in the game.

Artifact: `mod/build/DLS18_career_market_v15.apk` (61,793,355 bytes), SHA-256
`232CBC2B3670B57810F57096EB411D18CA280D48A8730291E7302FB42E9FE392`.

## 0b. v14 build status (2026-09-26)

The v14 checkpoint package was `mod/build/DLS18_career_market_v14.apk`, built from the v13 APK with the v14 game library and
career bridge. The seven-tab full-screen market retains its shortlist, sales and quick-sale flow, inbox, club books,
and transfer history. Opening fee, annual wage, and contract length are now adjusted together on one full-screen offer
page; exact fee and wage entry still uses the game's keyboard. The `k_price_rules` schedule sets dynamic-difficulty
values `0x13`–`0x15` to zero; harness checks cover the schedule and save/reload.

The v14 host simulation completed 12 seasons with seed 1 and 0 violations (`api_digest=ba4f50439bcba846`). The
offer-terms harness checks passed for fee/wage steps, one-year adjustments, and the 1–5 year limits. On
`small.txt`, the 2-season ARM cross-check and matching host run both produced `api_digest=b0d31a38190d3683`; the host
reported 0 violations. Stat curves, patch emulation, and game-hook checks pass. ADB lists no devices; v14 has not been
installed or launched.

## 0a. v5 (2026-09-25): user willingness fix

The AI willingness rules (section 4) blocked the user: a dream team ranks last by squad value (rep 1), so every starter elsewhere
was "club won't sell" and anyone 12+ above the user's best XI refused. From the starting team (best XI 56) only 272 of 953
players rated 65-69 were signable, and 1 of 1,274 rated 70-74. v5 gives the user its own rules (AI-to-AI unchanged; the `--no-user` digest is identical):
- a player refuses only if he is `USER_STAR_STRENGTH_GAP` (18)+ above the user's best XI **and** his club is `USER_STAR_REP_GAP` (40)+ rep bigger;
- clubs sell starters to the user for a premium of rep_gap/3 % (capped at 25%). Key players cost rep_gap/2 % (capped at 40%) and are refused only when the gap is more than 50;
- the game's Buy button says no (with the ratings) before the terms screen.
Result from the starting team: all players rated 65-69 are signable (average ask 375 coins), 1,109/1,274 rated 70-74, and 17/613 rated 75-79. Harness: 0 violations on 3 seeds × 12 seasons, and restarts are deterministic.
`mod/build/DLS18_career_market_v5.apk` = the v4 game library + `libCareerMarket_v5.so`. `tests/harness --eligibility` prints the table.

## 0. v4 changes (2026-09-25): one currency, one window, the game's own screens

- **One currency: coins.** The user club's money is the game's coin balance (CMyProfile credits, profile+0x2A7CC).
  Purchases and sales change coins through `CCredits::SetCredits`, which also refreshes the HUD. The user club gets no
  simulated revenue, because match rewards are its income (about 300-450 coins a season; `data/STOCK_TRANSFER_FLOW.md` section 5).
  It pays a small wage bill in coins every turn, at 2% of value a season (AI clubs pay 12% of value). A signing must leave enough coins
  to pay a season of the resulting wage bill. Player values are already coin prices, so AI clubs keep their own balances
  in the same units. The market screens show everything in coins.
- **One transfer window a season:** open from the season rollover until 6 league matches are played. There is no mid-season window.
  `CSeason::GetCurrentTurn` is a schedule slot, not a match count: league round r is slot `GetStartLeagueTurn() + 2(r-1)`
  (usually 18, 20, ...), with cup rounds on the odd slots. The window is open while slot <= start + 10, and the boundary is saved in
  `reserved[1]`. The old "turns 0-1" window never matched a real season.
- **The game's own transfer screens route through the market** (build_mod hooks at 0x277006 search, 0x250A3C scouting, and 0x23E404
  sell). The stock instant purchase (fixed coin price at any time, with the in-app coin shop) and the stock sale (50% of value to a
  random club, which reverted to his old club) are skipped. `career_market_stock_action` shows "Transfer Window Closed" outside the
  window. Otherwise it opens the market negotiation (asking price, counter-offer, wage terms, paid in coins). Selling offers
  "Get offers now" (the best current bid from an AI club, to accept, counter or reject) and listing.
- **Coin hack removed:** the base APK's `CMyProfile::GetCredits` always returned 16,777,215. build_mod restores it, and a save
  holding exactly that value is reset once to 1,000 coins.
- **Test evidence:** harness season slots are 17-56 (league 18-46) with simulated match income. Over 12 seasons and 3 seeds there are 0
  violations, AI activity goes from 137 transfers a season down to 40-60, wages stay at 63-65% of revenue, top/bottom strength
  holds at 82/66, and Gini stays at 0.20. The user's coins go from 1,500 to about 2,000 with a wage bill of about 90 a season. The final state is
  identical with restarts every turn, every 5 turns, or never. The ARM library matches the harness digest on small.txt
  (`8961c9a0617bdf08`). All six cave hooks were emulated (correct library and symbol names, arguments, exits).
- Current build: `mod/build/DLS18_career_market_v4.apk` (same save version 0xB3, so v3 saves load).

## 1. Which clubs trade

The market includes only real clubs: the 152 league clubs, the 18 relegation-league clubs, and the user club (171
accounts). `CTransfers::IsValidSearchTeam` also accepts national teams (league 9–13), all-star/misc sides
(17), and classic teams. 431 players appear on both a club roster and one of those squads, so the bridge
also excludes teams for which `CDataBase::IsTeamInternational` (0x20C0A8),
`IsTeamMiscellaneaous` (0x20C08E), or `IsTeamClassic` (0x20C048) is true.

## 2. Rosters really move (and persist)

See `data/ROSTER_MECHANICS.md`. The game rebuilds its live link table from a *default* table on every
`CalculateLinks` call. That happens at load, on every user signing or sale, and when the mod asks for it.
Only the user team's link is saved. The alpha moved AI players in the live table, so every AI transfer
was silently undone. v3 handles moves as follows:

| Move | How |
|---|---|
| AI → AI | Edit the **default** table under `SetOverrideLinks`: `CanRemovePlayer`/`CanAddPlayer` checks, `RemovePlayerFromLink`, `VerifyLink` (passes XI/captain flags on), `AddPlayerToLink`. Once per batch: regenerate the default simple links, then `CalculateLinks(1,0,0)` and `CalculateTeamRating` for the clubs involved. |
| AI → user | Stock `SignPlayer(info, seller, spec, 1, 0, 1)` (forceShirt 0 like `SignPlayerAttempt`), then `CPlayerDevelopment::AddPlayer`. |
| user → AI | `SellPlayer` ignores its buyer, so the player's default-table entry is moved to the buyer first. `SellPlayer(..., 1)` then removes him from the user and the recalculation places him at the buyer. User-created players (no default club) are not sellable. |
| Save | The whole default table (count + 33 u64 words per link) is serialized with minimum version 0xB3. `build_mod.py` sets the profile save version to 0xB3. On every load the bridge restores a pristine copy of the table, which it captures on its first call in the process. It then applies the saved image if the team ids match, so moves never leak between careers. |

Stock limits are enforced: squads stay between 16 and 32, and a club never loses its last goalkeeper. The user
squad stays at 17 or more, because below that the game auto-signs free players. An AI club also keeps its position minimums.

## 3. Economy

| Rule | Value |
|---|---|
| Reputation | 1–100 from each club's squad-value rank (recomputed, not saved) |
| Annual revenue | squad value × (16% + 8% × (100 − rep)/100), with a floor of 25% of the average club's revenue |
| Player wage demand | 12% of value; moving down the reputation ladder adds rep_gap/2 %, a big step up takes −5% |
| Wage budget | 66% of revenue (never below the current bill + 5%) |
| Cash reserve | 25% of the wage bill; it cannot be spent on fees |
| Transfer budget | Set by the board when each window opens: 40–75% (by reputation) of cash above the reserve. The user club gets 75%. |
| Sales | Full fee to cash, 60% of it added to the transfer budget |
| Opening cash | squad value × (12% + 18% × rep/100), at least 30% of revenue |
| Per turn | revenue/turns in, wage bill/turns out (ledger entries for the user club only, so AI ledgers keep their transfers) |
| Season rollover | 50% of cash above 1.5 × revenue goes to "Stadium & facilities investment" (a money sink) |
| Renewals | Demand = value-based wage × role (surplus 85%, rotation 95%, starter 105%, key 115%), +5% in the final season. Never compounds on the current wage. |

## 4. Valuation and negotiation

- **Role** at the current club: key player (best at his position and 4+ above team strength), starter (in
  the best XI 1-4-4-2), rotation, or surplus (position overfull or 8+ below the weakest starter).
- **Asking price:** value × role (80/100/125/150%) × contract (expired 30%, final season 60%, 1 year left 80%, 2 years left 92%),
  × 85% when the position is above its maximum, × 75% for surplus players at clubs with more than 25 players, and × 85% if the seller
  is below its cash reserve.
- **Buyer ceiling:** value × (100% + 10% per need level + 3% per rating point of improvement to the XI, up to +30%).
- **Fee:** ask + 35% of (ceiling − ask). A user bid that meets the asking price goes straight to wage terms.
- **Willingness:** a star (12+ above the buyer's strength) will not join a club more than 10 reputation
  points smaller, and nobody drops more than 45 points. Clubs don't sell starters to clubs more than 8 points
  smaller, or key players to any smaller club. The same rules apply to user bids, which get the notice "Player Not Interested".

## 5. AI squad building

- **Squad shape** (from the real DLS18 squads): minimum GK 2 / DEF 5 / MID 6 / FWD 2, target 2/7/8/3, maximum 3/10/12/5.
  Squad size is 16–30, and above 26 an AI club only buys for a real need or an upgrade of 5+ points.
- **Needs:** below the minimum is level 3, below the target level 1. Candidates must lift the best XI by 2+ points or fill a
  need with a decent player. A thin position may take a modest player (within 10 of team strength) if he costs at most 1/6 of the budget.
- **Choice score:** improvement × 100 + need × 250 + rating × 2 − 200 × fee/fee_room.
- **Limits:** 1–3 buys per window (by reputation), at most 2 sales per club per window, two passes per window
  turn in a rotating club order.
- **Releases:** a club only lets non-surplus players go while it stays at or above its target depth.
- **Bids for the user's players:** AI clubs bid on listed players and may bid unsolicited for an unlisted
  user starter.
  - **Sale value (v37d, user_sale_value):** market value × the seller's role weights {90, 100, 120, 150}.
  - The premium above market value is scaled by the user club's reputation: 20% at rep 1, 33% at 20, 60% at 50, 90% at 80, full at 100. A key player is worth about +10% at the bottom of the pyramid and +50% at the top.
  - Listed players get bids from 90% of the sale value up to 100% of it.
  - Unsolicited bids run from 100% of the sale value up to 115% of it.
  - The buyer's own valuation (buyer_max_price) must still reach the bid.
- **Cache:** the market cache is rebuilt from the live rosters at the start of every turn, so a career plays out
  identically whether or not the app was restarted.

## 6. Test evidence (no device, no game run)

`tests/harness.c` compiles the real `native_bridge.c` for x86-64 Linux (WSL). It maps a fake library image at
0x10000000 with `jmp` thunks at every game offset. Mocks model the default and live link tables, the override pointer,
`CalculateLinks`, `SignPlayer`/`SellPlayer` (including the ignored buyer), and the stock `CanAdd`/`CanRemove` limits.
The data comes from `data/dls18_dataset.json`: all 232 teams and 5,816 players, with values and ratings from the game's own formulas, checked in
Unicorn. The test user starts with a realistic weak dream team.

Checked on every turn and season:
- No player sits on two clubs.
- Every market club has 16+ players and a goalkeeper.
- The market cache matches the live rosters.
- `state_is_valid()` holds and budgets never exceed cash.
- `VerifyLink` never runs on a club below 16.
- Save/restart round trips (a fresh-process reload of pristine links, the user link and the market block) reproduce the market state and every AI
  roster exactly.
- A save older than 0xB3 loads with pristine rosters.

Results with 12 seasons × 3 seeds (`tests/balance_results.txt`, per-club CSVs `tests/balance_seed*.csv`):

| Metric | Season 0 | Season 11 |
|---|---|---|
| Transfers per season | ~191 (first reshuffle) | 30–61 (steady state; DLS never changes AI player ratings) |
| Clubs buying | 138 / 171 | 20–39 |
| Fee / native value | 95% | 100–111% |
| Wages / revenue | 63% | 64% |
| Average cash | 3,649 | 5,034 (flat since season ~8) |
| Clubs below their cash reserve | 0 | 0 |
| Squad size min / median / max | 17 / 21 / 25 | 19 / 20 / 27 |
| Best-XI strength, top 10 / bottom 10 | 82 / 64 | 82 / 66 |
| Squad-value Gini | 0.233 | 0.198 |
| Violations | 0 | 0 |

Determinism: the same seed gives a byte-identical final state whether the career is never reloaded or reloaded
twice a season, every 5 turns, every 3 turns, or every turn.

ARM cross-check: `tests/arm_crosscheck.py` loads the shipped, stripped `libCareerMarket.so` into Unicorn,
applies its relocations, and runs it against Python versions of the same mocks. On `tests/small.txt` (26 teams)
it produces the same API digest as the x86 harness (`6eac399cccfe7176` for 2 seasons), so the ARM binary
behaves exactly like the tested code.

Not verified: behaviour inside the real game (the device was not used, as requested), the real `CalculateLinks`
and `VerifyLink` side effects beyond what the static trace documents, and the UI flow.

## 7. Rebuild and retest

```sh
wsl -e sh mod/career_market/tests/run_tests.sh 12 1        # harness (needs gcc in WSL)
wsl -e sh mod/career_market/build_native_bridge.sh         # ARM .so (arm-linux-gnueabi-gcc in WSL)
python mod/career_market/tests/arm_crosscheck.py <so> mod/career_market/tests/small.txt 2
python mod/build_mod.py --out mod/build/lib/armeabi-v7a/libDLS18_market_v3.so
python mod/build_apk.py --base-apk mod/build/DLS18_career_market_ui_alpha_fixed2.apk \
    --lib mod/build/lib/armeabi-v7a/libDLS18_market_v3.so \
    --career-lib mod/build/lib/armeabi-v7a/libCareerMarket_v3.so --out mod/build/DLS18_career_market_v3.apk
```

Tune the constants at the top of `native_bridge.c` (the "Balance model" block), rerun the harness, and compare
`balance_results.txt`.

## Contract renewals and keeping players (v38, Club Hub)
- **Demand:** renewal_wage_demand, unchanged.
- **Acceptance:** he signs at or above a hidden point between 88% and 100% of that demand, fixed per player and season.
- **Counters:** a lower offer gets a counter two thirds of the way to his demand, never below his point.
- **Patience:** each short offer uses 0.4 of a strike plus 4 strikes per 100% short. Three strikes end the talks until next season.
- **Keep:** the user may keep up to 16 players. A kept player gets no unsolicited AI bids. Listing a player releases his keep.
