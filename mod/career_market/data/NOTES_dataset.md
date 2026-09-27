# DLS18 dataset / value-model notes (work in progress)

## Market call conventions (from native_bridge.c, read-only)
- GetPlayerInfoSimple(info, player_id, 0, 0) @0x20da0c
- GetPlayerValue(info, -1, -1, 0, 1) @0x212b3c (result clamped >=0)
- PU_GetPlayerRating(info) @0x2b35d0 (clamped 0..100)
- position group = (int8)info[0x7f]
- GetTeamValueTotal(team_id) @0x20c3a0 used for opening account

## Findings so far (static)
- players.dat.decoded record (0xB4) == TPlayerROM verbatim; LoadPlayerROM 0x20c710 binary-searches u16 id at +0 over records at base+0xC.
- PlayerROMtoInfoSimple 0x20dabc copies ROM->Info (see TPlayerInfo table below). Stats stored x10 (e.g. 809 = 80.9).
- PU_GetPlayerRating 0x2b35d0: row = PU_GetPlayerFEPos(info[0x80]) (table @0x63b1c0), weights 13 x int @0x63af50 (rows sum 1000),
  rating = clamp(trunc((sum w_i*raw_i + 35000)/10000), 0, 100).
- GetPlayerValue 0x212b3c: CConfig vars (defaults in s_tConfigVarInfo @0x61b7d8, entry 0x108: name[0x100], default int @+0x100):
  Gk/Def/Mid/Att Min/MaxRating 40/100, MinValue 45/60/90/120, MaxValue 450/600/900/1200, ExpFactor 1 + 5/10 = 1.5.
  v = minV + (int)(powf((clamp(r)-minR)/(maxR-minR), 1.5) * (maxV-minV)); scouted: *(1+20/100); bRandom adds -v/100 + Random(v/50);
  RoundToNearest(v,5); secret-player-turn discount only if CTransfers::ms_bSecretPlayerTurn && last arg.
- GetTeamValueTotal 0x20c3a0: sum over link ids of GetPlayerValue(GetPlayerInfo(id, team, 1, NULL, 0, -1), -1, -1, 0, 1).
- CConfig vars actually used: bundled `assets/data/x_android/dls_config.dat` (u32-XOR key 0x53d392af via FTTDecode 0x3fa130, then zlib;
  CConfigFileInfo::GetXMLReader 0x215f30 falls back to it when no downloaded config). <Config><GameVariables><PlayerValues> (vars 0x15b..0x16d):
  Gk 51-100 rating -> 50..3125, Def 51-100 -> 75..3375, Mid 52-100 -> 100..3875, Att 52-100 -> 150..4125, ExpFactor 2 + 3/10 = 2.3.
  (A server-downloaded config could override at runtime; not checkable offline.)
- Team league/category byte: CTeam+5 = TTeamROM u32 @+4. IsTeamInternational: 9..13; IsTeamMiscellaneaous: 17;
  IsTeamInRelegationLeague: {18,19,20,21,22,23,25,26,28}; IsValidSearchTeam 0x210884: not user team, not 0x15c, not 0x1f8, league != 0xff.
- Team names: assets/data/text/ftsteamnames.xlc (FTTL), keys TXT_TEAMNAMELONG_<id>/MED/SHORT (GetTeamName 0x20c0c8).
- VERIFIED (Unicorn, dls_native_check.py): PU_GetPlayerRating + GetPlayerValue(-1,-1,0,1) match the Python model for all 5816
  players; GetPlayerValue sweep over genPos 0-3 x rating 0-110 matches for bundled and default configs; GetTeamValueTotal matches.
- XMATH_RoundToNearest(v,5) quirk: threshold is 5/2 == 2, so remainder 2 already rounds up (12 -> 15, 11 -> 10).
- TPlayerInfo+0x76 = nationality (ECountry s16): FindReplacementPlayer 0x209b30 compares ldrsh [info+0x76] with its ECountry arg.
- TPlayerInfo has NO age/birth-date field. TPlayerROM has birth day u32 @0xa8, month u32 @0xac, year u32 @0xb0 (Fabianski 18/4/1985) — not copied by PlayerROMtoInfo(Simple).
