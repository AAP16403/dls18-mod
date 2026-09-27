"""Synthetic DLS-like dataset for the harness until the real extraction is available.

Line format consumed by harness.c:
  T <team_id> <valid> <tier> <n> <player ids...>
  P <player_id> <pos 0=GK 1=DEF 2=MID 3=FWD> <rating> <age> <value>
"""
import random
import sys

rng = random.Random(7)
out = []
pid = 1
teams = []
tiers = [(0, 20, 72), (1, 24, 64), (2, 24, 57), (3, 24, 50)]
tid = 1
for tier, count, mean in tiers:
    for _ in range(count):
        if tid == 0x102:
            tid += 1
        teams.append((tid, 1, tier, mean))
        tid += 1
teams.append((0x102, 0, 3, 48))          # user club (validity comes from USER_TEAM_ID)
for _ in range(10):
    teams.append((tid, 0, 9, 70)); tid += 1  # non-market teams (e.g. national sides)

players = []
for team_id, valid, tier, mean in teams:
    ids = []
    for pos, n in ((0, 3), (1, 7), (2, 8), (3, 4)):
        for _ in range(n):
            rating = max(30, min(95, int(rng.gauss(mean, 6))))
            age = rng.randint(18, 35)
            value = int(max(50, (rating - 30) ** 2.4 * (1.25 if age < 25 else 1.0 if age < 30 else 0.6)))
            players.append((pid, pos, rating, age, value))
            ids.append(pid)
            pid += 1
    out.append(f"T {team_id} {valid} {tier} {len(ids)} " + " ".join(map(str, ids)))
for p in players:
    out.append("P %d %d %d %d %d" % p)
open(sys.argv[1] if len(sys.argv) > 1 else "synthetic.txt", "w").write("\n".join(out) + "\n")
