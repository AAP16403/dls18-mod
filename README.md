# DLS18 mod

A mod for Dream League Soccer 2018 (Android, version 5.064, armeabi-v7a). It patches the game library
and adds a native companion library:

- **Career transfer market** (`mod/career_market`): an AI transfer market between clubs, club finances,
  negotiations with rivals, contracts and wages. From v37 it has its own transfer screen
  (`tm_screen.c`), drawn by the mod and designed as an HTML prototype first
  (`mod/career_market/design/transfer_v2`).
- **Gameplay and stats** (`mod/build_mod.py` and the modules next to it): a continuous stat rework,
  fair AI (no penalty cheating or difficulty rubber band), squad 64 and created-player limits, and DLS26
  animations ported and distributed by player ability (`anim_fit.py`, `anim_motion.py`, `anim_guard.py`).

All stat effects are continuous in the stat: no hard cut-offs.

## What is not in this repository

The game itself. No game binaries, APKs, animation packs, extracted assets or game data are included;
they belong to First Touch Games. To build, you need your own copy of the game:

1. Put the stock `libDLS18.so` (5.064) and the base APK where `mod/build_mod.py` and `mod/build_apk.py`
   expect them (see `mod/README.md` and `FILE_MAP.md`).
2. `python mod/build_mod.py --anim-count 2561 --out <libDLS18.so>` builds the patched game library.
3. `wsl -e sh mod/career_market/build_native_bridge.sh <libCareerMarket.so>` builds the market library
   (arm-linux-gnueabi-gcc).
4. `python mod/build_apk.py ...` packs and signs an APK with your own key.

## Tests

- `python mod/test_patch.py <lib>` and `python mod/career_market/tests/test_hooks.py <lib>`: patch and hook checks, in emulation.
- `wsl -e sh mod/career_market/tests/run_tests.sh`: the market simulation harness. It needs a dataset
  generated from your game with `make_dataset.py`.
- `python mod/career_market/tests/arm_crosscheck.py <libCareerMarket.so> <dataset>`: runs the ARM build under Unicorn.
- `python mod/career_market/tests/tm2_smoke.py ...`: drives the new transfer screen under Unicorn.

Read `mod/PATCHING_RULES.md` before changing any assembly.
