"""Find Thumb BL/BLX callers of a symbol (direct or via PLT). usage: python xref.py sym..."""
import sys, struct, pickle, os
from armdis import D, SECT, find, name_at, PLT, DEFINED
CACHE = os.path.join(os.path.dirname(__file__), "__pycache__", "bl.pkl")
def build():
    t = SECT[".text"]; base, off, size = t["addr"], t["off"], t["size"]
    hw = struct.unpack_from(f"<{size//2}H", D, off)
    out = []
    for i in range(len(hw) - 1):
        h1, h2 = hw[i], hw[i + 1]
        if (h1 & 0xF800) == 0xF000 and (h2 & 0xC000) == 0xC000:
            S = (h1 >> 10) & 1; imm10 = h1 & 0x3FF; J1 = (h2 >> 13) & 1; J2 = (h2 >> 11) & 1; imm11 = h2 & 0x7FF
            I1 = 1 ^ (J1 ^ S); I2 = 1 ^ (J2 ^ S)
            imm = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)
            if S: imm -= 1 << 25
            pc = base + i * 2 + 4
            if h2 & 0x1000: tgt = pc + imm            # BL
            else: tgt = (pc & ~3) + imm               # BLX
            out.append((base + i * 2, tgt))
    return out
if os.path.exists(CACHE): BL = pickle.load(open(CACHE, "rb"))
else:
    BL = build(); os.makedirs(os.path.dirname(CACHE), exist_ok=True); pickle.dump(BL, open(CACHE, "wb"))
def callers(name):
    va = find(name)
    tg = {va & ~1} | {a for a, n in PLT.items() if n == name}
    return [(s, name_at(s)) for s, t in BL if t in tg]
if __name__ == "__main__":
    for q in sys.argv[1:]:
        print("==", q)
        for s, n in callers(q): print(f"  {s:08x} {n}")
