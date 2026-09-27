"""In-game test on the emulator: scripted multi-touch (stick + optional right-hand hold) while
sampling live controller/player state via probe.py.

usage: python ingame_test.py
"""
import subprocess
import threading
import time

import probe

ADB = probe.ADB
DEV = "/dev/input/event2"          # virtio_input_multi_touch_1 (display touchscreen)
W, H = 1920, 1080
STICK = (300, 760)                 # floating stick: centre = where the finger lands
RIGHT = (1300, 450)                # empty right-side area (no HUD button)


def sx(x):
    return x * 32767 // W


def sy(y):
    return y * 32767 // H


def ev(*t):
    return f"sendevent {DEV} {t[0]} {t[1]} {t[2]}"


def down(slot, tid, x, y):
    return [ev(3, 47, slot), ev(3, 57, tid), ev(3, 53, sx(x)), ev(3, 54, sy(y)), ev(1, 330, 1), ev(0, 0, 0)]


def move(slot, x, y):
    return [ev(3, 47, slot), ev(3, 53, sx(x)), ev(3, 54, sy(y)), ev(0, 0, 0)]


def up(slot):
    return [ev(3, 47, slot), ev(3, 57, -1), ev(0, 0, 0)]


def run_device(cmds):
    subprocess.run([ADB, "shell", ";".join(cmds)], capture_output=True)


def scenario(name, stick_dx, right_hold, hold_s=4.0):
    cmds = []
    if right_hold:
        cmds += down(1, 201, *RIGHT)
    cmds += down(0, 101, *STICK)
    steps = 6
    for i in range(1, steps + 1):
        cmds += move(0, STICK[0] + stick_dx * i // steps, STICK[1])
    cmds += [f"sleep {hold_s}"]
    cmds += up(0) + (up(1) if right_hold else []) + [ev(1, 330, 0), ev(0, 0, 0)]
    t = threading.Thread(target=run_device, args=(cmds,))
    t.start()
    time.sleep(1.5)
    rows = []
    t_end = time.time() + hold_s - 1.8
    while time.time() < t_end:
        rows += probe.sample()
    t.join()
    print(f"\n=== {name}")
    for r in rows:
        print("  " + r)
    time.sleep(1.0)
    return rows


if __name__ == "__main__":
    run_device(down(0, 99, *STICK) + up(0) + [ev(1, 330, 0), ev(0, 0, 0)])   # clear stuck touches
    time.sleep(1)
    scenario("idle (no touch)", 0, False, 3)
    scenario("stick ~35% -> expect jog (sprint=0, target~0x800)", 60, False)
    scenario("stick full -> expect sprint (sprint=1, target~0x1000*fatigue)", 260, False)
    scenario("stick full + right-side hold -> expect close control (sprint=0, low urgency)", 260, True)
    scenario("stick ~35% + right-side hold -> expect close control", 60, True)
