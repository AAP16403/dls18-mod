# DLS18 tactical playbook

This is a practical guide for the DLS18 v5.064 build with the current sprint and close-control mod installed. It applies the evidence in [Runs, space, formations, and team behavior](RUNS_SPACE_FORMATIONS.md), [Player attributes and player ratings](PLAYER_ATTRIBUTE_RATING.md), and [Squad rating, selection, and match grades](TEAM_RATING_AND_MATCH_GRADES.md). Recommendations marked **tactical inference** are guidance, not guaranteed engine outcomes.

## Build the team around the weakest area

Start with the three team-area scores, not just the headline OVR or stars:

1. Check the three area scores and identify the lowest. The code returns raw area IDs; “keeper/defence,” “midfield,” and “forward” are interpretations of those groups, not confirmed UI labels.
2. Decide whether the selected formation asks that area to do extra work.
3. Use your best players in the jobs the shape depends on: pace for channel runs, stamina for repeated wide recovery, control for crowded receptions, passing for distribution, tackling/strength for defensive duels, shooting for the finisher.
4. Recheck the star threshold only after the OVR changes. A one-point increase may not cross a half-star boundary.

**Tactical inference:** if your forwards are much stronger than midfield, a two-striker formation can use the strength you have, but you still need a reliable passer to connect the lines. If the defence is the weak area, use a shape with more defensive cover and avoid pulling a centre-back out to chase the ball.

## Choose a shape for the kind of space you want

| If you want… | Try… | How to play it |
|---|---|---|
| Two nearby forward targets | 4-4-2, 4-1-2-1-2, 4-3-1-2, 5-3-2, or 5-2-1-2 | Use short support passes to the first forward, then look for the second forward or a runner into open space. |
| A central overload | 4-1-2-1-2 or 4-3-1-2 | Keep the ball moving through midfield. If the centre is crowded, switch to a fullback or wide lane before re-entering. |
| Width and three forward lanes | 4-3-3, 4-1-2-3, or 3-4-3 | Stretch the opponent first, then pass inside when a defender steps toward the wide player. |
| More midfield cover around one striker | 4-5-1, 4-1-4-1, or 4-4-1-1 | Let a midfielder support the striker before playing the final ball. Avoid sending the striker alone into a sprint with no passing option. |
| A deeper block with two outlets | 5-3-2 or 5-2-1-2 | Defend the central lane, then use the first safe pass to reach the forwards or the wide runners. |
| Three defenders and a larger midfield | 3-5-2 | Keep the wide midfielders fresh enough to recover; do not let both wide channels become open at once. |

The formation names and line counts are verified in the native table. The play patterns in this table are tactical inference.

## Run timing with the current controls

### With the ball

1. **Jog while scanning.** Medium joystick travel gives normal run pace in the mod. It leaves time to turn, pass, or protect the ball.
2. **Use close control near a defender.** Hold a free finger on the right side, away from HUD buttons. The mod slows the carry and shortens touches; use it to turn or keep the ball in a crowded channel.
3. **Move the defence, then attack a gap.** Pass to the free side or use a short combination to make an opponent shift. The engine has space and pass-path checks for AI runs; use open lanes instead of forcing a through pass into a crowded line.
4. **Sprint only into the gap.** Push the stick close to the edge to start sprinting. Release below the lower threshold when the lane closes or the receiver needs to control the ball.
5. **Finish with the right player.** A high OVR striker can still miss more often in the current mod if the shooting attribute is low; create a cleaner angle or pass to a better finisher.

### Without the ball

1. Use normal pace to hold a passing lane and keep the defensive line intact.
2. Close down with sprint when the opponent has a heavy touch, faces away, or has limited options.
3. For a standing tackle, square the defender toward the carrier before contact. The current mod adds 20 percentage points when the defender is facing within 45°; it does not guarantee a win.
4. Use a controlled jog to track a runner rather than following the ball and opening a second lane.
5. After winning the ball, look for a forward or wide outlet before using full sprint.

The slow right-side hold also sets controlled urgency without possession in this mod, which can help with positioning. It is not a documented second-defender press command.

## Select the player for the action

| Situation | Attribute priority | Why |
|---|---|---|
| Run behind the line | Speed, acceleration, stamina | Top speed only helps if the player can accelerate into the lane and repeat the effort. |
| Receive and turn centrally | Ball control, acceleration, passing | The receiver needs a manageable first touch and a quick release option. |
| Switch play or cross | Passing/crossing and enough stamina to reach the flank | The game has different kick-error paths; use a player suited to the delivery. |
| Finish a move | Shooting, then enough control to set up the shot | OVR is position-weighted, so a high overall number does not guarantee finishing quality. |
| Close down a carrier | Acceleration, tackling, stamina | Arrive under control so a challenge is timed and the player can recover. |
| Hold a defensive lane | Tackling, strength, stamina | The player needs to contest contact and remain available through repeated transitions. |
| Goalkeeping | Keeper-specific handling and shot-stopping fields | Outfield stats do not describe keeper performance. |

## Manage stamina as a team resource

The current mod still uses the game's stamina pool. A player with low stamina cannot start a new sprint below the threshold; sprint drain and recovery also depend on stamina. Rotate wide players and high-pressing players first if they are repeatedly covering the longest distances.

**Tactical inference:** keep the fastest player for the run where top speed creates separation, and use a high-acceleration player to press short distances. Do not spend both efforts sprinting after a pass that has already left the player's reach.

## Formation, philosophy, and rating

- Formation changes the AI's line counts and space zones. It does not directly change base OVR in the team-rating arithmetic.
- Defensive, Moderate, and Attacking are the three native philosophy labels. The aggressive-run dispatcher reads a separate runtime scalar; the link from the menu choice to that scalar remains unproven.
- Formation-aware match-stat code exists, so a match grade may depend on a player's tactical group. Exact scoring weights are unresolved.
- The current mod retunes gameplay stat effects but does not change player database values or squad-rating code. Your displayed base team rating should therefore remain the v5.064 value while the in-match feel changes.

## Simple match plan

1. **First few possessions:** jog and use short passes to find the opponent's weak side.
2. **When a defender steps out:** use close control to protect the ball or pass into the space behind that defender.
3. **When a runner has a clear lane:** play the forward ball, then sprint to meet it; avoid full sprint while dribbling through a crowd.
4. **When defending a lead:** choose a more covered shape, keep midfielders in their lanes, and sprint only to close a real threat.
5. **When chasing a goal:** choose a more forward shape and attacking philosophy, but preserve one reliable defensive outlet to cover a turnover.

## How to validate the advice in your own save

No live-match comparison was run while writing these reports. To check a tactical change fairly, keep the roster and opponent difficulty the same, change only one formation or philosophy setting, and compare several matches. Note:

- successful passes into space and through balls;
- turnovers caused by crowded carries;
- counterattacks conceded through wide channels;
- stamina remaining for wingers and midfielders;
- whether the weakest area score is reflected in the roles that struggle.

Treat a single match as a useful example, not proof that one formation is always better.
