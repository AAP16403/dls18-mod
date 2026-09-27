# DLS18 Career Transfer Market (v27)

Current build: `mod/build/DLS18_career_market_v27.apk` (61,797,454 bytes). It includes the v14 stat/control library
and v27 ARM career bridge. The game library is byte-identical to v14. V27 is built but not installed or launched.

The market uses coins as its single currency and one transfer window a season, open until 6 league matches are played.
The game's own transfer and sell screens route through market negotiation; the coin hack is removed. See
[BALANCE.md](BALANCE.md) for the balance and current validation record.

`native_bridge.c` is the market that ships in the APK as `libCareerMarket.so`. The code cave in `build_mod.py` hooks
the career turn (`CSeason::PlayTurn`), the save serializer (`CSeason::Serialize`), and the Dream League transfer screen.
The balance model, roster handling and test evidence are in [BALANCE.md](BALANCE.md). The engine
findings behind them are in [data/ROSTER_MECHANICS.md](data/ROSTER_MECHANICS.md) and
[data/NOTES_dataset.md](data/NOTES_dataset.md).

## Current v27 additions (2026-09-26)

- The full-screen market has seven tabs: Market, My Squad, Sales, Inbox, Finances, History, and Shortlist.
- Market, My Squad, Sales, Inbox, and Shortlist now use two-column grids of native player cards in the same full-screen
  market. Tapping a card opens its existing full-screen details/actions; club, position, rating, value, wage, listing,
  and offer status are shown below the cards. A selectable text-row fallback remains if card creation fails.
- All seven lists use four-item pages with Previous/Next controls and a visible range/total count. The card/text area
  reserves a footer so the page controls do not cover player cards or details. Tabs, Market/Sales filters, and Market
  search reset the relevant page; changing pages selects the first item for the right-side details and actions.
- The right pane shows the selected player or offer details above its actions. Empty card lists now keep their
  explanation visible there. Pages with four or more actions use two columns, and shared layout math keeps details and
  controls inside the panel at tablet and minimum supported screen heights.
- The Market tab keeps position, club, and name-search controls above the player cards. Position cycles without leaving
  the list, active filters are highlighted, and page navigation stays in the footer. The player action pane now contains
  only shortlist and offer actions instead of duplicating Previous/Next and filter navigation.
- My Squad keeps previous/selected/next club navigation in a toolbar above its cards; its action pane now contains only renewal or club-finance actions. Finances uses its paged club rows instead of redundant previous/next buttons.
- Sales, Shortlist, and Squad also use card selection and the common page footer instead of duplicating player
  Previous/Next buttons. Their action panes contain only actions for the selected player; page offsets reset after
  club changes, shortlist removal, and completed quick sales.
- Market and Sales keep independent position filters when switching tabs. Sales card collection and selected-player
  navigation now use the same filter. A full-width toolbar above the Sales cards cycles the filter without opening a popup.
- Primary actions use the game's highlighted button scheme consistently. Submit, accept, confirm, continue, offer,
  renewal, and get-offers actions stand out while navigation, rejection, removal, and filters remain neutral.
- The top Back button exits from root lists and returns child pages to their owning tab. It preserves the active player
  or club and clears temporary quick-sale or renewal-editor state when leaving those flows.
- The selected native player card has a three-pixel teal outline rendered over the card, making its relationship to the
  selected details and actions clear. Text-row fallback selection remains highlighted.
- The Market toolbar sorts matching players by rating high-to-low, market value low-to-high, or wage low-to-high.
  Sorting resets to page one and uses deterministic O(n log n) merge ordering across the full player database.
- Card drawing and touch behavior have not been confirmed in-game.
- The 16-player shortlist persists in career saves. Player Sales includes quick-sale confirmation with buyer and fee
  preview; protection, squad minimum, goalkeeper cover, and offer conflicts have specific messages.
- Negotiation screens explain the seller's counters, rival bids and raises, wage counters, and outcomes. The bid flow
  distinguishes broken-off talks (`-16`) from a seller that cannot release the player without dropping below 16 players
  or losing its last goalkeeper (`-17`). Transfer history shows the rival club's valuation for contested signings.
- Opening transfer terms now stay together on one full-screen page: fee and annual wage have direct 10% step controls,
  contract length has one-year steps from 1–5 years, and exact fee or wage entry remains available through the game's
  keyboard.
- Career price rules set dynamic-difficulty variables `0x13`, `0x14`, and `0x15` to zero; the harness checks the values
  after applying and reloading the configuration.

## Market evolution

- **AI transfers now happen.** The alpha moved AI players in the live link table, which the game rebuilds from its
  default table on every `CalculateLinks`, so the moves were undone at once. v3 edits the default table, as the game
  does for server roster updates, saves it with the career (save version 0xB3), and restores a pristine copy before
  every load.
- **User sales reach the buyer.** `SellPlayer` ignores its buyer argument, so the player is first placed at the buyer
  in the default table.
- **Only real clubs trade.** National, all-star and classic sides are excluded; their players are duplicates of club players.
- **Economy and AI rebuilt:** revenue by club size, wages tied to value, board-set budgets, a cash sink, role-based
  asking prices, player willingness, and squad planning against DLS18's real squad shapes. See BALANCE.md.
- **Deterministic:** a career plays out the same with or without app restarts.
- **Stock limits honoured:** squads of 16–32, never the last goalkeeper, and user squad of 17 or more.

## Features

- One window per season: from the season rollover until 6 league matches are played.
- Club accounts with cash, transfer budget, wage budget, payroll, squad value and best-XI strength, plus a per-club
  finance ledger.
- Dedicated full-screen browsing for market players, squad, player sales, offer inbox, finances, transfer history, and
  shortlist.
- AI clubs buy to fix thin positions or upgrade their best XI, sell surplus players, and make 1–3 signings per window.
- The user can:
  - bid for players with a single full-screen fee, wage, and contract editor, plus exact amount entry on the DLS keyboard;
  - list players, receive and counter AI offers, and make a confirmed quick sale;
  - renew contracts;
  - browse the market, club squads, club finances, completed transfers, and a saved shortlist.
- Big clubs may make an unsolicited bid for one of the user's unlisted starters, which the user can reject.
- Contract terms: modelled 2–5 season terms for original players, and saved terms for transfers and renewals.

## Build

From `DLS18_project` (the ARM bridge uses WSL; no NDK or zig needed):

```sh
wsl -e sh mod/career_market/tests/run_tests.sh 12 1       # host test harness on the real dataset
wsl -e sh mod/career_market/build_native_bridge.sh ../build/lib/armeabi-v7a/libCareerMarket_v27.so
python mod/career_market/tests/arm_crosscheck.py mod/build/lib/armeabi-v7a/libCareerMarket_v27.so mod/career_market/tests/small.txt 2
python mod/build_mod.py --out mod/build/lib/armeabi-v7a/libDLS18_market_v18.so
python mod/career_market/tests/test_hooks.py mod/build/lib/armeabi-v7a/libDLS18_market_v18.so
python mod/build_apk.py --base-apk mod/build/DLS18_career_market_v26.apk \
  --lib mod/build/lib/armeabi-v7a/libDLS18_market_v18.so \
  --career-lib mod/build/lib/armeabi-v7a/libCareerMarket_v27.so --out mod/build/DLS18_career_market_v27.apk
```

`build_native_bridge.ps1` (zig) is the older build path. Its zig install lived in a temp folder that Windows has since cleaned.
Compare the harness `api_digest` from `--no-user --restart-every -1` with the ARM cross-check digest on the same dataset
and season count. They must match.

Current build: `mod/build/DLS18_career_market_v27.apk`.

## Saves

- A save from the unmodded game or an older market build loads normally. The market initialises, and the AI rosters start from the
  game's defaults. Market data from the alpha is kept, but the alpha's AI moves never really happened.
- Market saves use extension fields through version 0xB5. Keep a backup before moving to an older build, especially after
  a season has been saved with newer market features.

## Install on Xiaomi HyperOS

If `adb install` returns `INSTALL_FAILED_USER_RESTRICTED`, enable **Settings → Apps → Permissions → Install via USB**
on the tablet and accept the on-screen prompt. The APK is signed with the same key as earlier builds, so `adb install -r`
updates in place and keeps the save.

## Python reference model

`engine.py` is an older offline model of the alpha rules (windows, counters, JSON persistence). It does not drive the APK
and does not include the v3 balance or roster changes. For v3, `tests/harness.c` is the reference, since it runs the real C code.
