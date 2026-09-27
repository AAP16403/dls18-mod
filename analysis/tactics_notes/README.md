# DLS18 v5.064 tactics and ratings research

This folder explains how the extracted build represents formations, AI movement into space, player attributes, squad ratings, and match-performance ratings. It combines static evidence from the original ARMv7 game library with the behavior documented for the locally built sprint and close-control mod.

## Reports

- [Runs, space, formations, and team behavior](RUNS_SPACE_FORMATIONS.md)
- [Player attributes and player ratings](PLAYER_ATTRIBUTE_RATING.md)
- [Squad rating, selection, and match grades](TEAM_RATING_AND_MATCH_GRADES.md)
- [Practical tactical playbook](TACTICAL_PLAYBOOK.md)

## Evidence labels

- **Code evidence** means a native symbol, disassembly path, or data table was found in the extracted DLS18 v5.064 files.
- **Mod evidence** means the behavior is described by the current patcher and build notes in [mod/README.md](../../mod/README.md).
- **Tactical inference** means a practical football interpretation of those mechanics. It is advice, not a claim that the game engine guarantees a particular run or result.
- **Unresolved** marks a field or behavior whose exact meaning has not been decoded.

Native addresses in these reports are ELF virtual addresses in the original library at `unpacked/apk/lib/armeabi-v7a/libDLS18.so`. The original library and data are preserved. The current mod changes controller sprinting, close control, and several gameplay stat curves; it does not edit the player database, formation tables, player OVR calculation, squad-rating calculation, or star thresholds.

This is a static analysis report. The reports do not claim that every AI branch, rating contribution, or formation behavior has been checked in a live match. See each document for the specific verification boundary.

## Source files

- `unpacked/apk/lib/armeabi-v7a/libDLS18.so`: native match, player, AI, and rating code.
- `unpacked/apk/assets/data/db/` and `unpacked/db_decoded/`: compressed and decoded player, team, and roster-link records.
- `catalog/db_records.jsonl` and `catalog/db_record_index.csv`: decoded record views and candidate player identities.
- `analysis/stat_audit.csv`: stock stat-interpolation call sites and numeric ranges.
- `analysis/armdis.py` and `analysis/xref.py`: the symbol-aware Thumb disassembler and direct-call scan used to verify function addresses and call order.
- `mod/build_mod.py`, `mod/test_patch.py`, and `mod/README.md`: current mod changes and build notes.
- Formation tables are read from `FS_iFormationInfo`, `FS_iFormationPlayerPos`, and `FS_iFormationFEPlayerPos`; the reports distinguish match-position IDs from front-end position IDs and pitch-widget coordinates.
- Philosophy names and option conversions are read from `CTeamTactics` native code. The reports include the exact stored values and separate the unproven link to the aggressive-run scalar.
