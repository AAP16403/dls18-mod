# DLS18 native UI components usable from the career-market mod (static analysis)

Status: complete for the questions asked (sections 0-9). libDLS18.so v5.064 armeabi-v7a, stock VAs (Thumb code: call
`base + va | 1`; data: `base + va`). Nothing here was run on a device.
**[V]** = read in the disassembly; **[G]** = guess / inference, not verified.
Tools: `F:\android modding\tmp\ui\gd.py` (GOT/string-resolving Thumb disassembler), `vt.py` (vtable dump), `loc.py`
(LOC id -> English), `scan3.py` (all CFEMessageBox ctor call sites). ABI is **softfp**: float arguments travel in core
registers / stack slots like ints (verified: `SetPixelRect(r1,r2,r3,[sp])`, `FESU_SetupText(r0..r3)`), so plain C `float`
parameters in the mod (built for armeabi-v7a softfp) match.

---------------------------------------------------------------------------------------------------------------------
## 0. Key facts (read first)

1. **[V] Callback argument = index of the pressed option button, in the order the buttons were added** (0-based).
   `CFEMessageBoxQueue::Process` 0x24ff06: `sel = box->GetSelection()` (`box+0x450`); if `sel >= 0` and `cb = box+0x40c`
   is set: `r = cb(sel)`, `SetSelection(-1)`; `r != 0` -> box deleted, `r == 0` -> box stays open (same frame state,
   selection cleared). The corner cancel cross (`box+0x414`, `AddCancelCross`) and tap-outside-to-close (`box+0x45d`)
   close the box **without calling cb** (only virtual `CancelledCB` +0xdc, empty in the base class).
2. **[V] Stock flag buttons are added in a fixed order, not by bit number** (`CFEMessageBox::SetupOptions` 0x248f8c, table
   in 6.2). For flags 3 (OK|Cancel) the order is **Cancel = 0, OK = 1**; verified with `ReviewRequestCB` 0x1fc63c (flags
   0x24 = "Later" idx 0, tick idx 1; the callback acts on 1).
   **`CFEMsgKeyboard` (0x24d784) passes flags 3**, so in keyboard boxes **0 = Cancel, 1 = OK**. The current
   `native_bridge.c` callbacks treat zero as Cancel and nonzero as OK, matching the traced order. The callback behavior
   still needs an on-device check.
3. **[V] Queue has only 4 slots** (`CFEMessageBoxQueue+0xe8..+0xf4`, active index `+0xf8`). `AddMessage` 0x24fd96: if all 4
   are used it **deletes the lowest-priority queued box** to make room. Keep at most 1-2 mod boxes queued.
4. **[V] All strings handed to the message-box API are copied**, so stack/static buffers may be reused right after the call:
   title -> `CFEArea::SetTitle` 0x21766a copies into `this+0xfc` (max 0x100 u16); description -> `SetDescriptionText`
   0x248bcc `new[]` + copy into `box+0x410`; option labels -> `CFEMessageButton` ctor copies (`+0x21c` subtext `new[]`;
   `CFEMessageBoxOptions` copies each option with `xstrlcpy`); setting-cell options/title -> `InitOptions` 0x257640 copies.
   Only pointers stored raw: the `TPlayerInfo*` statics of CFEMsgSignPlayer/SellPlayer (`ms_pPlayerInfo`) and the
   `int* pIndex` of setting cells -> keep those in static storage for the box lifetime.
5. **[V] LOC 0x4a5 `TXT_GLYPH_CREDIT` = U+0180 'ƀ' is the coin glyph** in the game font. Put `0x0180` in any u16 string to
   draw the coin icon inline (stock price text = `xsprintf(buf, "%s %s", LOCstring(0x4a5), FESU_GetCommaSeperatedString(v,3))`).
   `FESU_GetCommaSeperatedString(int v, int 3)` 0x2940e8 returns a (static) u16 "1,234".
6. **[V] Children may be added to a box between its ctor and `CFE::AddMessageBox`** (stock: CFEMsgSignPlayer/SellPlayer/
   EditPlayer ctors add the player card / settings table with vtable+0x6c `AddChild` inside the ctor). Option buttons can
   NOT be pre-added that way: virtual `SetupOptions` (+0xd0) runs at Init and starts with `ClearOptions` (+0xc0).
7. **[V] Box geometry fields** (set before AddMessageBox; `InitDimensions` 0x248d40 reads them):
   `+0x488` float = extra content height (px) reserved between the description text and the buttons (stock subclasses set
   it in their InitDimensions override: SelectLang 100, SignPlayer = card height, ScoutResults per row count);
   `+0x494` float = box width (default 590); `+0x490` = computed height; `+0x46c` int button width (210), `+0x470` int button
   height (64), `+0x474` spacing (10); `+0x498` float description text scale (0.9); `+0x480` u32 text colour (COL_WHITE).
   Height = descText + 20 + `+0x488` + const + buttons + 15.

---------------------------------------------------------------------------------------------------------------------
## 1. CFEMsgSignPlayer (ctor 0x251300, vtable 0x71d758, object 0x4e0)

`CFEMsgSignPlayer(this, TPlayerInfo* info /*r1*/, int teamID /*r2*/, int srcTeamID /*r3*/, bool b /*[sp]*/,
ESignPlayerMode mode /*[sp+4]*/, bool (*cb)(int) /*[sp+8]*/)`  [V]
- Base: `CFEMessageBox(this, LOCstring(0x3d7) "Sign Player?", NULL, NULL, flags 0, cb, b1 0, b2 1, i1 -1, 0x80)`.
  **Our own callback is accepted** (it is only stored in `box+0x40c`); `PlayerSignedCB` is just what the 3 stock callers pass.
- Statics written: `ms_pPlayerInfo` 0x7601d8 = info (raw pointer), `ms_eMode` 0x741d84, `ms_iTeamID` 0x741d7c,
  `ms_iSourceTeamID` 0x741d80, `m_bCreatePlayer` 0x7601ec = 0 (1 if `CDataBase::IsCreatedPlayerID`), `ms_pCard` 0x7601e0,
  `ms_pButton` 0x7601e4, `ms_pSparkleAnim` 0x7601e8, `ms_eNewMode` 0x741d88 (Init sets 0).
- `this+0x4dc` byte = secret-player flag (id == secret id, or POTW turn logic), `this+0x4dd` = bool arg.
- Price: `ms_iPlayerValue` 0x7601dc = `GetPlayerValue(info,-1,-1,1,1)` (0 on screen 0x10, secret price / created price
  overrides), copied to `this+0x47c`. The "%s %s" coin string built at 0x251498 into a stack buffer is **dead** (never used).
- Description: mode 1 -> "Select %s as your captain?" (LOC 0x415); mode 0/2 -> empty (so no description).
- Card: `new(0x600)` `CFEPlayerCard(params{info, teamID, 0, 0, -1, 0, flags, 0})` with flags **0x12** (mode 0), 2 (mode 1),
  0x82 (secret); alignment 0x12, `EnableInput(0)` (card not tappable), `AddChild(box, card, 0.5, 0.5, 0,0,0)`.
- **`SetupOptions` 0x252124** (mode 0/1, not created): option 0 = `AddOption(NULL, "fe_cross1.png", 0, NULL)` (cross icon,
  = "no"), option 1 = `new(0x230) CFEMessageCoinButton(NULL, *ms_iPlayerValue, index, NULL, 0.0f, 1 /*green*/)` via
  `AddOption(CFEButton*)`. Mode 1 (captain) / created player: option 1 is a tick icon instead. Mode 2 (after signing):
  Google/Facebook share buttons + OK tick.
  => **callback index: 0 = cross / decline, 1 = coin (buy) button**. (`PlayerSignedCB` also tests -1, which the queue
  never sends.)
- `SetMode` 0x2516b8: only does something for mode 2 (signed celebration: sparkles, confetti, title LOC 0x964
  "Player Signed", re-runs SetupOptions/SetupDimensions/DistributeOptions). `Process` 0x25234c: base Process, then if
  `ms_eNewMode != ms_eMode` -> `ms_eMode = ms_eNewMode; SetMode()`. Mode 0 is inert, so a mod callback that never sets
  `ms_eNewMode` gets a plain dialog.
- `RenderPre` 0x252388 / `RenderPost` 0x2524e0: extra drawing (starburst, old/new price ribbon) **only when `this+0x4dc`
  (secret flag) is set**.
- `InitDimensions` 0x25209c: `+0x488 = max(card height, const)` (+20 in mode 1), then base.

**Can we drive it with our own price/text/callback? Yes [V]:**
1. `box = new(0x4e0)`; ctor with a **static** `TPlayerInfo` copy (0xb0 bytes; `CDataBase::GetPlayerInfo` 0x20805c or copy
   the mod's own), teamID = selling club, srcTeam = same, b = 0, mode 0, cb = mod callback.
2. After the ctor, before `CFE::AddMessageBox`: `*(int*)(base+0x7601dc) = price;` `*(int*)(box+0x47c) = price;`
   `*(u8*)(box+0x4dc) = 0` (kills secret ribbon/sparkle paths); optional `CFEArea::SetTitle(box, L"Bid for ...")` 0x21766a
   and `SetDescriptionText(box, text)` 0x248bcc (copied; e.g. "Asking ƀ 1,200 · Wage 45/wk · 3 yrs" - several lines with
   `\n` are word-wrapped by `FESU_DrawBalanceText`) [G: `\n` handling not traced; wrap is automatic].
3. `CFE::AddMessageBox(box)`. The coin button shows our price because `SetupOptions` reads `ms_iPlayerValue` at Init.
4. cb(1) = buy pressed, cb(0) = declined; return 1 to close. Live price change while open: the coin button is
   `CFEMessageBox::GetOption(box,1)` (= `*(void**)(box+0x41c)`), value at **button+0x22c** (read every frame by
   `CFEMessageCoinButton::RenderText` 0x21fec0; 0 renders "Free" LOC 0x49c).
- Custom button labels are not supported by this class (SetupOptions is fixed); use recipe B/C for labelled buttons.
- The card still renders its own value text from `CFEPlayerCard::GetPlayerValue` (flag 0x10 -> buy price); hook 4.1 makes
  it show the mod price too.

## 2. CFEMsgSellPlayer (ctor 0x251160, vtable 0x71d670, object 0x4e0)  [V]

`CFEMsgSellPlayer(this, TPlayerInfo* info /*r1*/, bool (*cb)(int) /*r2*/)`:
- Base `CFEMessageBox(title LOC 0x67b "Sell Player?", NULL, NULL, flags **0x100008**, cb, 0, 1, **i1 = GetSellPlayerValue(info)**,
  0x80)`; `ms_pPlayerInfo` 0x7601d4 = info; description LOC 0x84b only for created players; card `new(0x600)` with
  params `{info, 0x102 /*user team*/, 0,0, modelOverride, 0, flags 0x22, 0}` stored at **`this+0x4dc`**, alignment 0x11,
  `EnableInput(0)`, added as child. Uses the **base** SetupOptions: flags 0x100008 = bit3 cross icon (idx 0) + bit20
  `CFEMessageCoinButton(NULL, box+0x47c, idx, NULL, 0, 0 /*not green*/)` (idx 1).
- **Generic "player card + one price button" dialog**: construct with any static TPlayerInfo and our cb, then before
  AddMessageBox write `*(int*)(box+0x47c) = amount` (the coin button reads `+0x47c` at Init), optionally SetTitle /
  SetDescriptionText. cb(1) = accept, cb(0) = decline. Card value text: flags 0x22 -> sell value (bit5), also covered by 4.1.

## 3. CFEMsgScoutResults (ctor 0x250558, vtable 0x71d3b8, object 0x9f8) and card grids

[V] ctor `(this, cb)`: base box (title LOC 0x99d, icon "fe_icon_scout.png", flags 0x80), then for `i < CConfig var 0x172`
(players per scouting session): `CSeason::GetLastSessionScoutedPlayer(i)` -> `CDataBase::GetPlayerInfo` -> card
`new(0x600)` flags 0x11 stored `this+0x4ec[i]`, plus a `CFEMessageCoinButton(NULL, GetPlayerValue(..), i, NULL, 0, 1)` under
it stored `this+0x5ec[i]`; slot positions `+0x7f0[i]`/`+0x8f0[i]`; `+0x4e0` count, `+0x4e4` rows, `+0x4e8` columns.
`Process` 0x2508ec polls all 64 slots: `coin[i]->IsReleased()` (vtable+0x98) or `card[i]->IsReleased()` -> `+0x7ec = i`, then
opens CFEMsgSignPlayer itself (hardwired). The player list comes from CSeason, so the class is **not reusable** for
arbitrary lists without hooks. It is, however, the verified template for recipe D (own card grid in a box).

## 4. Card price display and cheap hooks

### 4.1 Single per-card price hook  [V]
`CFEPlayerCard::RenderText` 0x237468 draws the value line only when `card+0x288 & 0x30` (0x10 = buy card, 0x20 = sell-value
card): at **0x237692** `blx CFEPlayerCard::GetPlayerValue` (4-byte PLT call, **the only caller** of GetPlayerValue 0x2343ac),
then `FESU_SetupText(0, colour, scale, -1)`, `FESU_GetCommaSeperatedString(v, 3)`, `FESU_DrawText` (coin image drawn next to it).
- Hook: `bl cave_cardprice` at 0x237692. In: r0 = `CFEPlayerCard*`; player id = `*(u16*)(r0+0x294)`, TPlayerInfo = r0+0x294,
  team = `*(int*)(r0+0x344)`, flags = `*(u32*)(r0+0x288)`. Out: r0 = value to print. Stock: tail-call `b.w 0x2343ac`.
  Called for every visible buy/sell card every frame (transfer search grid, hub carousel, sign/sell/scout dialogs, team
  management in sell mode): the cave must be cheap, i.e. **no dlopen/dlsym per call**. Suggested: the mod publishes a
  small table `{u16 id; i32 price}` + count at a fixed data slot the cave can reach PC-relatively (written once by
  `libCareerMarket` when it loads / whenever prices change); the cave does a linear/binary search and falls back to stock.
  (Implementation detail belongs to build_mod.py; not edited here.)
- Value 0 prints "0" on cards (only the coin button prints "Free").
- Card flag bits seen [V]: 0x2 (set by sign/sell dialogs), 0x10 buy (from `TPlayerSearchInfo+0xac` "available"), 0x20 sell
  value, 0x80 secret (`+0xad`), 0x400 player-dev stats, 0x8000 created player (`+0xaf`), 0x1000/0x1 (EditPlayer 0x1001),
  0x11 (scout results). `CFEPlayerCard::AddFlags` 0x23809a / `RemoveFlags` 0x2380a8 (u32 mask). A card created by the mod
  with flags 0x10 shows the (hooked) price; without 0x30 it shows no price line.
- Other per-card text usable as a status line: none cheap. The name bar uses `FESU_GetPlayerNames`; position via
  `FESU_GetPlayerPosString`; rating from `PU_GetPlayerPreciseRating`. Overriding those would affect every card in the game.
  Prefer the box description text or custom drawing (recipe E) for status lines.

### 4.2 Transfer search cells  [V]
`CFESDreamLeagueTransfers::SetupResults` 0x276bb4 builds one `CFETablePlayerCellTransfers` (0x11c bytes, ctor 0x256c3c from a
`TPlayerSearchInfo`, 0xb0 bytes: +0 player id, +4 team id, +0xac available, +0xad secret, +0xae bool -> card+0x35c,
+0xaf created) per result. `SetupPlayerCard` 0x256ca8 creates the card (`flags = available?0x10 | secret?0x80 |
created?0x8000`). Its price text is the same `CFEPlayerCard::RenderText` path -> hook 4.1 covers it. Team management cards
(sell mode) likewise (flag 0x20).

## 5. Number input

### 5.1 Keyboard box  [V]
- `CFEMsgKeyboard(this, const u16* title /*r1*/, const u16* placeholder /*r2*/, const u16* initialText /*r3*/,
  int maxChars /*[sp]*/, EKeyboardType type /*[sp+4]*/, bool (*cb)(int) /*[sp+8]*/)` 0x24d784, **object 0xce8**, flags 3
  (**0 = Cancel, 1 = OK**, see 0.2). Copies title? (base, copied) / placeholder into `+0x4dc` / initial text into `+0x8dc`.
  `+0xce4` = `CFETextField*` (0x138 bytes, ctor 0x241468 `(text, EKeyboardType, int, int)`), `+0xcdc` maxChars, `+0xce0` type.
  `Process` 0x24d880 copies the current text into **`box+0x8dc`** every frame, so the callback can read
  `(u16*)(box+0x8dc)` directly (512 u16 max) instead of going through JNI.
- **0x241FB5 = `CFETextField::GetText()` 0x241fb4 (Thumb)**: calls Java `FTTKeyboard.GetText` via JNI, returns a static
  u16 buffer at 0x75f168 (0x400 bytes, cleared each call).
- The type is stored in the text field (`+0x128`); the Java side is told only a field id (`ShowKeyboard(I)V`), so whether a
  numeric keypad appears for some EKeyboardType value is [G] (stock callers: CFETransferOptionsMenu name search,
  CFESCustomDataTeamName, stadium name, CFETableSettingCellTextField).

### 5.2 Stepper inside a message box (recommended for fee / wage / years)  [V]
Stock template: `CFEMsgBoxEditPlayer` ctor 0x24c09c (shirt number + boot colour steppers in a box) and
`CFEMsgBoxSelectLang::Init` 0x250fbc.
- `CFESettingsTable(this, int cols, int rows)` 0x25b9b4, **object 0x110** (a `CFELayoutTable`). Add to the box:
  `SetAlignment(table, 0x11)` 0x25ec90, `SetPixelRect(table, x, y, w, h)` 0x25f3f4 (floats), `box->vt[0x6c](box, table, f1 /*r2*/, f2 /*r3*/,
  f3 /*[sp]*/, f4 /*[sp+4]*/, u32 /*[sp+8]*/)` = `CFEEntity::AddChild(child, f,f,f,f, flags)` 0x25fad2; stock values:
  EditPlayer (0.5, 0, 0.9, 0, 0), SelectLang (0.1, 0, 0.8, 0, 0), sign-dialog card and queue (0.5, 0.5, 0, 0, 0)
  [meaning of the floats G: normalised anchor/size].
- `CFETableSettingCellInt(this, u8 id /*r1, stock 0*/, const u16* title /*r2*/, u16** options /*r3*/, u8 count /*[sp]*/,
  int* pIndex /*[sp+4]*/, bool wrap /*[sp+8]*/, void (*onChange)(int& idx, signed char dir) /*[sp+0xc]*/)` 0x2581d0,
  **object 0x158**. Integer variant with `const int* values` instead of `u16** options`: 0x258160 (shows the number).
  - Options and title are copied (InitOptions 0x257640); up to 255 options (u8 count).
  - Initial index = `*(u8*)pIndex % count` (ctor reads the low byte), stored at cell+0x14c and written back to `*pIndex` on
    every arrow press; `onChange(&cell->index, -1/+1)` is called after each step (may be NULL; may modify the index).
  - Hold-to-repeat: `SetHoldFrameCount(cell, int a, int b)` 0x25828c (+0x138/+0x13c thresholds). `wrap` = allow wrap-around
    (when 0 the left/right arrow is disabled at the ends).
  - `CFETableSettingCell::UpdateOption(cell, int i, const u16* text)` 0x2575f0 replaces option text live (copied).
- Add the cell: `table->vt[0xb0](table, cell, col, row, -1.0f, -1.0f)` = `CFELayoutTable::AddCell` 0x25b052.
- Reserve room: `*(float*)(box+0x488) = 100.0f` per row (SelectLang: 100 for one row).
- Read the value in the box callback from the static `int` passed as `pIndex` -> `options_value[idx]`.
Other widgets found (not needed): `CFETableSettingCellBool` (toggle), `CFETableSettingTabs` 0x259908 (tab strip),
`CFETableSettingCellTextField` 0x259a78 (text field row), `CFEOptionButton` 0x221ba4 (arrow cycler, no stock caller of the
C1 ctor), `CFESlider(const u16*)` 0x22d1c8 (float 0..1, `GetValue` 0x22d50e; no stock caller found), `CFETransferSearchMinMax`
0x244d74 (min/max range picker of the transfer filter; `Setup(bool, 10 ints, bool)` 0x244dc8, part of CFETransferFilter's
menu; not suitable inside a box without more work).

## 6. CFEMessageBox building blocks

### 6.1 Base class  [V]
`CFEMessageBox(this, const u16* title, const u16* desc, const char* icon /*r3, png name or NULL*/, int flags /*[sp]*/,
bool(*cb)(int) /*[sp+4]*/, bool b1 /*[sp+8] -> +0x45d close on tap outside*/, bool b2 /*[sp+0xc] -> +0x478*/,
int i1 /*[sp+0x10] -> +0x47c coin value for bits 18/20*/, int area /*[sp+0x14], stock 0x80*/)` C2 0x248aa8, **object 0x4dc**.
Fields: +0x40c cb, +0x410 description (heap copy), +0x414 cancel-cross button, +0x418..+0x444 option buttons (max **12**),
+0x448 flags, +0x44c option count, +0x450 selection, +0x45c close request (set 1 to close without cb), +0x464/+0x468
columns/rows, +0x47c coin value, +0x480 text colour, see 0.7 for geometry.
Useful methods: `SetDescriptionText` 0x248bcc, `CFEArea::SetTitle` 0x21766a, `SetTitleTextColour(u32)` 0x249740,
`SetPriority(int)` 0x2498fa, `GetOption(int)` 0x249726, `ClearOptions` 0x248cd4 (vt+0xc0), `InitDimensions` 0x248d40 (vt+0xc4),
`SetupDimensions` 0x248e80 (vt+0xc8), `DistributeOptions` 0x2493c8 (vt+0xcc), `SetupOptions` 0x248f8c (vt+0xd0),
`AddOption(text, icon, scheme, subtext)` 0x2496ba (vt+0xd4), `AddOption(CFEButton*)` 0x2495f0 (vt+0xd8), `AddCancelCross`
0x24934c, `Process` 0x249748 (vt+0x14), `RenderText` 0x249830 (vt+0x94), `RenderCustom` (vt+0xbc, empty), `CFE::AddMessageBox`
0x298608, `CFE::DeleteActiveMessageBox` 0x298668.
- **Button layout** (`DistributeOptions` 0x2493c8, not overridden by any class used here): 1 button -> 1 column, 2 or 4 ->
  2 columns, 3 -> 3 columns, > 4 -> 3 columns with as many rows as needed; `AddOption(CFEButton*)` widens the box
  (`+0x494`) when the columns do not fit. `CFEMessageBoxOptions` (the mod's current UI) only overrides `SetupOptions`
  (vt+0xd0: one `AddOption(text, "", 0, NULL)` per string), so it already gets this grid; what it cannot do is sub-labels,
  colour schemes, icons, coin buttons, cards or steppers.
- **Option button** = `CFEMessageButton(text, icon png or "", index, subtext, 0.0f)` 0x21f294, object 0x22c: `text` main
  label, `subtext` **second, smaller line** (copied to +0x21c, drawn by RenderText 0x21f6c8). `EButtonColScheme` via
  `CFEButton::SetScheme` 0x21a2fc: 0 normal, 2 green/highlight (coin buttons pass 2 when "green"), 0xb (ToS/privacy links)
  [colours of other values G].
- **Coin button** = `CFEMessageCoinButton(this, const u16* label, int value, int index, const u16* sub, float 0, bool green)`
  0x21fe70, object 0x230: draws label (if any) + coin image + comma value (`+0x22c`, live).

### 6.2 Flag bits -> buttons, in the order they are added [V] (index = position in this list among the set bits)
| order | bit | label |
|---|---|---|
| 1 | 0x2 | Cancel (LOC 0xbc) |
| 2 | 0x8 | cross icon (fe_cross1.png, no text) |
| 3 | 0x10000 | Stop |
| 4 | 0x40 | Visit Website |
| 5 | 0x20 | Later (+cross icon) |
| 6 | 0x800 | Copy info |
| 7 | 0x20000 | Copy Kit URL |
| 8 | 0x4000 | More Info |
| 9 | 0x8000 | Resume |
| 10 | 0x80000 | Settings |
| 11 | 0x200000 | Terms of Service (scheme 0xb) |
| 12 | 0x400000 | Privacy Policy (scheme 0xb) |
| 13 | 0x1 | OK |
| 14 | 0x2000 | Continue |
| 15 | 0x10 | "ƀ Get Coins ƀ" |
| 16 | 0x1000 | Reset (+cross icon) |
| 17 | 0x4 | tick icon (fe_tick1.png) |
| 18 | 0x100 | Not now |
| 19 | 0x200 | Video Clip |
| 20 | 0x400 | Create Game |
| 21 | 0x40000 | coin button, value `+0x47c`, green |
| 22 | 0x100000 | coin button, value `+0x47c`, normal |
| last | 0x80 | not a button: `AddCancelCross()` (corner X, closes without cb) |

### 6.3 Embedding a player card [V]
`card = new(0x600, 0, 0)`; `CFEPlayerCard::CFEPlayerCard(card, TPlayerCardInitParams)` C1 0x233b88; the params struct is
passed **by value** in r1-r3 + 5 stack words:
`{ TPlayerInfo* info /*r1, copied 0xb0 into card+0x294*/, int teamID /*r2 -> +0x344*/, int p2 /*r3, stock 0*/,
int p3 /*[sp] -> +0x348, stock 0 or 3 for GK?*/, int modelOverride /*[sp+4], CGfxStarHeads::GetModelOverride(id) 0x310568 or -1*/,
int p5 /*[sp+8], stock 0*/, u32 flags /*[sp+0xc] -> +0x288*/, int p7 /*[sp+0x10], bit0 -> +0x35c*/ }`.
Then `SetAlignment(card, 0x12 or 0x11)`, `EnableInput(card, 0)` (display only) or leave input on to make it tappable
(`card->vt[0x98]` IsReleased polls a tap, as ScoutResults does), `SetPixelRect(card, x, y, w, h)` (scout: w 252, h 148 are
the 0x437c/0x4314 floats), `box->vt[0x6c](box, card, 0.5f, 0.5f, 0, 0, 0)`. The box deletes its children.
Card fields: +0x285 available byte (`GetAvailable`/`SetAvailable` 0x234364), +0x288 flags, +0x294 TPlayerInfo, +0x344 team,
+0x358 dev level.

### 6.4 Tables
`CFELayoutTable(int cols, int rows)` 0x25aeb8 (grid container; `AddCell` 0x25b052, `GetCell(col,row)` 0x25afe4,
`ResizeGrid` 0x25b16e, `SetHighlightRow` 0x25b016); `CFESettingsTable` 0x25b9b4 (a layout table for setting rows, plus
`AddSettinglabel(text, col, row, f, f)` 0x25b9e4); cells: `CFETableTextCell(text, bool, u32 col, u32, u32, f, f, f)` 0x259e58,
`CFETableTitleCell(text)` 0x25a0e0, `CFETableImageCell` 0x25642c, `CFETableLogoCell(teamID, f, f, bool)` 0x25672c,
`CFETablePlayerCell(CFEPlayerCard*)` 0x256b64, `CFETableButtonCell(CFEButton*, ...)` 0x255b24. `CFERewardTable(int,int,int)`
0x25b3b8 is the animated post-match coin tally table (not a generic list). A text table (club, fee, status per row) inside a
box is possible with `CFELayoutTable` + `CFETableTextCell` but no stock box does it; [G] geometry untested. Prefer recipe E
(custom draw) for text tables.

### 6.5 Message-box classes in the lib (one line each; * = could host market UI)
CFEMessageBox (base, flag buttons) * · CFEMessageBoxOptions (string-list buttons; current mod UI) · CFEMessageBoxWithProcess
0x253dcc (base + per-frame `bool(*)()` hook) * · CFEMsgLoading (spinner, WithProcess) · CFEMsgKeyboard (text field) * ·
CFEMsgSignPlayer (card + coin price) * · CFEMsgSellPlayer (card + coin price) * · CFEMsgScoutResults (grid of cards + coin
buttons) * (template) · CFEMsgBoxScoutPlayer (scouting position picker, CFEMenu options) · CFEMsgBoxEditPlayer (card +
settings table steppers) * (template) · CFEMsgBoxSelectLang (settings-table stepper) * (template) · CFEMsgCreatePlayer
(multi-mode, settings table, 3D player) · CFEMsgPlayerDevSelect / CFEMsgPlayerDevResults (card + training options) ·
CFEMsgPOTW (player of the week cards) · CFEMsgAchievements (rows of achievements + coins, custom RenderCustom) ·
CFEMsgFriendlyMatch / CFEMsgNewLeague / CFEMsgStadium / CFEMsgSPW / CFEMsgImage / CFEMsgWatchVideo / CFEMsgGetCoins /
CFEMsgPromotion (image + text + options, custom RenderCustom) · CFEMessageBoxTournamentWin (trophy) · CFEMsgSocialShare ·
CFEMsgCloudConflict (two save summaries) · CFEMsgFormation (formation picker) · CFEMsgFacebookLeaderboard · CFEMsgTMHelp /
CFEMsgStadiumHelp / CFEMsgTransfersHelp (help pages) · CFEMsgMultiConnect (online) · CFEShopDialog (IAP).

## 7. Full screens

- `CFE::Forward(int screenID, bool /*unused*/, void* p1, void* p2, bool, bool)` 0x2984cc -> `CFEScreenStack::Forward(screen,
  p1, p2, b, b)`; the target screen gets `SetCustomParams(p1, p2)` (e.g. `CFESDreamLeagueTransfers::SetCustomParams` stores
  p1 in `ms_pTeam`). Stock: `Forward(4, 1, (void*)2 /*sell mode*/, 0, 1, 0)` opens team management in sell mode.
  `CFE::Back(bool)` 0x29910c. `CFE::GetCurrentScreenID` 0x297824 (0x10 = captain select; card Setup tests 0x19 [G: transfers]).
- **Transfer search screen as a market hub** [V structure, G feasibility]: results come from
  `*(TPlayerSearchInfo**)CFESDreamLeagueTransfers::ms_pPlayerSearchInfo` (0x763ba0 -> a `TAsyncPlayerSearchInfo` 56 bytes,
  e.g. `CTransfers::ms_tAsyncPlayerSearchInfo` 0x75cde4: +0 array pointer (stride 0xb0), +4 job state (3 = finished),
  +0x2c count). `Process` 0x276954: state 3 -> `ms_bSetupResults` (0x763ba8) = 1 -> `SetupResults` rebuilds the grid;
  a card tap -> `CurrentPlayerBid` -> CFEMsgSignPlayer. Feeding our own list = overwrite array entries/count after the async
  search finished and set `ms_bSetupResults = 1`; but every filter change (`ApplySearchFilter` 0x2768a0) restarts the async
  job (`CTransfers::StartAsyncPlayerSearch` 0x2109d8 / `Search` 0x210a00) which overwrites it. A cleaner way is a hook at the
  end of `CTransfers::Search` that post-filters/replaces results (not analysed). Medium-high risk; not recommended as the
  first step. The existing buy gates (STOCK_TRANSFER_FLOW 3.1/3.2) already let the stock screen act as the market's browse
  view, with hook 4.1 showing mod prices on its cards.

**Implemented through v27: paged full-screen market lists, selected details, and contextual navigation.** The dedicated market `CFEScreen` adds up to four
native `CFEPlayerCard` children in two columns. The bridge loads each `TPlayerInfo`, calls the card constructor at
`0x233b88`, sets alignment `0x12` and the pixel rectangle, and adds the card through the screen's child vtable. After
processing screen children, it polls each card's `vt[0x98]` release method and routes the selected row to the existing
full-screen offer/player detail flow. Club and fee/status text is drawn below each card. If any card cannot be
constructed, the screen removes partial cards and uses its existing selectable text rows. This grid runs on Market,
My Squad, Sales, Inbox, and Shortlist. All seven lists now use four-item pages with Previous/Next controls and a
visible range/total counter in a footer reserved from the cards and descriptions. Page offsets are aligned and bounded;
tab, position/club filter, and search changes reset the relevant list. The host harness checks empty, full, and partial
pages plus both navigation limits, and `test_hooks.py` checks the wiring for all seven modes. V18 displays the current
player/offer description above the action controls in the right panel and preserves empty-list explanations. Its shared
layout helper switches card-detail pages with four or more actions to two columns and is host-tested across 1–10 actions
at 768 px and 300 px heights. V19 adds persistent position, club, and name-search controls above the Market cards;
position cycles in place, active filters are highlighted, and redundant Previous/Next actions were removed from the
right pane. V20 applies card/page selection to Sales, Shortlist, and Squad as well, leaving only contextual actions in
their right panes. V21 applies the verified scheme 2 highlight to primary actions and leaves navigation or destructive
alternatives neutral. V22 routes the top Back button to the owning tab for child pages and exits only from root lists
or menu/close notices. V23 renders a teal outline after the selected native card so selection remains visible over
the card art. V24 adds deterministic O(n log n) Market sorting by rating, market value, or wage. V25 separates persistent Market and Sales position filters. V26 moves Sales position filtering into an inline toolbar above its cards. V27 adds an inline previous/selected/next club toolbar to Squad and removes redundant club navigation from Finances. Addresses were checked
with `analysis/armdis.py`; actual drawing and touch behavior remain
unverified in-game.

**v35: market inside the stock transfer screen** (native_bridge.c "Transfer screen UI", 7 modcore hooks). This replaces
the hub box on entry. The market's lists are fed to the stock card grid by wrapping `SetupResults` 0x276bb4. It reads the
count through `ms_pPlayerSearchInfo` and the entries from `CTransfers::ms_tAsyncPlayerSearchInfo`+0, so both are swapped
only while the stock body runs, via `modcore_resume`. Buttons are `CFETextButton` children of the screen (top band) and
of the footer menu (the gap between footer buttons 0x2a Free Scout and 9 Sell Player). The footer is its own render
layer drawn after the screen (`CFEEntityManager::RenderAll` 0x2609e4). Child rect = parent origin + pixel rect
(`CalculateRect` 0x25edea). Card taps go through `CurrentPlayerBid` 0x276fb8 (select), and card prices through
`CFEPlayerCard::GetPlayerValue` 0x2343ac.

## 8. Recommended recipes (ranked by value / risk)

Common helpers (all Thumb, `|1`): `new` = `_Znwj13EFTTMemHeapIDi` PLT **0x1c06ac (ARM stub, call even address as ARM, as the
mod already does)** `(size, 0, 0)`; `AddMessageBox` 0x298608; `SetDescriptionText` 0x248bcc; `SetTitle` 0x21766a;
`AddChild` = `(*(void***)box)[0x6c/4]`.

**A. Offer / bid dialog with player card and price button (value high, risk low)** - CFEMsgSellPlayer or CFEMsgSignPlayer
used as a template (sections 1-2): static TPlayerInfo, own cb, write price to `box+0x47c` (Sell) or `0x7601dc`+`box+0x47c`
(Sign) before AddMessageBox, title + multi-line description (terms, ƀ glyph). Result: stock-looking card dialog with a big
coin button. cb(1) accept, cb(0) decline.

**B. Clean multi-button prompt with sub-labels (value high, risk low)** - `CFEMessageBoxWithProcess` 0x253dcc
`(this, title, desc, icon, int flags /*[sp]*/, cb /*[sp+4]*/, bool(*process)() /*[sp+8]*/, void(*)() /*[sp+0xc], stock 0*/)`,
**object 0x4e8** [V: CFESPauseMenu::Process 0x27ea60 and CFESMultiInvite::Init 0x289e76 allocate 0x4e8] (+0x4dc process fn,
+0x4e0 second fn, +0x4e4 skip-first-frame byte). Process fn is called every frame (from the 2nd frame);
returning true closes the box without cb. On its first call: `ClearOptions` (vt+0xc0), then `AddOption(box, L"Accept",
"", 2 /*green*/, L"ƀ 1,200 · 3 yrs")` (vt+0xd4) for each button, then `InitDimensions` (vt+0xc4) - the exact sequence
`CFEMsgBoxSelectLang::Process` 0x2510c0 uses. Gives a 2-3 column button grid with two-line labels; cb gets the index.
(Alternative with no per-frame fn: recipe E's SetupOptions override.)

**C. Fee / wage / years steppers inside the offer dialog (value high, risk low-medium)** - section 5.2: add a
`CFESettingsTable` child with 1-3 `CFETableSettingCellInt` rows (u16** option labels like "ƀ 1,250", pIndex = static ints),
`box+0x488 = rows*100`, buttons via flags 0x3 (Cancel = 0, OK = 1) or 0xc (cross = 0, tick = 1, as the pause menu uses).
On OK read the static indices. Replaces the keyboard for amounts; press-and-hold repeats.

**D. Popup card-grid alternative (not used by the dedicated market screen)** - own `CFEMessageBoxWithProcess`
with N `CFEPlayerCard` children (input enabled) and optional `CFEMessageCoinButton` children under them, laid out like
CFEMsgScoutResults (card w 252 h 148 px, `SetAlignment 9`, positions via `SetPixelRect`), `box+0x488` = grid height,
`box+0x494` wider (e.g. 900) for 3 columns. Process fn polls `child->vt[0x98](child)` (IsReleased) for each card/button,
stores the index in a mod global and returns true (close) or opens the next box. Keep N small (<= 6-8): each card loads a
3D headshot.

**E. Custom-drawn box via vtable clone (value very high, risk medium)** - copy the 232-byte vtable of CFEMessageBox from
`base+0x71c1c0` into a static mod array, set `*(void**)box = clone + 8`, and replace slots with mod functions (`this` in r0):
`+0xbc RenderCustom` (draw anything each frame: `FESU_SetupText(font 0-3, u32 colour, scale, -1.0f)` 0x294944 then
`FESU_DrawText(TRect* out /*r0*/, x /*r1*/, y /*r2*/, w /*r3*/, h /*[sp]*/, const u16* text /*[sp+4]*/, u32 flags 0x12
/*[sp+8]*/, 1.0f, 1.0f, bool 1, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f)` 0x2949f0 - argument values copied from
CFEMessageCoinButton::RenderText 0x21ff96 (flags bit 0x10 -> centred, 0x20 other alignment [G]); `FE2D_DrawRectCol(x, y, w, h, u32 col)` 0x28af84; `FETU_DrawImage(png, x, y, w,
h, col, f, f, f)` 0x2969a8; box rect via `CFEEntity::GetRect(TRect* out, box)` 0x25eefc), `+0xd0 SetupOptions` (own labelled
buttons: call ClearOptions 0x248cd4 then AddOption 0x2496ba), `+0x14 Process` (call `CFEMessageBox::Process` 0x249748 first,
then own touch/timer logic; return 0). Multi-colour text, tables of clubs/fees, progress bars, badges become possible.
Clone memory must outlive the box; the stock destructor (slot +4) still frees it. Colours are 0xAARRGGBB [G, from COL_*
constants usage].

**F. Mod price on every card (value medium, risk low)** - hook 4.1 at 0x237692 with a non-dispatch cave.

Not recommended: reusing CFEMsgScoutResults for arbitrary lists (player source hardwired), feeding the transfer screen's
async result array (race with the search job), CFETransferSearchMinMax in a box.

## 9. Open points (not verified)
- Whether `\n` in description text forces a line break (wrap is automatic via FESU_DrawBalanceText).
- Exact Init timing of a queued box (AddChild during AddMessageBox vs first frame); all recipes write fields before
  AddMessageBox, which is safe either way.
- `EKeyboardType` values -> numeric keypad on the Java side.
- Colour format and meaning of EButtonColScheme values other than 0 / 2 / 0xb.
