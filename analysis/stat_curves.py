"""Print and validate the DLS18 gameplay stat curves and section 5 metrics."""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mod"))
import build_mod as bm  # noqa: E402


def strict(values, direction):
    deltas = [(b - a) * direction for a, b in zip(values, values[1:])]
    return bool(deltas) and all(delta > 0 for delta in deltas), min(deltas, default=0)


def stat_for_game(raw, knee):
    mapped = raw if raw >= 56 else math.floor(bm.stat_input_curve(raw, knee) + 0.5)
    return min(max(mapped, 40), 99)


def interpolate(raw, address, knee):
    _, _, low, high = bm.RETUNE[address]
    stat = stat_for_game(raw, knee)
    return low + (stat - 40) * (high - low) // 59


def table_probability(z2, slide, values, cfg):
    adjusted = z2 - (round(cfg.slide_shift * 2) if slide else 0)
    x = min(max(adjusted + 320, 0), 640)
    index, fraction = x >> 3, x & 7
    q = values[index]
    if index < 80:
        q += ((values[index + 1] - q) * fraction) >> 3
    scale = 21504 if slide else 23040
    base = 1024 if slide else 1280
    return (base + ((q * scale) >> 16)) / 256.0


def show_table(label, inputs, values, direction):
    ok, minimum = strict(values, direction)
    print(f"TABLE {label}: inputs={inputs}")
    print(f"  values={values}")
    print(f"  strictly_monotone={ok}; minimum_encoded_step={minimum}")
    return ok


def main():
    cfg = bm.build_parser().parse_args([])
    knee = cfg.stat_knee
    stat_inputs = tuple(range(0, 57, 4))
    stat_values = bm.stat_table_q8(knee)
    error_inputs = tuple(range(100))
    error_values = bm.error_decay_table_q15()
    duel_inputs = tuple(range(-160, 161, 4))
    duel_values = bm.duel_sigmoid_table_q16(cfg.duel_scale)
    foul_inputs = tuple(range(100))
    foul_values = bm.foul_table_q12(cfg.foul_softness)

    table_ok = True
    table_ok &= show_table("stat input Q8.8", stat_inputs, stat_values, 1)
    table_ok &= show_table("kick-error decay Q1.15", error_inputs, error_values, -1)
    table_ok &= show_table("duel sigmoid Q0.16", duel_inputs, duel_values, 1)
    table_ok &= show_table("foul factor Q4.12", foul_inputs, foul_values, -1)

    print("\nSECTION 5 METRICS")
    checks = {}
    slope_min = 1.0 / (1.0 + math.exp(20.0 / knee))
    stat_formula_strict = all(bm.stat_input_curve(s + 1, knee) > bm.stat_input_curve(s, knee)
                              for s in range(99))
    identity_above_56 = all(bm.stat_input_curve(s, knee) == s for s in range(56, 100))
    checks["input map: strictly increasing"] = stat_formula_strict
    checks["input map: minimum slope 20..40 >= 0.05"] = slope_min >= 0.05
    checks["input map: identity at and above 56"] = identity_above_56
    print(f"input map minimum slope on [20,40]: {slope_min:.6f} (target >=0.05)")
    print(f"input map identity at and above 56: {identity_above_56}")

    sprint_62 = interpolate(62, 0x2E1410, knee)
    sprint_85 = interpolate(85, 0x2E1410, knee)
    sprint_68 = interpolate(68, 0x2E1410, knee)
    sprint_80 = interpolate(80, 0x2E1410, knee)
    jog_62 = interpolate(62, 0x2E133E, knee)
    jog_85 = interpolate(85, 0x2E133E, knee)
    sprint_ratio = sprint_85 / sprint_62
    sprint_ratio_mid = sprint_80 / sprint_68
    jog_ratio = jog_85 / jog_62
    checks["sprint speed ratio 62/85 in 1.20..1.25"] = 1.20 <= sprint_ratio <= 1.25
    checks["sprint speed ratio 68/80 >=1.09"] = sprint_ratio_mid >= 1.09
    checks["jog speed ratio 62/85 in 1.15..1.20"] = 1.15 <= jog_ratio <= 1.20
    print(f"sprint speed ratio 62/85: {sprint_ratio:.3f} (target 1.20..1.25)")
    print(f"sprint speed ratio 68/80: {sprint_ratio_mid:.3f} (target >=1.09)")
    print(f"jog speed ratio 62/85: {jog_ratio:.3f} (target 1.15..1.20)")

    accel = {s: interpolate(s, 0x2E1798, knee) for s in (62, 85)}
    seconds = {s: 4096.0 / accel[s] / 60.0 for s in accel}
    checks["acceleration time near 5.0s / 3.1s"] = (
        4.5 <= seconds[62] <= 5.5 and 2.8 <= seconds[85] <= 3.4)
    print(f"acceleration to full sprint, 62/85: {seconds[62]:.3f}s / {seconds[85]:.3f}s "
          f"(target about 5.0s / 3.1s; steps {accel[62]} / {accel[85]} per frame)")

    shot_targets = {35: 8.5, 54: 3.9, 85: 0.95, 92: 0.82}
    shot_values = {s: bm.kick_error_degrees(s, cfg.shot_error_worst_deg, cfg.shot_error_best_deg)
                   for s in shot_targets}
    shots_ok = all(abs(shot_values[s] - target) <= target * 0.1 for s, target in shot_targets.items())
    checks["shot error targets at 35/54/85/92 within 10%"] = shots_ok
    print("shot error degrees at 35/54/85/92: "
          + " / ".join(f"{shot_values[s]:.3f}" for s in shot_targets)
          + " (targets 8.5 / 3.9 / 0.95 / 0.82; ±10%)")

    standing = {delta: bm.duel_probability(delta, scale=cfg.duel_scale) for delta in (-20, 0, 20)}
    standing_ok = (abs(standing[-20] - 10.0) <= 1.0 and abs(standing[0] - 50.0) <= 5.0
                   and abs(standing[20] - 90.0) <= 9.0)
    checks["standing duel at delta -20/0/+20 near 10/50/90"] = standing_ok
    valid_z2 = range(-297, 314)
    probability_errors = [
        abs(table_probability(z2, slide, duel_values, cfg)
            - bm.duel_probability(z2 / 2.0, slide, cfg.duel_scale, cfg.slide_shift))
        for z2 in valid_z2 for slide in (False, True)
    ]
    probability_error = max(probability_errors)
    endpoint_low = table_probability(-297, False, duel_values, cfg)
    endpoint_high = table_probability(313, False, duel_values, cfg)
    asymptotes_approached = endpoint_low > 5.0 and endpoint_high < 95.0
    checks["duel table formula error <=1 percentage point over valid z"] = probability_error <= 1.0
    checks["standing probabilities approach but do not hit 5/95%"] = asymptotes_approached
    print(f"standing duel at delta -20/0/+20: {standing[-20]:.3f}% / {standing[0]:.3f}% / "
          f"{standing[20]:.3f}% (targets about 10/50/90%; limits 5/95%)")
    print(f"standing table at valid raw-stat extremes: {endpoint_low:.4f}% / {endpoint_high:.4f}% "
          "(must remain inside 5..95%)")
    print(f"standing/slide table maximum formula error over all valid z: {probability_error:.4f} percentage points "
          "(target <=1.0)")

    cave2_source = bm.cave2_asm(cfg)
    cave2_code = bm.asm(cave2_source, bm.CAVE2)
    cave2_used = len(cave2_code) + bm.CAVE2_RODATA_SIZE
    cave2_layout_ok = (len(cave2_code) <= bm.CAVE2_RODATA_OFFSET
                       and bm.CAVE2_RODATA_OFFSET % 4 == 0
                       and bm.CAVE2_RODATA_SIZE % 2 == 0)
    checks["CAVE2 tables fit with aligned/even rodata"] = cave2_layout_ok
    print(f"CAVE2 code/data: {len(cave2_code)} + {bm.CAVE2_RODATA_SIZE} / {bm.CAVE2_SIZE} bytes; "
          f"free={bm.CAVE2_SIZE - cave2_used}; rodata offset={bm.CAVE2_RODATA_OFFSET} (4-byte aligned)")

    print("\nCHECKS")
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}")
    print(f"{'ALL PASS' if table_ok and all(checks.values()) else 'FAILURES'}")
    return 0 if table_ok and all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
