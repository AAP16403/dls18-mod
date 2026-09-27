"""Convert data/dls18_dataset.json (real DLS18 teams/players) into the harness text format.

  T <team_id> <valid_search_team> <kind> <n> <player ids...>
      kind: 0 club league, 1 relegation league, 2 international, 3 miscellaneous, 4 unassigned,
            +8 when the team is a classic side
  P <player_id> <gen_pos 0..3> <rating> <age> <value>
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
data = json.loads((HERE.parent / "data" / "dls18_dataset.json").read_text(encoding="utf-8"))
KIND = {"club_league": 0, "relegation_league": 1, "international": 2, "miscellaneous": 3, "unassigned": 4}
# The dataset's team 0x102 is an all-star side; a career starts with a weak dream team. Build a
# realistic starting squad from unattached players rated 55-64 (2 GK, 6 DEF, 7 MID, 4 FWD).
on_roster = {pid for t in data["teams"] for pid in t["roster"]}
free = sorted((p for p in data["players"] if p["id"] not in on_roster and 55 <= p["rating"] <= 64),
              key=lambda p: (p["rating"], p["id"]))
user_squad = []
for pos, n in ((0, 2), (1, 6), (2, 7), (3, 4)):
    user_squad += [p["id"] for p in free if p["gen_pos"] == pos][:n]
for t in data["teams"]:
    if t["team_id"] == 0x102:
        t["roster"] = user_squad
out = []
for t in data["teams"]:
    kind = KIND[t["category"]] + (8 if t.get("classic") else 0)
    valid = 1 if t["market_search_team"] else 0
    out.append(f"T {t['team_id']} {valid} {kind} {len(t['roster'])} " + " ".join(map(str, t["roster"])))
for p in data["players"]:
    out.append(f"P {p['id']} {p['gen_pos']} {p['rating']} {p.get('age_2018_07_01') or 0} {p['value']}")
dest = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "dls18.txt"
dest.write_text("\n".join(out) + "\n")
print(f"wrote {dest}: {len(data['teams'])} teams, {len(data['players'])} players")
