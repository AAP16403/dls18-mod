# Career money model (v7): every coin accounted for

Installed 2026-09-25 as `mod/build/DLS18_career_market_v7.apk` (save version 0xB5; v6 saves load).

Coins are the only currency. In a career, every coin the user club earns or spends goes through the mod.
Either the mod pays or charges it itself, or it sets the game's price and books the game's own charge.
The season books (Market → Club Finances → Season books) therefore reconcile with the coin balance.

## Income

| Source | Amount | Where it is paid |
|---|---|---|
| Match prize money | win 3.66% of the division's season target, draw 40% of a win, loss 12% | post-match screen |
| Gate receipts (home) | 2.2% of the target for a crowd of 60% of the division's reference stadium; scales with the real attendance, with diminishing returns beyond twice that | post-match screen |
| Goals / clean sheet | 8% / 15% of a win prize (up to 6 goals) | post-match screen |
| Cup matches | the same result prizes, booked as cup money | post-match screen |
| Cup winners' prize | 12% of the target (replaces the stock 50 coins) | post-match screen |
| Achievements | stock amounts, kept as their own rows | post-match screen |
| TV rights / sponsorship | 25% / 15% of the target | end-of-season box |
| League prize | 20% of the target for 1st, falling linearly to 2% for 16th | end-of-season box |
| Promotion bonus | 15% of the next division's target | end-of-season box |
| Season objectives | 100 coins each (was 25); match objectives 20/40/60 (was 5/10/15) | stock objective screens |
| Player sales | negotiated fee (market) | market |

Season targets by division: Elite 4,300 · Junior Elite 3,400 · Div 1 2,700 · Div 2 2,100 · Div 3 1,600 · Academy 1,200.
Reference stadiums for gate receipts: the capacity needed to be promoted into the division (80k / 60k / 45k /
30k / 15k); the Academy's is 6k.
Rewarded videos pay nothing.

## Costs

| Cost | Price | Config var |
|---|---|---|
| Wages | 12% of each player's value a season, taken every match | market |
| Medical treatment | 40 coins per week out injured (was 30) | 0x01A HealPlayerCost |
| Energy refill | up to 20 coins a player, less when fitter (was 50) | 0x01B EnergyMaxCost |
| Training session | 30–300 by the player's level (was 20–200) | 0x04A / 0x04B |
| Scouting | 60 coins for the first session, then 40 more for each extra one (was 50 then 100) | 0x177 / 0x178 |
| Scouted-player price surcharge | removed (was +20%); the market sets prices | 0x179 |
| Free scouting session chance | 25% (was 10%) | 0x17B |
| Stadium sections | 2.5× the configured min/max cost, since capacity now earns gate money | 0x051/52, 0x054/55, 0x057/58 |
| Kits, pitch patterns, created players | unchanged (one-off purchases) | — |
| Player purchases | negotiated fee (market) | market |

Prices are written into `CConfig::ms_iVars` (base + 0x75BA54) when a career loads, every turn, and when the market or
the post-match screen opens. The game shows and charges the same number everywhere. Percentage rules are applied to the
value the config loaded, and re-derived if the game reloads its config, so they never compound.

## Books

Categories 0–11 are stored in the 0xB4 block (`g_ext.econ`) and 16–26 in the 0xB5 block (`g_ext2`), each with
this-season and last-season totals. The Season Books screen shows income, spending, net and the coin balance,
plus a price list of the live values.

## Routing the game's own coin movements

build_mod.py hooks the entries of `CMyProfile::SubtractCredits` (0x377a18) and `CMyProfile::AddCredits` (0x377984) from
CAVE2. The cave recovers the original caller's return address; for the `CCredits` wrappers it is at `[sp+4]`. It then calls
`career_market_on_spend` / `career_market_on_income(amount, caller, base)`, which book the amount by caller (return
addresses from `data/COIN_FLOWS.md`):

- Spends: heal (split into medical and fitness by amount), training, scouting, stadium, pitch/kits (customisation), create
  player, friendly fees, claw-back (other), stock sign (purchases, normally bypassed by the market).
- Income: achievements and objectives (awards), welcome/share/come-back/IAP/login/easter-egg/reimbursement (other income).
  The match credit (CFE::Process 0x298cba) is skipped because the post-match hook has already booked it.

`tests/test_hooks.py` emulates both hooks directly and through the `CCredits` wrappers, checking the registers the resumed
stock code relies on. The harness checks the routing tables.

## No coin shop, no ad coins
- The `CFEShopDialog` constructor (0x254a9c) is patched into a plain "Coins: You do not have enough coins." OK box. This
  covers all 9 insufficient-coins pushes (heal, training, scouting, create player, sign, friendly, stadium ×2, kits); all of
  them pass a NULL callback.
- The header coin button no longer opens the shop (0x246c32).
- The post-match video doubler is gone (`CConfig::GetMaxDoubler` returns 0), and the other rewarded-video grants are
  skipped (0x203ae6).

## Post-match screen
The mod's rows come first (prize, gate, bonuses, cup prize), then the kept stock rows: achievements, and the cup win (paid
as the mod's cup winners' prize, 12% of the target). A friendly or all-star bonus is kept as it is. The stock
league-position award is dropped because the season-end payout replaces it. At most 9 detail rows plus the total are
shown; surplus achievements are folded into one row. Gate receipts use the real attendance (`ms_tInfo+0xf64`).
A new career's stadium holds 6,462.

## Not changed / limits
- The friendly entry fee is stored in a byte by the game (at most 255); it is left at the config value.
- Created players can't be sold through the market (the stock delete path is behind the market's sell gate).
- None of this has been run on the device; test on the tablet first.
