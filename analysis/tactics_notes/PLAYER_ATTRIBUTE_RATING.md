# Player attributes and player ratings

## Two different meanings of “rating”

The native library has a player OVR function and a separate match-performance rating path. OVR is a position-weighted summary of a player's stored attributes. Match performance is assembled from match-stat events. They answer different questions: a high OVR player can have a poor match, and the OVR does not rise just because the player made a run.

## How player OVR is calculated

PU_GetPlayerRating at 0x2b35d0:

1. Reads the player's position and maps it through PU_GetPlayerFEPos.
2. Uses the resulting position category to select a position-specific row of weights.
3. Accumulates 13 numeric player fields against that row.
4. Scales the result and clamps the output to 0–100.

PU_GetPlayerPreciseRating uses the same position-specific approach but returns finer precision. The native stat accessor functions identify the 13 fields used by the rating calculation. These offsets are relative to the TPlayerInfo structure, not the start of the compressed database file. The stored 16-bit values are divided by 10 by the accessors.

| TPlayerInfo offset | Attribute |
|---:|---|
| +0x88 | Acceleration |
| +0x8a | Speed |
| +0x8c | Stamina |
| +0x8e | Strength |
| +0x90 | Tackling |
| +0x92 | Ball control |
| +0x94 | Shooting |
| +0x96 | Crossing |
| +0x98 | Passing |
| +0x9a | Heading |
| +0x9c | Goalkeeper shot stopping |
| +0x9e | Goalkeeper handling |
| +0xa0 | Presence |

The OVR routine weights these 13 fields differently for each position category. The exact per-position coefficient matrix has not been decoded, so this report identifies all inputs but does not claim a numeric contribution for each one.

The player-card detail ratings are also explicit native calculations. Each is an arithmetic mean of three stored attributes:

| Detail rating | Attributes averaged |
|---|---|
| Fitness | Speed, acceleration, stamina |
| Keeping | Shot stopping, handling, presence |
| Passing | Passing, heading, crossing |
| Attacking | Shooting, strength, heading |
| Defensive | Strength, tackling, heading |
| Technique | Passing, ball control, crossing |

The free-kick stat accessor is derived as the mean of crossing, shooting, and passing; it is not a fourteenth stored field in the OVR input set.

The player database layout is documented in [DATABASE_LAYOUT.md](../../DATABASE_LAYOUT.md). `catalog/db_records.jsonl` preserves each raw record and its aligned numeric interpretations. Candidate player names and record offsets are in `catalog/db_record_index.csv`.

## Attribute-to-play links found in the native code

The original binary contains CPlayer::AttributeInterpolate_Internal, which maps stored stats into gameplay values. The audit at [analysis/stat_audit.csv](../stat_audit.csv) lists the stock call sites. The player utilities expose the fields in the table above plus derived passing, fitness, keeping, attacking, defensive, and technique detail ratings.

| Attribute or group | Gameplay use visible in code | Tactical effect |
|---|---|---|
| Speed | Average run speed, interception run speed, sprint speed, and ball-carry speed. | Faster players reach a loose ball or open channel sooner; speed matters most once the route is clear. |
| Acceleration | CPlayer::UpdateUrgency raises or lowers movement urgency by a stat-dependent step. | A quick accelerator reaches jog/sprint pace sooner; a high top speed with weak acceleration takes longer to become useful. |
| Stamina | CPlayer::UpdateSprint drains and restores a bounded stamina pool. | Repeated sprints weaken later transitions; use pace selectively and substitute tired runners. |
| Ball control | Run speed with the ball, first-touch/take-ball checks, and dribble-touch strength. | Better control supports tighter carries and cleaner receptions under pressure. |
| Passing and crossing | Kick error and assist paths vary by kick type; the stat audit shows separate call sites for passing and longer/crossing actions. | Use higher-passing players to start combinations and higher-crossing players to deliver from wide positions. Exact per-kick weighting is not fully decoded. |
| Shooting | Shot error range, shot placement chance, and shot assist. | Shooting quality matters after creating a clean angle; do not expect a poor shooter to gain accuracy from a high team OVR alone. |
| Tackling | Tackle reach/action interpolation and goalkeeper/field action call sites. | Standing-tackle chance depends on tackling versus carrier control, includes half the strength difference, and gains 20 percentage points when the defender faces the carrier within 45° at contact. The slide-tackle roll remains separate and has no facing bonus. |
| Strength | Body-contact and collision paths. | Strength affects whether a player holds space in a shoulder-to-shoulder contest. |
| Goalkeeper attributes | Shot stopping, handling, and reaction/positioning-related code paths. | Keeper quality affects saves and claim/handling situations; goalkeeper OVR is not a substitute for checking its keeper-specific profile. |
| Heading and presence | Named player-stat accessors exist; match/player-stat paths consume them. | Useful for aerial or box play, but this audit does not assign every situation or weight to these fields. |

The mapping above separates named native accessors from tactical interpretation. It does not claim every attribute has a one-to-one visible effect in every animation.

## Stock versus the current mod

The current patch widens gameplay stat differences around a pivot of 65. Its patcher changes gameplay interpolation, sprint/close-control movement, shot and pass error, tackling, collision force, stamina drain/recovery, and animation transition blending. The mod notes list the complete stock-to-mod ranges in [mod/README.md](../../mod/README.md).

Examples from that report:

| Gameplay value | Stock range (weak → strong) | Current mod range |
|---|---:|---:|
| Jog / average run speed | 3204 → 3738 | 3050 → 3850 |
| Sprint speed | 3738 → 4539 | 3650 → 4950 |
| Urgency increase step | 13 → 19 | 10 → 24 |
| Speed with the ball | 870 → 990 | 800 → 1010 |
| Shot placement chance | 33 → 66 | 20 → 80 |
| Shot assist | −60 → 80 | −80 → 96 |
| Tackle reach | 1638 → 2048 | 1400 → 2150 |

These are gameplay interpolation values from the patch notes, not player OVR values. The patcher does not rewrite `players.dat`, `teams.dat`, PU_GetPlayerRating, or the team-rating routine. Therefore, the current mod changes how strongly player quality is felt during play while leaving the stored player attributes and computed squad OVR/star calculation intact.

The current mod also adds direct sprint and hold-to-close-control input. Close control lowers movement urgency and scales down dribble-touch strength; sprint raises urgency and continues to use the game's stamina system. See the installed control model in [mod/README.md](../../mod/README.md).

## Choosing players for tactical jobs

The recommendations below are tactical inference based on the attribute call sites and run/formation systems:

- Use high speed and acceleration for wide channels, recovery runs, and attackers asked to run behind.
- Use stamina for players expected to repeat full-length runs or cover the outside of a three-player back line.
- Use ball control and passing for central receivers who will turn under pressure.
- Use shooting for the player expected to finish the move; the nearest/highest OVR player is not automatically the best finisher.
- Use tackling and strength for the player asked to close down or hold a defensive lane.
- Judge goalkeeper handling and shot stopping separately from outfield speed or passing.

## Limits

- The position-specific OVR weight table has not been fully assigned to user-facing stat names.
- The native accessor list is richer than the short stat interpolation audit; some fields appear in player cards or match stats without a decoded gameplay interpolation call.
- The post-match/player-performance score has its own code path and is discussed separately in [Squad rating, selection, and match grades](TEAM_RATING_AND_MATCH_GRADES.md).
