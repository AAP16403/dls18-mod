# Squad rating, selection, and match grades

## Squad OVR and positional area scores

CDataBase::CalculateTeamRating at 0x20b71c expands a team's player records, orders the roster through PU_InsertionSortTPlayerInfo, processes no more than 18 entries, calculates each included player's OVR, and accumulates an overall score plus three area scores. It stores:

- overall team rating at team-record offset +0x08;
- three area ratings at +0x0c, +0x10, and +0x14.

The four general-position IDs are routed into three area buckets: IDs 0 and 1 share a bucket, ID 2 has its own, and ID 3 has its own. In the game's general-position scheme this is consistent with keeper/defence, midfield, and forward areas. CDataBase::GetTeamWeakestArea at 0x20c470 compares the three stored area ratings and returns the weakest area.

### Exact accumulation recovered from the routine

The path at `CDataBase::CalculateTeamRating` (`0x20b71c`) is more specific than a general “squad average” description:

1. `ExpandTeam` expands player records; the expanded count is read from team-record byte `+0x148`.
2. `PU_InsertionSortTPlayerInfo` sorts those records; the processed count is capped at `0x12` (18).
3. Each included `TPlayerInfo` record is `0xb0` bytes. The routine reads its position byte at `+0x82`, maps it with `PU_GetGeneralPosFromPos`, and calculates that player's OVR with `PU_GetPlayerRating`.
4. A four-way switch at `0x20b7bc` accumulates integer OVR sums and counts. Its table bytes at `0x20b7c0` are `02 02 05 08`: general-position IDs 0 and 1 go to the same bucket, ID 2 to the second, and ID 3 to the third.
5. Each area field is its bucket's sum divided by its count; an empty bucket is stored as zero. The overall field is the sum of all included OVRs divided by the processed count.

The write mapping is `team +0x14` = IDs 0/1 bucket, `+0x10` = ID 2 bucket, `+0x0c` = ID 3 bucket, and `+0x08` = overall. The engine's labels for the three general-position areas are not exported as names; “keeper/defence, midfield, forward” is an interpretation of the grouping, not a proven UI label mapping.

`CDataBase::GetTeamWeakestArea` (`0x20c470`) compares `+0x0c`, `+0x10`, and `+0x14`, writes the selected minimum rating through its output parameter, and writes area codes 3, 2, or 1 respectively. Ties prefer code 3, then code 2, then code 1. This confirms the raw area-code mapping without assuming localized labels.

The roster sorter receives an indirect comparator pointer. Its exact callback name is not exported in this build; the team-rating context strongly suggests the included 18 are the highest-rated available players, but that ordering detail is not treated as fully proven here. What is directly proven is that the calculation sorts, caps the list at 18, groups included players by general position, and averages rating values into the stored fields.

### What changes each rating

- Improving one player's database stats can change that player's position-weighted OVR, then affect the team average if that player is included in the rating set.
- Improving players in the weakest area raises that area's score when their rating displaces or improves the relevant included entries.
- Changing formation alone does not add a direct OVR multiplier in the team-rating arithmetic. The formation data affects match shape; the rating routine uses expanded player records and their positions.
- The team OVR routine never reads a formation ID in this path: its inputs are the sorted player records, each player's position, and each player's position-weighted OVR. Formation counts are read by separate AI and match-performance routines.
- A formation or lineup change can still change which players are selected, how the lineup screen groups them, or which positions they occupy. That may change displayed lineup summaries without changing the underlying player data.
- Star rating is coarse. A small OVR increase may leave the visible half-star value unchanged until it crosses a threshold.

## Squad star thresholds

CDataBase::GetStarRatingByRating at 0x20bfc2 converts integer team OVR to half-star values:

| Team OVR | Displayed star value |
|---:|---:|
| 0–50 | 0.5 |
| 51–54 | 1.0 |
| 55–58 | 1.5 |
| 59–62 | 2.0 |
| 63–66 | 2.5 |
| 67–70 | 3.0 |
| 71–74 | 3.5 |
| 75–77 | 4.0 |
| 78–79 | 4.5 |
| 80+ | 5.0 |

These are observed code thresholds, not predictions of player-card star displays in every UI screen.

## Squad rating versus lineup summary

CTeamLineup::GetTeamStats at 0x2f1624 iterates the player IDs in the selected lineup, fetches their player records and OVRs, and accumulates separate general-position counts, totals, and minima. CTeamLineup::PlayerPositionSuitability, GetPreferredPlayer, and AdjustLineup are the related paths used when choosing or repairing a lineup.

This produces a useful distinction:

- **Squad/team OVR** is calculated from an expanded roster and stored on the team record.
- **Lineup summary** is calculated from the currently selected lineup and its represented player positions.
- **Formation shape** is selected in the tactics system and drives AI zones and team movement.

Do not read a formation graphic or lineup minimum as though it directly changed every player's base OVR.

## Position and fit

The player OVR function chooses its stat-weight profile from the player's position. The team rating function also converts player positions into general-position buckets. That makes position relevant to both a player number and team-area coverage. The formation slot tables use position-enum IDs, but this audit has not proven that merely moving a player to a visual formation slot rewrites the player's underlying database position.

Practical implication: fill a formation with players who can perform its jobs, but distinguish **role fit** from **base player rating**. A strong player may still be awkward in a job if the relevant pace, control, passing, tackling, strength, or keeper attributes are weak.

## Match-performance rating is separate

The binary also exposes STAT_PlayerGetRatingOverall at 0x2b730c, STAT_PlayerGetRating at 0x2b7748, and STAT_PlayerRating at 0x2b74e8. These are a separate match-stat/performance-rating path, not the static squad OVR function.

`STAT_PlayerRating` (`0x2b74e8`) explicitly queries `GAI_FORMATION_NUMSTATICDEF` and `GAI_FORMATION_NUMSTATICMID` near its entry (`0x2b74f4–0x2b7512`) before processing the player's match-stat record. The helper counts come from the active formation's static line counts. This proves that formation structure is used during the match-performance-rating calculation. The exact event-to-points mapping and final display path have not been fully decoded, so this report does not assign numeric match-grade bonuses to goals, passes, tackles, runs, or formation choices.

This distinction matters for modding:

- The current mod does not patch these match-performance rating functions.
- The current mod does not patch the base OVR or team-star functions.
- Formation may affect a match-performance calculation, but the size and direction of each effect remain unresolved.

## What this report does not claim

- It does not prove a chemistry system or a hidden formation multiplier on squad OVR.
- It does not identify the indirect roster-sort comparator with certainty.
- It does not fully explain player development, manager quality, or post-match rating awards.
- It does not claim that a higher team OVR guarantees better tactical movement or a win.
