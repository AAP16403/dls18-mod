# Runs, space, formations, and team behavior

## How to read this report

**Code evidence** below comes from the original DLS18 v5.064 ARMv7 library, not from a modern DLS build. **Tactical inference** turns the code structure into practical guidance; the live game still needs to confirm exact run timing and teammate choices.

## The team AI works in layers

The native symbol AITEAM_TeamProcess at 0x2a3440 is the team-level coordinator. Its call path includes possession checking, team-strategy updates, space evaluation, goalkeeper behavior, formation focal-point and zone updates, external team AI, ball avoidance, and individual-player processing. AITEAM_PlayerProcess at 0x2a3c84 then assigns a movement destination, facing direction, and urgency to AI-controlled players; it also calls the loose-ball behavior.

The formation pass is split into named operations:

- AITEAM_FormationProcess at 0x2a5ef2 sequences dynamic formation setup, dimensions, zone setup and adjustment, out-of-possession processing, a secondary formation pass, avoidance, and a final adjustment.
- AITEAM_FormationProcessOOP at 0x2a4c44 is the explicit out-of-possession branch.
- AITEAM_FormationBackLineCalculate, AITEAM_FormationMidLineCalculate, and AITEAM_FormationFrontLineCalculate calculate the three line groups.
- AITEAM_FormationDimensionsSet (`0x2a69c4`) is in the direct formation path and calls the back-, front-, and midfield-line calculators. AITEAM_FormationWidthCalculate (`0x2a668c`) exists and uses interpolated values plus corner/penalty-state checks, but no direct caller was found in the scanned call sites.
- AITEAM_FormationDynamicProcess at 0x2a6c00 contains special handling for dangerous set pieces and attacking throw-ins.
- AITEAM_MarkingProcess is called from the secondary formation pass, and AITEAM_PlayerLooseBallProcess is called from AITEAM_PlayerProcess. The named AITEAM_PlayerHoldingProcess and AITEAM_PlayerRunningProcess helpers exist, but their direct callers were not found in the scanned branch calls; possible indirect dispatch remains unresolved.

**Practical reading:** the formation is more than a static menu picture. It seeds line zones, and the AI adjusts positions and movement around possession, opposition, space, and special situations. It is still a set of game-specific zones; the native names do not prove modern tactical features such as coordinated pressing triggers or manual player instructions.

### Exact update path recovered from direct calls

The direct calls in `AITEAM_TeamProcess` (`0x2a3440`) establish this order, when its state guards permit the normal formation pass:

1. `AIGAME_CheckPossession` updates the team possession view.
2. `AITEAM_UpdateTeamStrategy` updates a team strategy record.
3. `AITEAM_EvaluateSpaceInfo` computes space information.
4. `GAI_GKProcess` handles the goalkeeper separately.
5. `AITEAM_FormationSetFocalPoint`, then `AITEAM_FormationProcess`, set and process the team's formation.
6. If the internal match-state field is `10`, `EX_ProcessTeamAI` is called; a false return skips the later player loop.
7. The team loop visits ten player slots. `AIGAME_AllowAIPlayerProcess` gates each call to `AITEAM_PlayerProcess`.

`AITEAM_PlayerProcess` (`0x2a3c84`) calls `AITEAM_PlayerLooseBallProcess`, then applies a movement destination, facing point, and urgency or urgency time. The code proves a ten-slot loop and a separate goalkeeper call; calling those ten slots “outfield players” is the likely interpretation, not a decoded slot-name table.

Inside `AITEAM_FormationProcess` (`0x2a5ef2`), direct calls occur in this order: dynamic setup → line dimensions → zone setup → zone adjustment → out-of-possession processing → secondary processing → avoidance → final adjustment. The secondary pass (`0x2a5b28`) calls dynamic-situation handling → set-piece handling → marking → run dispatch, then may add further zones. This is why the same formation table can feed different destinations in possession, out of possession, and special situations.

The run dispatcher (`AITEAM_RunningProcess`, `0x2a7408`) has a precise gate: its second argument must be nonzero, and a runtime scalar read from a global AI-state object at offset `+0xa654` must be at least `0x3d` (61). Only then does it call `AITEAM_AggressiveRunProcess` (`0x2a7434`). `CTeamTactics::SetPhilosophy` stores the selected value in the tactics object at byte `+1`; the dispatcher does not read that object directly. No direct data-flow path from the menu value to the `+0xa654` scalar has been established, so “Attacking enables aggressive runs” remains a plausible inference, not a proven one.

## How the engine evaluates space and runs

The space evaluator AITEAM_EvaluateSpaceInfo at 0x2a36ac calls GU_GetPlayerForwardBestSpace, angle blending, distance, and field-geometry math. The run destination routines add more checks:

- AITEAM_RunGetSeekSpaceDest at 0x2a7c8c checks nearby players, queries GM_GetPointSpace, introduces some random variation, and calls ACT_PassCheckPath. This is evidence that a seek-space destination considers both local room and whether a pass route is usable.
- AITEAM_SeekSpaceRunProcess at 0x2a793c adds the selected run to the formation zones, checks whether fullback support is allowed, and uses average run speed when setting up the run.
- AITEAM_RunGetAggressiveDest at 0x2a80c0 uses interpolated values, distances, nearest-player checks, and random variation to produce a more forward run destination.
- AITEAM_AggressiveRunProcess at 0x2a7434 resets run state, requests aggressive destinations, and adds eligible destinations to the formation zones.
- AITEAM_RunningProcess at 0x2a7408 calls the aggressive-run path only when its second argument is nonzero and the runtime scalar at global offset +0xa654 is at least 61 (decimal). This scalar is not read directly from the CTeamTactics philosophy byte. A link from Defensive/Moderate/Attacking to this gate remains unproven.

The direct-call evidence distinguishes the two run families: `AITEAM_RunningProcess` directly calls the aggressive-run process under the gate above, while `AITEAM_SeekSpaceRunProcess` (`0x2a793c`) directly calls `AITEAM_RunGetSeekSpaceDest` (`0x2a7c8c`). No direct caller for `AITEAM_SeekSpaceRunProcess` was found in the analyzed library's `.text`; indirect or other-module calls are not ruled out. Its runtime invocation schedule is unresolved. The seek-space destination routine directly checks AI-player eligibility, calls `AITEAM_FullBackSupportAllowed`, samples nearby-player geometry with `GM_GetPlayerNearestPointFX`, queries `GM_GetPointSpace`, adds random variation, and checks a candidate route with `ACT_PassCheckPath`. These calls prove that the tests exist, not the probability or timing of a run in a match.

These are AI-selected teammate movements. The named routines do not add a dedicated “send runner” button for the human player. The controlled player is moved through the controller path; AI teammates select their own destinations.

## Formations present in this build

The native formation-name table contains 12 options. FS_iFormationInfo supplies three line counts and a fourth raw value; CTeamTactics::GetFormation returns the first three fields. The fourth field is included below for reproducibility, but its meaning is unresolved.

| Index | Menu label | Line counts in native table | Fourth raw value |
|---:|---|---:|---:|
| 0 | 4-4-2 | 4 / 4 / 2 | 0 |
| 1 | 4-1-2-1-2 | 4 / 4 / 2 | 2 |
| 2 | 4-3-1-2 | 4 / 4 / 2 | 16 |
| 3 | 4-5-1 | 4 / 5 / 1 | 8 |
| 4 | 4-1-4-1 | 4 / 5 / 1 | 4 |
| 5 | 4-4-1-1 | 4 / 5 / 1 | 32 |
| 6 | 4-3-3 | 4 / 3 / 3 | 0 |
| 7 | 4-1-2-3 | 4 / 3 / 3 | 4 |
| 8 | 5-3-2 | 5 / 3 / 2 | 1 |
| 9 | 5-2-1-2 | 5 / 3 / 2 | 9 |
| 10 | 3-4-3 | 3 / 4 / 3 | 16 |
| 11 | 3-5-2 | 3 / 5 / 2 | 8 |

The eleven slot IDs per formation are stored in FS_iFormationPlayerPos. They are native position-enum values, not decoded coordinates. The arrays are:

| Index | Eleven slot IDs, in stored order |
|---:|---|
| 0 | 0, 1, 5, 7, 2, 17, 12, 13, 16, 20, 21 |
| 1 | 0, 1, 5, 7, 2, 17, 8, 18, 16, 20, 21 |
| 2 | 0, 1, 5, 7, 2, 12, 11, 18, 13, 20, 21 |
| 3 | 0, 1, 5, 7, 2, 17, 10, 18, 9, 16, 19 |
| 4 | 0, 1, 5, 7, 2, 17, 12, 8, 13, 16, 19 |
| 5 | 0, 1, 5, 7, 2, 17, 12, 18, 13, 16, 19 |
| 6 | 0, 1, 5, 7, 2, 12, 11, 13, 20, 19, 21 |
| 7 | 0, 1, 5, 7, 2, 12, 8, 13, 20, 19, 21 |
| 8 | 0, 1, 5, 6, 7, 2, 12, 11, 13, 20, 21 |
| 9 | 0, 1, 5, 6, 7, 2, 12, 18, 13, 20, 21 |
| 10 | 0, 5, 6, 7, 17, 12, 13, 16, 20, 19, 21 |
| 11 | 0, 5, 6, 7, 17, 10, 18, 9, 16, 20, 21 |

The three formation arrays are separate native symbols:

| Symbol | Address / size | What the code establishes |
|---|---:|---|
| `FS_iFormationInfo` | `0x63a49c` / `0xc0` | Twelve 16-byte records. `CTeamTactics::GetFormation` (`0x2f1bbc`) returns words 0, 1, and 2 as the three line counts; it does not return the fourth word. |
| `FS_iFormationPlayerPos` | `0x63a55c` / `0x210` | Twelve rows of eleven native `EPlayerPosition` IDs, one per formation slot. |
| `FS_iFormationFEPlayerPos` | `0x63a76c` / `0x210` | Twelve rows of eleven front-end position IDs. The table has the same dimensions, but its slot-by-slot relationship to `FS_iFormationPlayerPos` is not established. |

`PU_GetPlayerPosFromFEPos` (`0x2b3370`) and `PU_GetPlayerFEPos` (`0x2b3388`) convert between the two position enums. The decoded FE-to-player-position table at `0x63b190` is `0, 1, 2, 6, 8, 11, 18, 17, 16, 20, 19` for FE IDs `0–10`. The reverse map at `0x63b1c0` is many-to-one: for example, native position IDs 5, 6, and 7 all map to FE category 3, while FE category 3 converts back to canonical position ID 6. A comparison of the stored formation rows also shows that not every FE-table slot equals the conversion of the same-index match-position slot. Therefore `FS_iFormationFEPlayerPos` should be treated as a separate front-end table until its consumer and row mapping are traced; its IDs are not reliable names for match roles. Reliable role names for IDs beyond the keeper slot have not been established here. The front-end pitch widget calls `CFETMPitch::GetFormationTPoint` (`0x243bf8`) from `CFEFormationPitch::RenderUp` (`0x22f4e8`). That routine reads two floats per formation slot using a formation stride of `0x58` bytes and slot stride of 8 bytes, then scales them to the widget dimensions. The coordinate pointer is loaded through the GOT, but its target data source was not decoded. These are presentation coordinates, not measured match-AI coordinates.

The line counts also have active code consumers. `GAI_FORMATION_NUMSTATICDEF/MID/FOR` (`0x2a5f6c`, `0x2a5fe0`, `0x2a6044`) read the three counts, while `GAI_FORMATION_ISSTATICDEF/MID/FOR` (`0x2a5f3c`, `0x2a5f94`, `0x2a600c`) classify slots against those line boundaries. `AITEAM_FormationDynamicSet` (`0x2a6ab8`) calls these helpers when setting per-player formation state. Separately, `STAT_PlayerRating` (`0x2b74e8`) reads the static-defender and static-midfielder counts. This is the concrete route by which the selected shape affects AI grouping and a match-performance-rating path; it is not a direct modifier to base player OVR or squad stars.

The stored positions are sufficient to prove the available formation shapes and their line counts. They do not, by themselves, prove exact player coordinates, run tendencies, or which flank advances in every match state.

## Philosophy choices

CTeamTactics::GetPhilosophyName and its option conversion expose three menu values:

| Menu option | Stored value | Menu label |
|---:|---:|---|
| 0 | 0 | Defensive |
| 1 | 50 | Moderate |
| 2 | 100 | Attacking |

`CTeamTactics::GetPhilosophyName` (`0x2f1be8`) names exactly these three stored values; the option converters (`0x2f1c48`, `0x2f1c5e`) map menu indices 0, 1, and 2 to 0, 50, and 100. The setter (`0x2f1ba2`) changes values above 100 to 0. The aggressive-run threshold is a separate runtime read; the current trace does not prove that any of these three choices writes it. No philosophy adjustment appears in the base team-rating routine described in [Squad rating, selection, and match grades](TEAM_RATING_AND_MATCH_GRADES.md).

## Tactical implications of each shape

The game-data table proves the line counts. The following strengths and risks are **tactical inference**, not a measured AI guarantee.

| Shape family | What the shape gives you | What to protect |
|---|---|---|
| 4-4-2 | Two forward outlets and two four-player lines; useful for direct passes and second-ball support. | Avoid leaving the two central midfielders to cover too much width alone. |
| 4-1-2-1-2 / 4-3-1-2 | Central passing triangles and two forwards; the diamond-like variants can keep combinations close together. | The middle is crowded; use the fullbacks or switch the play when the centre is blocked. |
| 4-5-1 / 4-1-4-1 / 4-4-1-1 | More midfield cover around a single main striker; good for compact possession and protecting a lead. | The striker can become isolated; move a midfielder forward before asking for a long sprint. |
| 4-3-3 / 4-1-2-3 | Three forwards can stretch a back line and create wide and central forward lanes. | Keep enough midfield cover behind the ball, especially after a failed forward pass. |
| 5-3-2 / 5-2-1-2 | A five-player defensive line with two forwards; can keep a deeper block while retaining outlets. | Width on transitions depends on wide players leaving the back line; stamina and pace matter. |
| 3-4-3 / 3-5-2 | Three defenders with a larger midfield or forward line; can put more players near the ball. | The channels outside the three defenders need cover when wide players advance. |

## Controls in the current installed mod

The stock v5.064 sprint logic waits for a sustained directional hold (about 1.5 seconds, as described by the patch analysis). The currently built mod replaces that with analog sprint:

- Push the floating joystick to at least 87.5% of its ring to start sprinting.
- Ease back below 78% to stop sprinting; the gap prevents rapid on/off flicker.
- Sprint start is blocked below the game's 25% stamina threshold. A sprint already active can continue until stamina is empty.
- Hold a free finger on the right side while dribbling for close control. It slows the player and shortens touches. Keep that finger off HUD buttons; a swipe remains a skill move.

These inputs and thresholds are **Mod evidence**, not the stock DLS18 controls. Full details are in [mod/README.md](../../mod/README.md).

## What this means in play

- Use seek-space and forward-pass lanes to move the opponent before forcing the final pass. The engine explicitly has free-space and pass-path queries.
- Save full sprint for a run into open grass, a recovery chase, or a decisive transition. Jogging keeps the ball-carrier controllable; close control is for crowded spaces.
- In a narrow shape, move the ball wide before trying to force a central dribble. In a wide shape, use a central pass once the opposing line shifts out.
- While defending, prioritize the line and passing lane first; accelerate into the ball only when the channel closes. This advice is based on the formation/marking structure, not a documented defensive “press” command.

## Unverified behavior

- Reliable role names for native position IDs and the runtime coordinate table used by the front-end formation pitch.
- The exact data field that carries the menu philosophy value into the aggressive-run scalar.
- The normal invocation schedule for AITEAM_SeekSpaceRunProcess; its destination logic is present, but no direct caller was found in the scanned branch calls.
- Runtime run likelihood by formation, player, score, or match minute.
- Whether the opponent AI uses identical thresholds to the user-controlled team's AI.
- No live-match A/B test was run for this report.
