# Career market v27: handoff (2026-09-26)

Current status (2026-09-26): the v27 market bridge and APK are built. The full-screen market has seven tabs, bounded
four-item pages with Previous/Next controls and visible range/total counters on every list, a persistent shortlist,
native two-column player-card grids for Market, My Squad, Sales, Inbox, and Shortlist, sales/quick-sale flow, offer inbox,
club books, transfer history, a selected-details pane above responsive action buttons on every card-list screen, a
persistent position/club/name filter toolbar above the Market cards, and one integrated offer-terms page with
fee, wage, and contract steppers; a harness check covers the step sizes and 1–5 year bounds. Negotiation messages
identify every `NegotiationEvent`; `-16` and `-17` user-bid
refusals explain broken talks or a thin seller squad, and contested history entries show the rival club's valuation.
The optional dynamic-difficulty values `0x13`–`0x15` remain zeroed. The stat, hook, host simulation, and ARM parity checks
pass, including filter/action-layout and page-bound tests, 0 host violations, and matching host/ARM
`api_digest=b0d31a38190d3683`. See `BALANCE.md` and `../STAT_REWORK.md` for test results and hashes. v27 is built but
not installed or launched. The card draw, selected-details layout, page controls, action touch path, and save behavior
have not been exercised in-game.

V27 adds a persistent previous/selected/next club toolbar above Squad cards and removes redundant club navigation from Finances. It also retains the v25 separation of Market and Sales position filters. Both persist across tab changes; Sales cards and selected-player
navigation use the same Sales filter. A persistent toolbar above the Sales cards changes the filter in place.

The remainder of this file records the original v6 build work and is historical. Its old “not started” and v6 output
statements do not describe the current implementation.

Read first:
- `DLS18_project/CLAUDE.md`
- `mod/PATCHING_RULES.md`. Mandatory. Rule 10: never patch files containing backslashes through a Git Bash
  heredoc (it has broken `\n` several times this session). Use the Edit/Write tools or a `.py` patch file.
- `BALANCE.md`
- `data/ECONOMY_HOOKS.md` (match/season hooks, verified addresses)
- `data/UI_COMPONENTS.md` (native UI recipes)
- `data/STOCK_TRANSFER_FLOW.md`

User constraints:
- Never store temp files on C: (use `F:\android modding\tmp`).
- At most 2 subagents at a time.
- Don't run or install the game unless the user asks.
- Another session may edit files concurrently: re-read before editing.
- New build outputs get new file names; the current package is v27.

## Goal (the user's words, paraphrased)

A finished, commercial-quality transfer market and club economy for Dream League Soccer 2018 career mode:
- clubs value players from supply and demand and interact (auctions, rival bids);
- fair pricing, and match rewards that replace the predatory coin trickle;
- a tight transfer flow;
- real interactive native UI instead of chains of option-list prompts.

## What v6 already does (in `native_bridge.c`)

1. **Valuation** (search `Valuation.`):
   - The market index per position and 5-point rating band is supply (releasable players) against demand (clubs that
     would improve and can afford). The target is computed in `compute_market_index()` on every cache build. The saved
     index `g_ext.market_index` moves a third of the way per match turn in `update_market_index()`, range 88-122%.
   - `market_value()` = native value × index.
   - `seller_reservation()` is the hidden minimum: role 85/100/120/150%, contract factor, replaceability,
     overfull/clearance, cash pressure. Discounts can't take it below 70% of market value.
   - `seller_asking_price()` is the quote: reservation × markup 100/108/112/120%.
   - `buyer_max_price()`: need, XI improvement, rich club +10%.
2. **AI-to-AI auctions:** `resolve_ai_bids()`. Clubs pick targets in parallel; contested players go to the highest
   valuation at a second-price fee. The runner-up is stored in `g_ext.history[]` for a news feed.
3. **User negotiation** (search `User negotiations (v6)`):
   - hidden reservation plus a smaller-club premium; user purchases floored at 85% of market value (`USER_BUY_FLOOR_PCT`);
   - asking price, halfway counters, a softened quote for near misses, insult strikes (3 = talks off for the window);
   - round limit 6;
   - an AI **rival** may join (`find_rival`), raising in 5% steps up to its valuation and signing the player if the
     user walks away (`rival_takes_player`);
   - personal terms, with bench players asking +15% wages and starters −5%.
   - Every call sets `g_neg_event` (enum `NegotiationEvent`) plus `g_neg_rival_team`/`g_neg_rival_fee` for the UI.
   - New error codes: −16 talks broken off, −17 selling club squad too thin.
4. **Selling:**
   - AI bids for user players are at market prices: listed 90–100%, unsolicited for starters 115%
     (`USER_SALE_CEILING_PCT`, `UNSOLICITED_PREMIUM_PCT`).
   - A user counter moves the buyer halfway, up to its valuation, then it makes a final offer or walks away.
   - `career_market_quick_sale()` is always available in the window: 45% of market value, to the club where the player
     fits best.
   - Anti-flip: players the user signs can't be sold until next season (`note_user_signing` / `user_signing_locked`).
5. **Roster fixes:**
   - `default_move(..., check_remove)`: user sales no longer check the stale original club's squad size.
   - `default_add` / `default_remove`: unattached players (the free players a dream team starts with) can now be sold.
   - The user is exempt from the AI "2 sales per club per window" cap.
6. **Economy** (search `User club economy (v6)`):
   - Division targets (coins a season): Elite 4,300, JE 3,400, Div1 2,700, Div2 2,100, Div3 1,600, Academy 1,200.
   - Per match: prize money for W/D/L, gate receipts, goal/clean-sheet bonus, cup prize.
   - Season end: TV 25%, sponsor 15%, league prize 20%→2% by position, promotion bonus.
   - `user_match_payout()` / `user_season_end_payout()` pay coins and fill the season books `g_ext.econ`.
   - User wages are now 12% of value, the same as AI (`USER_WAGE_RATE_PCT`).
   - User reputation is floored by division (Elite 80 … Academy 5).
   - AI contract renewals: up to 40 a club (was 3); expired/final-year discounts are 45/65%.
7. **Save:** new extension block `g_ext` (`MarketExt`, `MARKET_EXT_SAVE_VERSION 0xB4`), cleared in `clear_state`,
   serialized after the rosters. The harness writes version 0xB4.

Harness (`tests/harness.c`): simulated manager (`simulate_user`), simulated user matches and promotion
(`h_sim_user_match`), season books print, `--eligibility` report. Latest 12-season run (seed 12345): 0 violations;
promotion from Academy in season 4 at strength 72; Academy about 1,440 coins a season, Div3 about 1,900.

## Historical v6 checklist (superseded)

1. **Harness cleanup / open bug — DONE (2026-09-25):**
   - `nosign commit_fail 10` was a false lead: `g_commit_fail` in `native_bridge.c` was never reset at the top of
     `commit_transfer`, so once *any* commit failed at check 10 (`move_player`) earlier in a run, every later,
     unrelated negotiation failure kept reporting the stale code 10 even though `commit_transfer`/`move_player` was
     never called for it. Added `g_commit_fail = 0;` at the top of `commit_transfer` (native_bridge.c, just before the
     seller-index check) so the diagnostic reflects the current call only.
   - Re-diagnosed with proper (non-stale) instrumentation: across a full 8-season seed-12345 run, all 47 user-buy
     attempts that never close fail *before* `commit_transfer`/`move_player` is ever reached — round limit
     (`USER_BID_ROUND_LIMIT`, 6 rounds) reached, insult-strike limit reached, or the personal-terms `demand > room`
     reject in `begin_user_player_negotiation`, sometimes with an AI rival then signing the player
     (`NEG_RIVAL_SIGNED`). These are the intended negotiation-breakdown outcomes described in "User negotiation (v6)"
     above (round limit, 3 insult strikes, a rival taking the player), not a roster/`move_player` bug.
     `move_player` itself was verified correct in every case it *was* reached (checked `get_specific` state before and
     after `sign_player`/`SellPlayer`: seller loses the player, buyer gains it).
   - All temporary investigation instrumentation (added to `native_bridge.c` and `tests/harness.c`) was removed;
     only the one-line `g_commit_fail` reset remains.
   - Debug prints added earlier this session (`H_DEBUG_BIDS`, `H_DEBUG_SALES`, `pre role`, `nobid`, `full`,
     `sell fail`, `nosign`) were already `getenv`-gated; confirmed, no change needed.
   - `--restart-every -1/1/5` give identical digests (`3b6b90dda49e0f53` / `04486416164c90f9` on
     `dls18.txt 8 12345`); `--no-user` passes with 0 violations (digest `0375a45322c18e5a` /
     `75652e5d05496f1d`). `run_tests.sh 12 1` (the canonical check): 0 violations, digest `bb7972dadcd3c6fa` /
     `06870286bf4b5177`.
   - Late-season coin build-up (e.g. seed 12345, 12 seasons: user coin balance climbs from ~1.5k to ~16k) is
     simulated-manager-only: `simulate_user()`'s "best upgrade per coin" buy logic only bids when it finds a
     +2-rating starter upgrade it can afford and successfully negotiate, capped at `USER_BUY_LIMIT_PER_WINDOW` (5)
     per window. As the simulated squad improves, qualifying upgrade targets get scarcer, so per-season transfer
     spend (`purchases`) falls while match/season income keeps flowing at a roughly constant rate — cash accumulates.
     The underlying income/expense formulas are unaffected; a real player would simply spend more (bigger squad,
     luxury signings, cosmetics). No code change needed.
2. **Hooks in `build_mod.py` — (a) and (b) DONE (2026-09-25), (c) still open:**
   - **CAVE was full before this started** (only ~12 bytes free of 1,676; confirmed by rebuilding and measuring, not
     guessed). The two new hooks (~80 bytes of cave code, plus dlsym symbol strings) don't fit. Rather than trim
     scope, found a **second cave**: `DEBUGCHARACTER_RenderPlayerData` + `DEBUGCHARACTER_RenderPlayerPitch`
     (`build_mod.py CAVE2 = 0x381368`, 1,112 contiguous bytes: 572 + 540, verified back-to-back with no gap) are two
     adjacent, free-standing (non-virtual) functions with **zero** direct `bl`/`blx` callers (`analysis/xref.py`),
     zero raw-address hits anywhere in the file outside their own body (checked with a whole-file byte scan,
     excluding `.dynsym`/`.dynstr` which trivially contain every symbol's own address), and zero `.rel.dyn`
     relocations into their range — the same bar `FTTCollectionsTest` (CAVE) itself was held to. `CAVE2` has its own
     `EXPECT` guard (first 8 bytes) and its own small rodata tail (`CAVE2_RODATA`, currently 54 bytes: the
     `career_market_on_match_awards` and shared `career_market_on_season` dlsym names). Only 134 of 1,112 bytes are
     used — there's room for several more future hooks without repeating this search. `CAVE` itself is back to
     untouched-v5 size (1,568 code / 1,676) plus 6 bytes for the dispatch fix below (1,574 / 1,676 — ~6 bytes free;
     don't add more to `cave_market_dispatch` or the other `cave_market_*` routines without freeing space first).
   - **Found and fixed a real bug in `cave_market_dispatch`** (build_mod.py, shared by every market hook) while
     wiring hook (a): it computed the wrapped C function's return value in r0, then **unconditionally overwrote r0**
     twice more (`mov r0, r7` for the dlclose call, then dlclose's own return value) before returning — so its
     caller never saw the wrapped function's actual result. Harmless for the pre-existing hooks (none of them read
     dispatch's return value), but it would have silently broken hook (a)'s fallback logic: `cave_econ_match`'s
     `cmp r0, #0` would always have seen dlclose's return (0 on success), always taken the "not handled" branch, and
     always tail-called the stock `SetMatchCredits` with the stock total — i.e. hook (a) would have had **no effect
     at all**, silently. Fixed by saving `blx ip`'s result in r4 (free after the dlsym call) and restoring it into
     r0 after dlclose. Caught by tracing the emulation in `test_hooks.py`, not by inspection — the Unicorn step
     really is mandatory, per PATCHING_RULES rule 7.
   - (a) Per-match income at **0x23a274** (`bl cave_econ_match` over `blx SetMatchCredits`; cave2). Gates on career
     (`ms_tInfo+0xfb0 == -1`), `MC_bInPostMatchCallback == 1`, and **not forfeited** (`tGame+0x9ebc == 0`) — on any
     failure it returns 0 and the cave tail-calls the stock `SetMatchCredits` unchanged, so forfeit/DLO matches keep
     exactly the stock reduced-row behaviour instead of reimplementing it. On success: reads division
     (`GetUserLeagueInTree`), side/goals/home/neutral (direct `ms_tInfo`/`tGame` field reads, ECONOMY_HOOKS section
     2), and league-vs-cup-vs-friendly from the match's tournament tid (`GetSpecificTournament` by turn slot →
     `CTournament::GetID` → `MCU_IsTournamentLeague`; tid 11 = friendly, matching the stock tournament-award row
     logic in ECONOMY_HOOKS 1.2) — then calls `user_match_payout(base, &facts, 0)` (new `credit` parameter: 0 here
     so `SetMatchCredits` does the crediting and the post-match screen's coin counter still animates; the harness's
     `h_sim_user_match` passes 1, unchanged), rewrites `ms_tCreditAwardInfo` from scratch with the mod's own rows
     (`write_credit_row`; at most 8 category rows + Total = 9, under the 9-row display cap) via
     `CREDIT_AWARD_ROWS`/`CREDIT_AWARD_COUNT`, and calls the real `SetMatchCredits(profile, total + pending)`.
     **Simplified for v1** (documented, not verified against a device): capacity is passed as 0, so `econ_gate` uses
     its division-reference fallback rather than the real stadium capacity/attendance; **existing stock reward rows
     (achievements, tournament awards) are dropped, not merged** — a future pass could read them before overwriting
     and append the mod's income rows after, respecting the same 9-row cap. The video doubler
     (`CConfig::GetMaxDoubler` 0x201678) was **not** patched off — still open, per ECONOMY_HOOKS 1.5 option (a).
   - (b) Season end at **0x29900a** (`bl cave_econ_season` over `blx CSeason::NextSeason`; cave2). One dlsym symbol,
     `career_market_on_season`, shared by the pre- and post-rollover calls (saves a whole rodata string in the tight
     cave budget) — `native_bridge.c`'s `career_market_on_season(void *season, int32_t block, uint32_t base)`
     discriminates on `season != NULL` (pre; a real pointer, `profile+0x14`) vs `== NULL` (post; the cave passes 0).
     Pre records `GetUserLeagueInTree` + `CTournament::GetTeamLeaguePos` (main league, `season+0x6ac`) before the
     rollover; the cave then calls the real `CSeason::NextSeason` directly (`bl 0x36aa08`, in range — Keystone
     initially mis-encoded this by +4 bytes, caught by the branch-target guard and fixed by re-using `branch_to()`
     for every far cave2 branch, not just the b.w ones); post re-reads the (now rolled-over) division to derive
     promoted/relegated, calls `user_season_end_payout`, and shows a plain option-box summary (`ui_show_options`,
     UI_AFTER_CLOSE) — **not runtime-tested that a box queued from here (outside the mod's own UI flow, deep in
     `CFlow::Process`) actually displays correctly; ECONOMY_HOOKS 3.2 flags this as unverified too.**
   - `SAVE_VERSION_SETUP/BOOT` raised to **0xB4** in build_mod.py, matching `MARKET_EXT_SAVE_VERSION` (already 0xB4
     in native_bridge.c from the v6 save-format work).
   - Verified: `python mod/build_mod.py --out ...` — all guards pass (byte-identity, keystone statement counts,
     both caves' size/alignment, every branch target, no absolute jumps). `mod/career_market/tests/test_hooks.py`
     extended with 3 new cases (match handled, match fallback, season pre→NextSeason→post) and rerun against the
     v6 build: **ALL OK**, including the 6 pre-existing cases (no regression). Host harness
     (`run_tests.sh 12 1`): unchanged digest, 0 violations (the `user_match_payout` signature change — added
     `credit` — is a no-op for the harness's own call).
   - (c) Still open: market prices on every player card at **0x237692** (UI_COMPONENTS 4.1). This must be a cheap
     path, not dlopen per frame — needs its own small published price table, not the dispatch mechanism.
3. **Native UI** (UI_COMPONENTS.md recipes 1–5; replace the option-list flows in the `ui_show_*` functions).
   **Not started except the OK/Cancel fix below.** This is the biggest remaining piece — several recipes, each
   touching real native game UI classes (message-box vtables, settings tables, player cards) with byte-offset
   precision and no way to test rendering without the tablet.
   - **OK/Cancel inversion — FIXED (2026-09-25):** confirmed the suspected bug (UI_COMPONENTS.md 0.2:
     `CFEMsgKeyboard` flags 3 -> button order Cancel = index 0, OK = index 1) against all three keyboard callbacks
     that had it backwards — `ui_callback_exact_amount`, `ui_callback_search`, `ui_callback_offer_counter_amount`
     all treated `selection == 0` as confirm and `!= 0` as cancel; now flipped (`== 0` cancels, `!= 0` confirms).
     Host-harness regression rerun: unchanged digest, 0 violations (these are UI-only paths the harness doesn't
     exercise, so this only confirms the file still builds clean, not the fix itself — needs a device to verify).
   - Still open — negotiation dialog: a `CFEMsgSignPlayer` / `CFEMsgSellPlayer` card box with the price on the coin
     button (`box+0x47c`, and 0x7601dc for SignPlayer), plus fee/wage/years **steppers** (settings table 0x25b9b4 +
     stepper rows 0x2581d0) instead of keyboard entry, replacing the current multi-screen option-list flow in
     `ui_show_bid_terms` (main menu -> amount submenu -> contract submenu).
   - Explain each `g_neg_event` in the description, e.g. "Chelsea bid 850. Beat it with 895."
   - **Player card grid — implemented in v15 and expanded in v16:** native `CFEPlayerCard` children inside the full-screen
     market, with a selectable-row fallback. The card rendering/touch path still needs a device check. Club finances/books
     as a custom-drawn box (recipe 5) remain open.
   - Coin icon: character 0x0180 in text.
   - New messages to wire: −16 (talks broken off), −17 (club can't sell, squad too thin), quick sale, sell-locked
     signings, season books screen (`g_ext.econ.this_season/last_season`, categories in `enum EconCategory`), and a
     transfer news feed (history plus `g_ext.history[].rival_team`).
4. **Verify and ship:**
   - `wsl -e sh mod/career_market/tests/run_tests.sh 12 1` (0 violations, identical digests with restarts)
   - `wsl -e sh mod/career_market/build_native_bridge.sh <F: path>/libCareerMarket_v6.so`
   - `python mod/career_market/tests/arm_crosscheck.py <so> mod/career_market/tests/small.txt 2`: must equal the
     harness `--no-user --restart-every -1` api_digest on small.txt. The crosscheck's Python mocks may need the v6
     changes: ext block serialization, market index.
   - `python mod/build_mod.py --out mod/build/lib/armeabi-v7a/libDLS18_market_v6.so` (guards must pass)
   - `python mod/career_market/tests/test_hooks.py <lib>`: all hooks OK, including the new ones.
   - Then `python mod/build_apk.py --base-apk mod/build/DLS18_career_market_v5.apk --lib ... --career-lib ... --out mod/build/DLS18_career_market_v6.apk`.
     Set `TMP`/`TEMP` to `F:\android modding\tmp`.
   - Update BALANCE.md (new section for v6), README.md, and the memory note. Install only if the user asks
     (`platform-tools/adb.exe -s e93a82ce install -r`; use `MSYS_NO_PATHCONV=1` in Git Bash).

## Build/test notes
- Harness compile: `gcc -O1 -g -std=gnu11 -w -no-pie -fno-pie -I.. harness.c -o harness -lm` in WSL
  (`MSYS_NO_PATHCONV=1 wsl -e sh -c 'cd "/mnt/f/android modding/DLS18_project/mod/career_market/tests" && ...'`).
- The harness includes `native_bridge.c` directly, so static functions are callable from it.
- Freestanding ARM build: no libc. Use `mul_div`/`clamp_add`, and don't use 64-bit division without `__aeabi_ldivmod`
  (`-lgcc` is linked).
