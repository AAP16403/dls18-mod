# Patching rules for `build_mod.py` and the career-market bridge (read before editing)

Four tablet builds crashed on 2026-09-24/25. Every one of them passed `test_patch.py`, because the
tests don't execute the code paths that broke. Follow these rules.

## 1. Never jump to an absolute address

`libDLS18.so` is position-independent: Android loads it at a **random base** every launch. Addresses
in this project (`0x2e0f78`, `0x1fc9f4`, ...) are offsets from that base.

Wrong (v9 crashed with `fault addr 0x002e0f78`, `pc 002e0f78 <unknown>`):

```
movw r0, #0x0f79
movt r0, #0x002e
bx   r0            @ jumps to raw 0x2e0f79, not base+0x2e0f79
```

Right: use PC-relative branches, which keystone resolves relative to the cave address:

```
b.w  0x2e0f78
bl   0x3851ac
```

`movw`/`movt` are fine for **data constants** (e.g. `T2_LIT`), never for a branch target. When the
target isn't reachable with `bl` (e.g. calling a PLT stub through a register), compute it from a
PC-relative anchor, as the market dl stubs do: `adr r12, <cave label>`; subtract the label's link-time
address; add the target's link-time address; `blx r12`.

## 2. Never use a far *conditional* branch out of the cave

Keystone silently mis-encodes Thumb-2 conditional wide branches (`bge.w`, `blt.w`, `bne.w`, ...)
over long distances. The cave (`0x1fc6f0`) is about 0.9 MB from the hooks near `0x2e0xxx`. The
19:12 skill-tier build had `bge.w 0x2e0f88` encoded as a branch to `0x2dd9a6`. That address is inside
`CPlayer::SetAnimFromStateAction`, so the game crashed as soon as a player started moving.

Wrong:

```
cmp   r5, fp
bge.w 0x2e0f88
b.w   0x2e0f78
```

Right: take a short conditional branch inside the cave, then use unconditional `b.w` (range ±16 MB, which keystone encodes correctly):

```
cmp   r5, fp
blt   better
b.w   0x2e0f88
better:
b.w   0x2e0f78
```

## 3. Keystone silently drops lines it can't parse (check your f-strings)

The cave source is a Python f-string. In `cave_market_dl_close`, the constants were written as
`#{{DL_CLOSE_PLT & 0xffff:#x}}`. The doubled braces made the text a literal `{...}`, and keystone
**skipped those lines without an error**. As a result the stub computed `ip = <its own cave address>` and ran
`blx ip` on an even address. That switched the CPU to ARM mode on Thumb code, and the game crashed at boot with the pc in
random memory (Play Services / font mappings).

- Use `{...}` for values and `{{...}}` only where the assembly text itself needs literal braces (register lists: `push {{r4, lr}}`).
- `asm()` now fails if the source contains `#{`, or if keystone's statement count doesn't match the
  source (one statement per line, plus one per extra `;`).

## 4. Thumb bit: set it for game functions, never for PLT stubs

When the market bridge (`career_market/native_bridge.c`) or cave code calls a game address through a pointer:
- **Game functions** (in `.text`) are Thumb: use `address | 1`, e.g. `base + 0x20DA0D`.
- **PLT stubs** (in `.plt`, `0x1c0000`–`0x1ea000`) are **ARM**: use the even address, e.g. `dlopen` =
  `base + 0x1D90FC` and `operator new` = `base + 0x1C06AC`. The alpha build used `0x1D90FD`/`0x1C06AD`. That ran
  ARM code as Thumb, which is garbage, and crashed at boot.
- Check every `base + 0x...` in `native_bridge.c` with `analysis/armdis.py` (`armdis.PLT`, `armdis.SYMS`). Each
  must be a function start with the right Thumb bit.

## 5. Data addresses: verify against the game's own code

`profile_instance()` read `*(base + 0x4BA07C)`. That address is inside `.text` (an OpenSSL routine), not data.
The profile is the global object `MP_cMyProfile` at `base + 0x84A260` (GOT slot `0x73052C`), and the current
season is at `profile + 0x14`. Game code such as `CFESDreamLeagueOverallStatistics::SetupFixturesTable` does exactly
this. Before using a global, find the game code that reads it and resolve the GOT slot.

## 6. Build-time guards (don't remove them)

`build_mod.py` exits with an error if:
- any branch leaving the cave lands on an address that isn't written literally as `b*/bl 0x...` in the
  source (this catches mis-encoding);
- any `bx`/`blx` uses a register built directly by `movw`/`movt` (this catches absolute jumps);
- keystone skipped a line, or an f-string placeholder was left unformatted (this catches dropped instructions).

If a guard fires, fix the assembly. Don't work around the guard.

## 7. Verify before installing on the tablet

1. `python mod/build_mod.py --out <lib>`. It must finish without a guard error.
2. Run `test_patch.py`. Its hard-coded `bm_cfg()` is missing newer options. Build the config from
   `build_mod.main()`'s parser defaults instead: patch `ArgumentParser.parse_args` to capture the namespace.
3. For any new or changed cave routine, emulate that routine in Unicorn **with the library mapped at a non-zero
   base** (e.g. `0x40000000`), which catches absolute-address bugs. Stub the PLT entries it calls (`dlopen`/`dlsym`/
   `dlclose`/...), assert the CPU is in ARM mode on entry to each stub, and assert it resumes at the intended game
   address with the right `sp`. Cover every branch path.
4. Disassemble the cave with capstone and check every branch that leaves the cave.
5. Install as an update so the save is kept (the same key in `mod/keys/` signs every build):
   `platform-tools/adb.exe -s e93a82ce install -r <apk>`. Then launch the game and watch it
   (`adb shell monkey -p com.firsttouchgames.dls3 -c android.intent.category.LAUNCHER 1`). Boot crashes show up
   within about 5 s.
6. Crash logs: `adb logcat -b crash -d`. When the backtrace has only one frame, get the full report with `adb shell dumpsys dropbox --print data_app_native_crash`. The report has no memory map, but `r10` often holds the
   `libDLS18.so` base. Subtract it from the other registers to find cave addresses.

## 8. Save versions: the minimum version must not be above the version you write

`CFTTSerialize::SerializeInternal` always writes a field, but on load it skips any field whose minimum version is above
the version stored in the file (serializer+0x18 = the file's version; the constructor reads it from the file).
So a new save block with minimum version N requires `build_mod.py` to write profile version N or higher
(`SAVE_VERSION_SETUP`/`SAVE_VERSION_BOOT`, now 0xB3). Otherwise saves get the block written but loads skip it,
and every later field is misread. The game refuses to load files newer than its constant, so an older build rejects
a 0xB3 save cleanly instead of misreading it.

## 9. AI rosters live in the default link table

`CalculateLinks` rebuilds the live table from the default table (DB+0x24) and keeps only the user link, and only the user link
is saved. Never move AI players in the live table: they revert. See `career_market/data/ROSTER_MECHANICS.md` and the
roster section of `native_bridge.c`. Any change to the market logic must pass `career_market/tests/run_tests.sh`
(0 violations, identical digests with `--restart-every -1` and `1`) and the ARM cross-check
(`career_market/tests/arm_crosscheck.py` must match the harness `api_digest`).

## 10. Shell quoting when patching files from Bash

Python heredocs in Git Bash turned `\n` escapes inside C strings into real line breaks and broke `harness.c` twice.
Write patch scripts with the Write tool, or use the Edit tool, whenever the text contains backslashes.

## 11. Keep the cave string block an even length

The cave code is padded with 2-byte NOPs up to `MARKET_RODATA_OFFSET`. When the career strings had an odd total length, the
padding came up one byte short and every string landed one byte early. `dlopen` got "ibCareerMarket.so", so the whole market
would have silently failed on the device. `build_mod.py` now pads the block to an even length and exits if the
alignment is off. After any cave or string change, emulate every market hook and check the `dlopen`/`dlsym` names (see
`career_market/tests/test_hooks.py <libDLS18.so>`).

## Useful facts

- Tablet: Xiaomi 23043RP34I, adb serial `e93a82ce`, ABI `arm` (armeabi-v7a lib).
- Cave = body of unused `FTTCollectionsTest` at `0x1fc6f0`, 1,676 bytes. The tail holds the career-market strings.
  Adding routines moves every label after them, so always re-check branches after any cave edit. **It is full**
  (v6, 2026-09-25): only ~6 bytes free after the existing P1-T3/market-dispatch routines. Don't add more here
  without freeing space first (check with `python build_mod.py` — it exits with the exact overflow if it doesn't fit).
- Cave2 = `DEBUGCHARACTER_RenderPlayerData` + `DEBUGCHARACTER_RenderPlayerPitch`, `build_mod.py CAVE2 = 0x381368`,
  1,112 contiguous bytes (found when Cave filled up building v6's economy hooks). Verified free-standing (not a
  vtable method), zero `bl`/`blx` callers (`analysis/xref.py`), zero raw-address hits in the file outside their own
  body, zero `.rel.dyn` relocations in range — same verification bar as Cave itself. Only 134 bytes used so far
  (v6's `cave_econ_match`/`cave_econ_season`); plenty of room for future hooks. Reaching Cave1 routines (e.g.
  `cave_market_dispatch`) from here needs a plain `bl`/`b.w` to their absolute address (position-independent, no
  base math needed, since both caves are in the same loaded image) — but reaching Cave1's *rodata* needs a local
  string in Cave2's own tail (`CAVE2_RODATA`); `ADR` can't span the ~1.5 MB between the two caves.
- `SetAnimFromStateLoco` hook `A2` at `0x2e0f74`. The cave must return to `0x2e0f78` (candidate better)
  or `0x2e0f88` (candidate not better) with all registers except `r0`/`r5` preserved.
- Career-market hooks: `CSeason::Serialize+0xaa` (`0x36bc42`, runs at boot when the save loads),
  `CSeason::PlayTurn+0xd6` (`0x36a078`) and `CFESDreamLeagueTransfers::OnScreenEnter+0x24` (`0x276490`). Each one calls
  `libCareerMarket.so` through dlopen/dlsym/dlclose and passes the `libDLS18.so` base as the 3rd argument.
- v6 economy hooks (cave2): per-match income at `CFEPostMatchCreditAwards::SetupCreditAwardInfo` tail (`0x23a274`,
  over `blx SetMatchCredits`) and season-end payout at `CFlow::Process` step 6 (`0x29900a`, over
  `blx CSeason::NextSeason`). Same dlopen/dlsym/dlclose dispatch as the others, via `cave_market_dispatch` in Cave1
  (reached from Cave2 with a plain `bl`).
- `libCareerMarket.so` is built by `career_market/build_native_bridge.ps1` (zig, `arm-linux-android`, softfp).
  The ELF header says hard-float, but float arguments are passed in core registers (softfp), which matches the game.
- Current built package (2026-09-26): `mod/build/DLS18_career_market_v27.apk` (v14 stats rework + paged full-screen
  transfer market UI with selected details, persistent Market filters, and focused card-list actions; see
  `STAT_REWORK.md` and `career_market/BALANCE.md`). Curve, patch, hook, 12-season host, UI wiring, and ARM parity checks
  pass. It has not been installed or launched.
- Historical v3 package (2026-09-25): `mod/build/DLS18_career_market_v3.apk` (market v3, save version 0xB3).
- Previous build (2026-09-25 07:11): `mod/build/DLS18_career_market_ui_alpha_fixed2.apk`. It's the other session's
  `career_market_ui_alpha` plus the fixes from rules 3, 4 and 5. It boots and plays matches on the tablet. On the first launch it
  crashed once in `CFEHeaderMenu::RenderText` → `FTTFont_GetUnicodeTextDimensions` (null text, 4 s after launch). That didn't
  happen again after the restart. If it comes back, check whether the market UI hands the header menu a null string.
- Don't install `DLS18_animation_rework.apk`, `DLS18_dribble_animation_rework.apk`,
  `DLS18_skill_tiered_dribble.apk` (v1), `_v9.apk`, `DLS18_career_market_ui_alpha.apk` or
  `DLS18_career_market_ui_alpha_fixed.apk`. Each has at least one of these crash bugs.
  `DLS18_career_market_alpha.apk` and v2–v8 weren't checked.
