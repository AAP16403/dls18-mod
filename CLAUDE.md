# DLS18 modding project

- Before editing the assembly in `mod/build_mod.py` (the code cave and its hooks) or the game addresses in
  `mod/career_market/native_bridge.c`, read [mod/PATCHING_RULES.md](mod/PATCHING_RULES.md). Four builds crashed on the
  tablet because of absolute jumps, mis-encoded far conditional branches, lines keystone silently dropped, and wrong
  Thumb bits or data addresses.
- Career market: read `mod/career_market/BALANCE.md` first. Test changes with
  `wsl -e sh mod/career_market/tests/run_tests.sh` and `mod/career_market/tests/arm_crosscheck.py`. Don't run or install the
  game unless the user asks.
- Keep the branch guards in `build_mod.py`. If a guard fires, fix the assembly.
- More than one agent may work in this folder at once. Write new build outputs under new file names, and re-read
  a file before editing it.
