# DLS18 Mod Status Report — 2026-09-27

## Installed on the tablet
- **`build/DLS18_career_market_v30.apk`**
  - Installed with `adb install -r`, so the save is kept.
  - Game library: `F:/android modding/tmp/v27_lib.so`.
  - Tests pass and the ARM crosscheck digest matches.
- The game launches, and Transfers opens the market box without Safe Mode or a crash.
- **Still glitchy:**
  - A second row of buttons (Make Offer / Continue / Confirm Sale) hangs below the player box.
  - Boxes appear stacked.
- Do **not** install `DLS18_skill_tiered_dribble_v9.apk`: it crashes at pc 0x2e0f78.

## Local DLS26 animation prototype
- `build/DLS18_career_market_v30_dls26_contact_proto_v5.apk` is built and signed locally; it has **not** been installed or match-tested.
- It keeps v30's game and market libraries. Only the animation package changes: standing tackle 331, short slide 737, and long slide 1286 use DLS26 13.430 motions resampled to DLS18 contact timing.
- The source clips and decoded format, rig evidence, previews, candidate screening, and current limits are in `analysis/DLS26_13430_animation_port.md`. The directional standing slots 329 and 333 remain original because the DLS26 alternatives do not yet have a reliable left/right match.
- The current experiment is `build/DLS18_career_market_v30_dls26_tackle_stumble_append_v15_turns.apk`. It keeps the tackle, recovery, deek, and close-control additions through ID 2560, replaces hard-coded sprint loops 512, 514, and 516 with DLS26 ET0141/142/143 payloads, replaces existing category-3 sidestep slots 686, 693, 695, 698, and 702 with DLS26 ET0502/0507/0508/0510/0512 payloads, and replaces category-5 standing-turn slots 13, 15, 17, 19, 1253, and 1255 with exact-name DLS26 turn payloads. The animation database and native library remain at 2561. This package is signed and locally decoded, but has **not** been installed or match-tested; selection and transitions remain unverified. v14, v13, and the original SATs are rollback sources.

## Market UI fix history
| Build | Fix |
|---|---|
| v28 | Safe Mode on Transfers. The request flag was cleared before `CFEScreenStack::NewScreen`, which runs on a later frame, had built the screen. |
| v29 | Stack overflow. The market screen's Process called ProcessAll, which called Process again, recursively. |
| v30 | The full-screen market UI drew over the Safe Mode frame and took no input. It is disabled (`MARKET_USE_FULLSCREEN 0`) and the game's own option boxes are back. |

## Market UI rework (stopped partway, at your request)
**Suspected causes of the current glitches (not yet confirmed):**
1. **Shared memory:** every box shares the global `g_ui_options`, `g_ui_title` and `g_ui_description`. The stock box keeps pointers to them, so opening a new box rewrites the text of any box still open.
2. **Too many buttons:** pages ask for more than 3 buttons, and the stock box fits 3 per row, so the extras spill outside it.
3. **Timing:** callbacks open the next box while the current one is still alive, which stacks them.

**Plan for v31:**
- One live box at a time; the next page opens only after the current one closes.
- A 2-slot ring of text buffers, so a closing box is never overwritten.
- At most 3 options per box, with "More..." paging and always a Back button.
- No changes to the market logic.

**State of the stopped work:**
- The agent had started editing `career_market/native_bridge.c` (modified 14:32; new code in place) but had **not** compiled or tested it.
- No v31 was built.
- Before building again, either finish and test these edits or revert them.
- v30 on the tablet does not include them.

## Modding limits (the /goal)
| Area | Before | Now | State |
|---|---|---|---|
| Mod code space | Small code caves | +256 KB RX (CAVE3) and +64 KB RW (MODDATA) via `elf_extend.py` | Loads on the device |
| Runtime hooks | Fixed patches | modcore: 64 hook slots, `modcore_register`/`modcore_init` | Emulation test ALL OK |
| Animation clip cache | 2535 | 3840 (maximum 3854), via `anim_caps.py` | Built; not checked in a match |
| Animation unlocks | — | Clips 69/70 re-enabled; 31 extra celebrations added to `animlist.xml` | Built; not checked in a match |
| Player development records | 64 | 255 | Built; not checked in a match |
| Players, teams, text | — | Sized from the data files; no fixed cap found | Research only (`data_limits/DATA_LIMITS.md`) |
| Custom image size | 512 | Build option `--custom-image-max` | Not tested |
| Squad size | 32 | 32 | **Waiting on you.** Raising it breaks the save format. |
| Created players | 32 | 32 | **Waiting on you.** Raising it breaks the save format. |

## Earlier finished work
- **Money model (v7 onward):** price schedule, books, routing of spending and income, no coin shop, no ad coins. See `career_market/MONEY.md`.
- **Stat rework:** report and a prompt for a smaller model in `STAT_REWORK.md`.
- **Research docs:**
  - `data_limits/DATA_LIMITS.md`
  - `anim_limits/ANIMATION_LIMITS.md`
  - `career_market/data/COIN_FLOWS.md`
  - `UI_COMPONENTS.md`
  - `ECONOMY_HOOKS.md`

## Waiting on you
1. Resume the market UI rework (v31), or revert the partial edits?
2. Yes or no on raising the 32-player squad and created-player caps (breaks the save)?

## Update: v31 market UI rework (built, not installed)
- `build/DLS18_career_market_v31.apk`, SHA-256 A31427DF...CD27B5.
- **Tests:** violations=0, a new UI box test, the crosscheck digest b0d31a38190d3683, test_hooks ALL OK.
- **Real cause of the glitch:** the player page passed 6 as the button count but gave only 2 labels, so the box read garbage labels from nearby memory.
- **New UI layer:**
  - One box at a time; the next page opens after the old box closes, driven by a copy of the queue's vtable.
  - At most 3 buttons per box, with "More..." paging.
  - Back buttons and player navigation on every page.
- **Not yet verified on the device:** the vtable swap and the new layout. The install is waiting for the tablet to reconnect.

## Update: more limits raised (libDLS18_limits_v32.so, no APK built)
- **Player of the Week roster:** 16 -> 64 (`--potw-max`, up to 255).
  - The roster is moved into mod memory at MODDATA+0x8000.
  - The message box still shows 16 at most.
  - The total weight in the POTW config must stay at 255 or below.
- **Minimum squad:** new `--min-squad 11..31` flag; the default stays at 16.
- **Declined:**
  - Boots, skin and gloves textures: the game picks them with hard-coded random ranges, and the extra texture files don't exist.
  - Post-match reward rows: several screens have fixed sizes, and 12 rows are never reached anyway.
- **Tests:** test_hooks ALL OK, test_patch ALL PASS, new Unicorn limits test 23/23.
- **Code:** `limits_caps.py`; `build_mod.py` gained the two flags; DATA_LIMITS.md updated.
- **Squad size and created players:** stay at 32. The research recommends against raising them: it changes the save format, and created-player ids would collide with real ones.

## Update: combined v32 APK (built, not installed)
- `build/DLS18_career_market_v32.apk` (sha256 5462dc30...): the v31 market UI rework plus libDLS18_limits_v32.so.
- The game library differs from the installed v27 library only at the POTW patch sites.
- The library hashes inside the APK are checked.
- Install with `adb install -r` when the tablet is connected.

## Update: v33, squad 64 and created players 255 (built, not installed)
- `build/DLS18_career_market_v33.apk` (sha256 b502c75b5ebd3536...). Game library `build/libDLS18_v33.so` (926f0355af655c3e); market `build/lib/armeabi-v7a/libCareerMarket_v33.so` (9f327e6d2b11dabe). Both hashes were checked inside the APK.
- **User squad: 32 -> 64** (`squad_caps.py`, `squad/squad64.c`, `build_mod.py --squad-max 32..64`).
  - The game still sees a normal 32-player squad; players 33-64 live in overflow tables in MODDATA, kept in lineup order.
  - AI squads stay at 32.
  - **Save version is now 0xB6.** Old saves load fine; a v33 save won't load in older builds.
  - Players 33-64 don't show on the pre-match / pause team screens, stats tables or transfer search. To field one, move him into the first 32 on the squad screen.
- **Created players: 32 -> 255** (`created_caps.py`, `--created-max`). Created ids moved to 0xFEDF..0xFFDD.
- **Market:** the user cap is now 62 (was 30) when the squad hook is present, per-position caps scale with it, and players 33-64 are listed for selling.
- **Tests:** squad emulation 189/189, static checks, created-player test, limits test, test_patch, and test_hooks all pass. Market tests show violations=0, and the ARM crosscheck digest matches (fdd0bde8e0d97325).
- **Installed on the tablet 2026-09-26** with `adb install -r` (the save is kept). Installed library hashes match (926f0355..., 9f327e6d...). Not yet play-tested.

## Update: v34, fair AI (built, not installed)
- `build/DLS18_career_market_v34.apk` (sha256 ec1e060024e2fbf2...). Game library `build/libDLS18_v34.so` (44f326738747668c) = v33 plus 11 bytes at five sites (`ai_fair.py`). Market library unchanged (v33).
- **Penalties: the CPU keeper no longer reads your shot.**
  - Stock: at the moment you strike, the keeper copied your real aim 37.5% of the time. Otherwise it was forced to dive away from the ball. Every CPU penalty save came from that read, wherever you placed the shot.
  - Now the keeper guesses at random, before seeing the shot. A guess within reach saves: about 25% of corner shots and 39% of central ones in the test.
  - The CPU taker and your own keeper were already fair (no read), so they are unchanged.
- **No in-match difficulty spikes.**
  - Stock: at kick-off and again at half-time, a CPU trailing by 2+ goals got +12 difficulty, and -12 when leading. Margins beyond 5 goals added 8 per goal.
  - Now the CPU plays the whole match at the base difficulty.
- **Smaller jumps between matches.** The post-match dynamic difficulty step is 2 per goal of margin (stock 5; `--dd-step`). One big win no longer makes the next opponents much harder.
- **No hidden "cheater" difficulty bonus.** The game added a `Cheat_Difficulty` bonus when its server cheat rules flagged the profile. A modded economy can trip those rules. The bonus is removed.
- `--stock-ai` turns all of this off.
- **Tests:** a new Unicorn test (`F:/android modding/tmp/ai_fair_v34/test_ai_fair.py`) runs the real functions: the stock baseline is reproduced and the fair behaviour is confirmed. The squad, static, created-player, limits, test_patch and test_hooks suites all pass.
- **Installed on the tablet 2026-09-26** with `adb install -r` (save kept). Installed library hashes match (44f32673..., 9f327e6d...). Not yet play-tested.

## Update: v35, market inside the stock transfer screen (built, not installed)
- `build/DLS18_career_market_v35.apk` (sha256 b87551b937d6be37...). Game library `build/libDLS18_v35.so` (6e8f88f3307e610c) = v34 plus 7 runtime hooks. Market library `build/lib/armeabi-v7a/libCareerMarket_v35.so` (bd4df19394bf03a2).
- **No box when you open Transfers.** The stock screen and its player cards are the market:
  - **Card prices:** every buy card shows the market asking price instead of the stock value.
  - **Tapping a card** selects the player. The top-right band shows his rating, fee, wage, and whether he is interested.
  - **Tapping the selected card again**, or pressing Make Offer, starts the negotiation. That is the only box.
- **New buttons in the empty space:**
  - **Top right** (above the position tabs): a status line (window, squad size out of 62, wage room), then [Make Offer] [Shortlist +/-] [Sort].
    - Make Offer becomes View Offer when an offer for the player is active, and Sell / List for your own player.
    - Sort cycles Default / Rating / Price / Wage and also sorts the stock search results.
  - **Bottom** (between Free Scout and Sell Player): list tabs [Search] [Shortlist (n)] [Offers (n)] [For You] [Club].
    - Shortlist, Offers and For You fill the stock card grid with market players.
    - For You means players who would sign now at a fee and wage you can pay, strongest first.
    - Club opens the records (inbox, sales, finances, history).
- **How it works:**
  - The buttons are real game buttons: children of the screen, and of the footer for the bottom tabs, placed from the measured positions of the card scroller and the Free Scout / Sell Player buttons.
  - The market lists are fed to the stock card grid only while its SetupResults runs, and the stock search results are restored straight after.
  - Hooks: Init, Process, RenderPost, SetupResults and CurrentPlayerBid of the transfer screen, plus CFEPlayerCard::GetPlayerValue and CFEFooterMenu::RenderPost.
  - `MARKET_TRANSFER_SCREEN_UI 0` in native_bridge.c brings back the old hub box.
- **Tests:**
  - Host harness: new transfer-screen test (lists, sorting, the SetupResults swap and restore, card selection); violations=0, digests unchanged.
  - The ARM crosscheck digest matches (fdd0bde8e0d97325).
  - test_hooks now checks all 8 runtime hooks: ALL OK.
  - test_patch, squad, static, created-player, limits and fair-AI suites all pass.
- **Not verified on the device:** button positions, label sizes, and whether the bottom tabs draw above the footer background.

## Installed 2026-09-27: v35 + DLS26 animations (v15_turns)
- `build/DLS18_career_market_v35_dls26_anim_v15.apk`, installed with `adb install -r` (the save is kept). Installed library hashes match: libDLS18 129c43446569454c, libCareerMarket bd4df19394bf03a2.
- Contents:
  - v35 game library rebuilt with `build_mod.py --anim-count 2561` (`build/libDLS18_v35_anim.so`). It reproduces all 9 animation patches of the other agent's v15_turns library byte for byte, and differs from v35 only there.
  - The v35 market library.
  - The other agent's `anims_career_market_v30_dls26_tackle_stumble_append_v15_turns.pak`.
- **Tests on the combined library:** test_patch, test_hooks (8 runtime hooks), squad, fair-AI and limits all pass.
- **Not match-tested:** the new transfer screen layout and the animation selection and transitions.
- The first two install attempts were cancelled at the tablet's on-screen USB install prompt.

## Installed 2026-09-27: v35b (crash fix + transfers through the game's own dialogs)
- `build/DLS18_career_market_v35b_dls26_anim_v15.apk`, installed. libDLS18 900847e9... (`build/libDLS18_v35a_anim.so`, `--anim-count 2561` + anim_guard); libCareerMarket 188f3d44... (`libCareerMarket_v35b.so`); DLS26 anims v15_turns.
- **Crash fix (anim_guard.py):**
  - The device crashed with SIGFPE (divide by zero) in CPlayer::SetAnimControl, reached from ControlTakeBall / UpdateTake. An appended clip (r10 = 2553) gives 0 ticks until its action point.
  - All five integer divisions in SetAnimControl now go through a CAVE3 stub `safe_idiv` (divisor 0 becomes 1, then __aeabi_idiv). The stub is emulation-tested.
  - Stock clips never divide by 0, so nothing changes for them. The clip data itself is being tuned separately.
- **Transfers (v35b):**
  - Tapping a card opens the game's own Sign Player dialog: the player card plus a coin button with the market asking fee. The coin makes the offer.
  - A seller counter or a player wage counter comes back in the same dialog with the new amount. The cross closes it and decides nothing; the offer stays in the Offers tab.
  - The stock Sell Player button -> tapping a player opens the game's own Sell Player dialog with today's best club bid; the coin sells. With no bid, the player is listed.
  - The Search tab is removed; the stock filter and name search are the search. Using them switches the grid back to search results, and tapping the active tab again does too.
  - Tabs are now [Shortlist] [Offers] [For You] [Club]. The top button is now Negotiate (full terms, counters, exact amounts), View Offer, or Sale Options.
- **Tests:** harness violations=0, digests unchanged; new checks for the Sign dialog at the asking fee and a cross that makes no offer. The ARM crosscheck digest matches; test_hooks and all the lib suites pass.
- **Open:** animation tuning (ball placement, tackle contact, root travel) is being done by a background agent.

## Installed 2026-09-27: v35b + tuned DLS26 animations (v16)
- `build/DLS18_career_market_v35b_dls26_anim_v16.apk`, installed (libDLS18 900847e9..., libCareerMarket 188f3d44...). Anims: `anims_career_market_v30_dls26_tackle_stumble_append_v16_tuned.pak`.
- **What v16 fixes:**
  - Every ported clip now has correct ball-contact ticks and points. The appended deeks and dribbles had none; that caused the crash, and their touches never moved the ball.
  - Each clip uses its own root path and heading curves; before, clips copied a template's curve and players slid off their line.
  - Tackles 331, 737 and 1286 keep the stock contact point and total travel.
  - The sprint, sidestep and turn loops match the stock distance per cycle.
- Full tables are in `analysis/DLS26_13430_animation_port.md` (v16 tuning) and `analysis/dls26_tuned_v16_validation.md`.
- **Check on the device:**
  - Tackle 331 approach glide.
  - The heading turn after contact on slide 737.
  - Entry crossfades.
  - The 13% longer stride on sprint 514.

## v35c: nothing over the search bar (built, not installed)
- `build/DLS18_career_market_v35c_dls26_anim_v16.apk`: the same game library and v16 anims as the installed build, market library `libCareerMarket_v35c.so`.
- **The band above the position tabs was not free space.** It is the stock name-search field (CFETransferOptionsMenu Init 0x2486bc: a CFETextField at the top of the filter panel). Negotiate, Sort and the status line drew over it.
- **Now nothing is added to the screen area.** All market buttons sit in the footer gap between Free Scout and Sell Player, in two rows:
  - [Shortlist (n)] [Offers (n)] [For You] [Club]
  - [+/- Surname of the last tapped player] [Negotiate] [Sort]
- The status line was removed; the Sign / Sell dialogs show the terms.
- **Tests:** harness violations=0, digests unchanged; the ARM crosscheck digest matches.

## Installed 2026-09-27: v35d (card-tap crash fix)
- `build/DLS18_career_market_v35d_dls26_anim_v16.apk`, installed and hash verified. libCareerMarket 349580da... (`libCareerMarket_v35d.so`); libDLS18 is still `libDLS18_v35a_anim.so`; DLS26 anims v16.
- **Crash:** every card tap in the transfer screen crashed with SIGSEGV at fault address 0x295 in libCareerMarket (ts_bid_hook).
  - `CFESDreamLeagueTransfers::CurrentPlayerBid(CFEPlayerCard *)` is static, so the card is in r0. The hook read r1 (= 1) and loaded card+0x294.
  - Fixed: the hook reads r0, ignores pointers below 0x10000, and leaves created players (id >= 0xFEDF) to the stock create-player path.
  - The harness now puts the card in r0 and 1 in r1, which is what the device had.
- **Tests:** run_tests violations=0, digests 5e8a0227b26971cc / d882fa7c258e4f7e unchanged; ARM crosscheck api_digest fdd0bde8e0d97325 matches the harness.
- **In progress:**
  - Continuous (no hard cutoff) rework of buying and pricing (libCareerMarket v36).
  - Continuous anim_fit rules, plus anim_motion integrated in build_mod.py (libDLS18 v36).

## Installed 2026-09-27: v36b (continuous stats everywhere + animation fit)
- `build/DLS18_career_market_v36b_dls26_anim_v16.apk`, installed and hashes verified.
  - libDLS18 a04b5f69... (`build/libDLS18_v36.so`, `--anim-count 2561`)
  - libCareerMarket 6b96fba6... (`libCareerMarket_v36.so`)
  - anims v16
- **libDLS18 v36:**
  - anim_guard.
  - anim_fit (every rule continuous, see anim_fit.py docstring):
    - control / aerial / stock-pool linear biases
    - deek keep probabilities (ramps 25..99 and 5..80)
    - a new style_fit hook (continuous deek style pick, replaces SM_DRIBBLE_*)
    - lunge ramp 30..95
    - stumble bias over strength 20..99
  - anim_motion (minimum crossfade for clips 2535 and up, contact-capped).
  - Crossfade, sprint stamina and GK reaction lines extended down to stat 1 (no flat low end).
- **libCareerMarket v36:** buy logic with no hard cutoffs.
  - The market index is interpolated between band centres.
  - Role is a weight mix (surplus/rotation/starter/key ramps) for:
    - reservation (85/100/120/150)
    - markup (100/108/112/120)
    - renewal wage and user premium
  - Contract factor slides with the time left, including the season in progress: 45/65/80/92/100 at 0/1/2/3/4 seasons.
  - Replaceability ramps from 95% at a 2-point gap to 110% at 6. Cash discount ramps from 85% at half the reserve to 100%.
  - Wage demand:
    - +0.5% per reputation point down, to -5% at a step up of 20
    - +15% for a bench player (improvement -4) to -5% for a starter (+3)
  - Will-join and starter-move checks: refusal probability rises smoothly with the gaps (deterministic draw per player, club and window) instead of walls.
  - Negotiation:
    - The accept point is hidden between the reservation/ask midpoint and the ask.
    - Counters are smooth.
    - A bid under the reservation costs patience in proportion to the shortfall (1 strike at 80%, 3 strikes end talks).
  - Balance, 8 seasons: fee/value 105-109%, below_pos_min 0 from season 4 (was 1-4), AI transfers about 106-114 a season (was 68-77); user 37 buys / 31 sales.
  - Tests: harness continuity sweep (h_test_continuous), violations=0, digests cdf14dcf412253c2 / 4bb75895db8a2d41; ARM crosscheck 7029bc38bb23f947 matches.
- **Still stepwise, in the stock game code:**
  - deek success roll 0x2E41C4 (0% below control 30/40)
  - kick flags at control 80/90 (0x2E5F98)
  - slide tackle random(control-50) (0x2DB5A8)
  - free-kick shooting 81/84 gates
  - flat low ends in ControlFinish and PM_FreeKickCPUAITake

## Installed 2026-09-27: v36c (continuous deek success and kick flags)
- `build/DLS18_career_market_v36c_dls26_anim_v16.apk`, installed and hashes verified: libDLS18 962099ef... (`libDLS18_v36c.so`), libCareerMarket 6b96fba6... (v36).
- **roll_fit (0x2E41C2, CAVE3 0x96b540):** the deek success roll XSYS_Random(50) < threshold.
  - style 0 ramps over control 5..85 (floor 6%)
  - style 1 ramps over control 15..95 (floor 4%)
  - style 2 stays 100%
  - Success % (style 1 / style 0) at control 20/40/60/75/85/99: 6/18, 30/42, 56/68, 74/86, 86/100, 100/100. Stock was 0% below 30/40.
- **kick_fit (0x2E5F98, CAVE3 0x96b940):** kick-selection flags.
  - Flag 4: continuous chance over control 45..92.
  - Flag 0x200 (with 4): up to 35% over control 60..99.
  - Both are redrawn in the same 8- and 32-frame blocks as stock, reading the frame counter PC-relatively through GOT 0x73063C.
  - % of frames (4, 0x200) at 60/75/85/99: (31,0), (63,9), (86,19), (100,35). Stock: 0 below 75, 50% at 75-79, 100% from 80, 0x200 25% from 90.
- **Not changed:**
  - UpdateActionSlideTackleX: P = 1 - 21/(control-50) is already continuous (flat up to 71).
- **Still stepwise, semantics unknown:**
  - free-kick gates GL_FreeKickProcess 0x2BB91A (shooting 81) and LOG_SetPiecePositionPlayerForKick 0x2C73FE (shooting 84)
  - ControlFinish / PM_FreeKickCPUAITake flat low ends
- **Tests:** test_patch ALL PASS (emulated at every control 0..99, plus continuity sweep); test_hooks ALL OK.

## Installed 2026-09-27: v37 (Transfer Market v2 replaces the stock transfer screen)
- `build/DLS18_career_market_v37_dls26_anim_v16.apk`: libDLS18 `libDLS18_v36c.so` (unchanged), libCareerMarket d0b7b3e0... (`libCareerMarket_v37.so`), anims v16. Installed and hashes verified (installed after the user asked).
- **Design:** the approved HTML prototype (artifact "Transfer Market v2", 1024x640 tablet layout, turf palette), rebuilt in the game in `mod/career_market/tm_screen.c`.
- **How it replaces the stock screen:**
  - career_market_new_screen_hook (CFEScreenStack::NewScreen 0x23BC14) builds our own CFEScreen when the game asks for screen 0x19 (CFESDreamLeagueTransfers).
  - The screen uses a cloned vtable (init / exit / process / render_post), keeps id 0x19 so the screen stack and Back work as before, and hides the stock header and footer (CFEScreen::DisplayFooter / DisplayHeader 0x23B74C / 0x23B75C).
  - `MARKET_TM2 0` brings back the v35 stock-screen market.
- **Drawing:** everything is drawn per frame:
  - FE2D_DrawRectCol
  - FESU_SetupText + FTTFont_PrintUnicode / FESU_DrawTextBold, with text measured by FTTFont_GetUnicodeTextWidth
  - the font height calibrated once with FTTFont_GetUnicodeTextDimensions
- **Layout:** in design units on a 640-high canvas, scaled to the screen height; extra width goes to the player list.
- **Touch:** XCTRL_Touch* on track 1 (the tracker CFEEntity::IsTouchInRect reads). Taps are hit-tested against rectangles registered while drawing; drags scroll the list and move the fee slider. Input pauses while a message box (search keyboard) is open.
- **Screen:**
  - top bar: Back, window and matches left, coins, wage room, squad n/max
  - rail: Scout / Shortlist / For You / Offers / My Squad, plus a market pulse by position
  - list: search (game keyboard), position chips, sort (rating / price / value); rows show a rating badge, name, club, asking price, value, trend, "In talks", and a shortlist toggle
  - detail: role (from the role weights), contract seasons left, wage demand, stats via PU_Get*Stat (keepers: shot stopping, handling, presence), chance he joins (the same curves the decisions draw against), rivals, asking price and value, Make offer / Continue talks, Shortlist
  - Offers tab: incoming bids with Accept / Ask +10% / Reject
  - My Squad tab: list or unlist a player
  - negotiation sheet: club reply log, fee slider (50-130% of asking), contract 1-5 years, wage demand, patience (3 bars from TalkMemory), rival bid, Accept their counter, Submit, Walk away, and a personal-terms stage (accept, or offer 92%)
- **Logic changes (native_bridge.c):**
  - join_refusal_pm / move_refusal_pm expose the refusal chances; player_will_join and starter_move_allowed draw against them.
  - Wage talks are continuous. He accepts from a hidden point between 88% and 100% of his demand (wage_accept_point). Under it he meets you halfway, and patience drains with the shortfall (one strike at 80%). This replaces the 92% / 80% / 90% cut-offs.
- **Tests:**
  - run_tests: violations=0, digests cdf14dcf412253c2 / 4bb75895db8a2d41 unchanged. The new h_test_tm2 checks 5 tabs x 3 sorts, the position filter, shown vs drawn join chance (49% vs 52%) and a bid in an open window.
  - ARM crosscheck 7029bc38bb23f947 matches.
  - New `tests/tm2_smoke.py` runs the real ARM lib in Unicorn at 1280x800, 1024x640 and 1422x800: builds screen 0x19 through the hook, taps every tab, filter and sort, scrolls, selects the cheapest player, drags the fee slider and submits. The emulated career signs him and the sheet shows "Signed for ...". About 4,500 rects and 7,500 texts per run, all coordinates finite, all text terminated. ALL OK.
- **Unverified on device:**
  - the font scale calibration and text baseline
  - touch coordinates matching FE units (as CFEEntity::IsTouchInRect assumes)
  - how the hidden footer and header behave on the next screen

## Installed 2026-09-27: v37b (Transfer Market v2 fixes from the tablet)
- `build/DLS18_career_market_v37b_dls26_anim_v16.apk`, installed and hash verified: libCareerMarket 0935da62... (`libCareerMarket_v37b.so`), libDLS18 v36c.
- **Fixes from the user's tablet screenshot:**
  - **Alignment:** every label was drawn centred on its left edge; the font kept the previous screen's centred alignment. tm_text now calls FTTFont_SetAlign(0) (0x38E300) and does its own centring and right-alignment from the measured width.
  - **Stock footer (Scout Players / Sell Player) and header (menu dots, coin sparkle) over the screen:** the screen stack re-enables them after Init, so DisplayFooter / DisplayHeader(false) now run every frame.
  - **Back:**
    - The top bar now has a real Back button ("Close" while negotiating).
    - Android back is handled: CFEEntityManager::ProcessPhysicalBackButton sets CFEHeaderMenu+0x310 = 1, and with the header hidden the screen consumes that itself (sheet first, then CFE::Back).
  - **Could not bid from the Shortlist:**
    - One bid check now serves both the market and the screen (user_bid_block), with specific reasons: window, not for sale, signings limit, club busy, 3 bids running, in talks, won't join, club keeps him, talks ended.
    - The user may now run 3 bids at once (was 1).
    - Rows show a status (In talks / Won't join / Not for sale ...). The detail panel explains a disabled "Can't bid" button.
  - **Input:**
    - The fee has -5% / +5% steps and "Type fee" (game keyboard, digits) next to the slider.
    - The search box has a Clear button.
    - "Save / Saved" pills replace the + glyph.
- **Polish:**
  - Switching tabs selects that tab's first player.
  - The My Squad and Offers toolbars show a summary (players, squad value, listed; bids for you, your bids of 3).
  - Squad rows show position, value and contract.
  - The sheet keeps the seller and ask from when talks opened, and shows a result summary (TRANSFER COMPLETE / TALKS ENDED) instead of the controls once talks are over.
- **Tests:**
  - run_tests violations=0 with the digests unchanged; ARM crosscheck matches.
  - tm2_smoke ALL OK at 2880x1800 / 1024x640 / 1422x800. It now also checks left alignment on every print, the Android back path, and a bid from the Shortlist that signs.
  - It writes PNG previews of the draw calls (`tm2_smoke.py ... <out dir>`) for layout review.

## Installed 2026-09-27: v37c (Back crash, stock footer input, search keyboard)
- `build/DLS18_career_market_v37c_dls26_anim_v16.apk`, installed and hash verified: libCareerMarket 1dac0e0b... (`libCareerMarket_v37c.so`), libDLS18 v36c.
- **Back crashed (SIGILL):** CFEScreenStack::DeleteTopScreen calls vtable slot 1 (deleting destructor), and the base CFEScreen slot 1 is a trap. Both our screens now have their own deleting destructor (exit, CFEScreen D1 0x23B5B3, operator delete 0x5C15C9).
- **Scout Players / Sell Player still took taps:** hiding the footer was not enough. The screen now disables the header and footer input (CFEEntity::EnableInput) and removes the footer buttons through the button mask (CFEFooterMenu::RemoveButton); both come back on exit.
- **Search box did not open the keyboard:** the screen now calls CFETextField::ShowKeyboard on the box's text field.
- tm2_smoke checks that the deleting destructor frees the screen, the footer input is off, and its buttons are removed and restored.

## Built 2026-09-27: v37d (what computer clubs pay for the user's players)
- `build/DLS18_career_market_v37d_dls26_anim_v16.apk`: libCareerMarket c682518e... (`libCareerMarket_v37d.so`), libDLS18 v36c. Not installed yet: the tablet was not connected.
- **Why:** computer clubs charged the user an importance premium for their key players (role x1.5 plus markup and a standing premium), but bid for the user's players at 90-100% of market value. A key player such as a 79 LM was valued at his 1,190 market value while other clubs' 79 LMs asked about 2,200.
- **user_sale_value:** AI clubs now value the user's players with the same importance weights an AI seller uses (role_blend {90, 100, 120, 150} for surplus / rotation / starter / key).
  - The part above market value shrinks smoothly with the user club's reputation, which has a floor set by the division.
  - The scale is 20% of the premium at rep 1, 33% at 20, 60% at 50, 90% at 80 and 100% at 100.
  - So a full key player fetches about +10% at the bottom of the pyramid, +30% mid-pyramid and +50% at the top.
- **Where it applies:**
  - AI bids for listed players start at 90% of the sale value and are capped at the sale value.
  - Unsolicited bids for starters start at the full sale value and are capped at 115%.
  - The cap on the buyer's valuation when the user counters (create_ai_offer_for_user) and the best current bid (best_ai_bid_for_user_player) use it too.
  - The buyer still has to value him that highly (buyer_max_price: need, lift to its best XI, budget), afford him and have him willing to join.
- **My Squad:**
  - Rows show "Clubs pay X" instead of the market value.
  - The detail panel shows CLUBS WOULD PAY next to MARKET VALUE, and "Best bid now: X from <club>" (or that nobody would bid right now).
- **Tests:**
  - run_tests violations=0. The digests changed as expected (ea85af2fcd25deab / 697e902c76876000).
  - The ARM crosscheck matches harness --no-user (7029bc38bb23f947).
  - tm2_smoke ALL OK.
  - test_hooks reports STRAY calls on v37c as well; that is a mock-coverage gap in the test from the v2 screen, not from this change.

## Installed 2026-09-27: v38 (Club Hub)
- `build/DLS18_career_market_v38_dls26_anim_v16.apk`, installed and hash verified: libCareerMarket ed6ffa85... (`libCareerMarket_v38.so`), libDLS18 v36c. v37d (c682518e) was installed before it.
- **Club Hub** (`mod/career_market/hub_screen.c`):
  - The club's own window, opened from a Club Hub button in the transfer market's top bar.
  - It is a second view of the transfer screen object, so Back (the Market button or Android back) returns to the market and never leaves through the screen stack.
  - Prototype: `design/club_hub/club_hub.html` (artifact https://claude.ai/artifact/1X1oFqcMrh3VEeD3wBSP3g).
  - **Finances:**
    - spendable coins after the wage reserve
    - the wage bill against the wage budget and the room left
    - the season net
    - income and spending by category, for this season or the last
  - **Board:**
    - A target finish, the expected position from squad strength: 1 + the sum over league rivals of a ramp over -6..+6 strength difference, so it is continuous.
    - League membership comes from CTournament::GetTeamLeaguePos 0x36202E on the league at CSeason+0x6AC.
    - Confidence runs from the live position against the target, 9% for each place.
    - What the division pays: match prizes, goal and clean-sheet bonuses, gate, the cup prize, TV and sponsor money, the league prize range and the prize at the current place.
    - What promotion is worth: the bonus, plus the next division's higher season money.
  - **Contracts:**
    - depth per position against the min, target and max
    - every player with role, contract left (with a bar), wage, renewal ask and what clubs would pay, contracts ending first
    - a detail panel to renew (wage slider, -5% / +5%, a typed wage, 1-5 years, his patience), list or keep him
- **Renewal talks** (`hub_renewal_offer`):
  - He signs at or above a hidden point between 88% and 100% of his demand, fixed per player and season.
  - Below it he counters two thirds of the way to his demand, never below that point.
  - Each short offer uses 0.4 strike plus 4 strikes per 100% short. At 3 strikes the talks are over until next season.
  - Renewal talks use their own talk slots and never overwrite a running transfer talk.
- **Keep:**
  - Up to 16 kept players, saved in the former `MarketExt.reserved` space, so saves stay compatible.
  - A kept player gets no unsolicited AI bids.
  - Keeping him takes him off the sale list, and listing him releases the keep.
- **Tests:**
  - A new harness test (h_test_hub) covers:
    - the board target and its continuity (one strength point never moves it a full place)
    - the contract rows and their order
    - a renewal at the full demand signs and extends the contract
    - lowballs get counters and use patience until talks end, and talks stay closed that season
    - accepting a counter signs
    - AI target searches skip kept players, and listing releases a keep
  - run_tests violations=0. The API digest is unchanged, and the ARM crosscheck matches (7029bc38bb23f947).
  - tm2_smoke ALL OK. It now also:
    - opens the Hub from the market button
    - visits every section and scrolls the contracts
    - uses the wage slider, +5% and the years buttons, and makes a renewal offer (answered on screen)
    - taps Keep and List
    - checks Android back and the Market button return to the market without leaving the screen
- **Save check on the tablet** (profile.dat read over adb, the game not launched):
  - In season 1 the log's last 128 transfers were 124 AI-to-AI and 4 signings by the user.
  - One AI bid for the user's 79-rated MID (player 13032) was waiting for the user in Offers. Club 26 opened at 1,126, under the v37c pricing, and its counter stood at 1,141.
