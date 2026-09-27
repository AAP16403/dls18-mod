"""Read live controller / player state from the running game (emulator, adb root).

usage: python probe.py [samples] [interval_s]
Prints per human controller: stick power, sprint flag (+0x54), right-finger touching,
controlled player's urgency (+0x7c current / +0x7e target) and stamina (+0x114).
"""
import struct
import subprocess
import sys
import time

ADB = r"F:\AndroidSDK\platform-tools\adb.exe"
PKG = "com.firsttouchgames.dls3"
GOT_GAME = 0x73063C        # GOT slot holding &game-state struct (see GC_ControllerGetInputAll)


def sh(cmd):
    return subprocess.run([ADB, "shell", cmd], capture_output=True, text=True).stdout


PID = sh(f"pidof {PKG}").strip()
BASE = None
for line in sh(f"cat /proc/{PID}/maps").splitlines():
    if "libDLS18.so" in line:
        rng, _, off = line.split()[:3]
        if int(off, 16) == 0:
            BASE = int(rng.split("-")[0], 16)
            break
if BASE is None:
    sys.exit("libDLS18.so not mapped")


def read(addr, n):
    out = sh(f"dd if=/proc/{PID}/mem bs=1 skip={addr} count={n} 2>/dev/null | xxd -p")
    return bytes.fromhex(out.replace("\n", "").strip())


def u32(a):
    return struct.unpack("<I", read(a, 4))[0]


def sample():
    game = u32(BASE + GOT_GAME)
    rows = []
    for team in range(2):
        cnt = read(game + team * 0x20 + 0x9DB9, 1)[0]
        for i in range(min(cnt, 4)):
            ctrl = u32(game + 0x9DBC + team * 0x20 + i * 4)
            c = read(ctrl, 0x90)
            hw = c[5]
            if hw == 4:          # CPU controller
                continue
            direction, power = struct.unpack_from("<Ii", c, 0x7C)
            player = struct.unpack_from("<I", c, 8)[0]
            urg = tgt = stam = -1
            if player:
                p = read(player + 0x7C, 4)
                urg, tgt = struct.unpack("<hh", p)
                stam = u32(player + 0x114)
            rows.append(f"team{team} ctrl{i} hw={hw} dir={direction & 0xffff:#06x} power={power:#06x} "
                        f"sprint={c[0x54]} urg={urg:#06x} target={tgt:#06x} stamina={stam:#x}")
    return rows


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    dt = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    print(f"pid {PID} libDLS18 base {BASE:#x}")
    for _ in range(n):
        for r in sample():
            print(r)
        time.sleep(dt)
