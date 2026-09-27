"""Transfer Market v2 smoke test on the real ARM libCareerMarket.so (Unicorn).

Runs the arm_crosscheck market simulation, then builds screen 0x19 through career_market_new_screen_hook and
drives it like the game does: Init, then Render/Process each frame, with scripted taps and drags on every tab,
a player row, Make offer, the fee slider, Submit and Leave. The game's drawing, font and touch functions are
mocks that check their arguments (finite coordinates, NUL-terminated text) and count calls.

Usage: python tm2_smoke.py <libCareerMarket.so> <unstripped build with symbols> <dataset.txt>
"""
import math
import struct
import subprocess
import sys

import arm_crosscheck as ac
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP


def f2i(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def i2f(i):
    return struct.unpack("<f", struct.pack("<I", i & 0xFFFFFFFF))[0]


stats = {"rect": 0, "print": 0, "bold": 0, "width": 0, "back": 0, "bad": []}
scale = [1.0]
colour = [0xFFFFFFFF]
record = []            # draw calls of the current frame, for PNG previews
_font_cache = {}


def pil_font(px):
    from PIL import ImageFont
    px = max(6, int(round(px)))
    if px not in _font_cache:
        for name in ("C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"):
            try:
                _font_cache[px] = ImageFont.truetype(name, px)
                break
            except OSError:
                continue
    return _font_cache[px]


def text_w(text, sc):
    try:
        return pil_font(20.0 * sc).getlength(text)
    except Exception:
        return len(text) * 9.0 * sc
screen_size = [1280.0, 800.0]
touch = {"pos": (0, 0), "down": (0, 0), "touching": 0, "pressed": 0, "released": 0}


def read_wide(addr, limit=400):
    out = []
    for i in range(limit):
        c = struct.unpack("<H", ac.uc.mem_read(addr + 2 * i, 2))[0]
        if c == 0:
            return "".join(chr(x) for x in out)
        out.append(c)
    stats["bad"].append(f"unterminated text at {addr:#x}")
    return "".join(chr(x) for x in out)


def check_coords(name, *vals):
    for v in vals:
        f = i2f(v)
        if not math.isfinite(f) or abs(f) > 20000:
            stats["bad"].append(f"{name}: coordinate {f}")


def m_rect(x, y, w, h, c):
    stats["rect"] += 1
    check_coords("rect", x, y, w, h)
    record.append(("r", i2f(x), i2f(y), i2f(w), i2f(h), c))
    return 0


def m_setup(font, col, sc, sy):
    colour[0] = col
    scale[0] = i2f(sc)
    if not (0.05 < scale[0] < 10.0):
        stats["bad"].append(f"font scale {scale[0]}")
    return 0


seen = set()


def m_print(x, y, text):
    stats["print"] += 1
    check_coords("print", x, y)
    s = read_wide(text)
    seen.add(s)
    record.append(("t", i2f(x), i2f(y), s, colour[0], scale[0], False))
    return 0


def m_bold(text, x, y, col):
    stats["bold"] += 1
    check_coords("bold", x, y)
    s = read_wide(text)
    seen.add(s)
    record.append(("t", i2f(x), i2f(y), s, col, scale[0], True))
    return 0


def m_width(text):
    stats["width"] += 1
    return f2i(text_w(read_wide(text), scale[0]))


def m_dims(out, text):
    s = read_wide(text)
    ac.uc.mem_write(out, struct.pack("<ff", text_w(s, scale[0]), 20.0 * scale[0]))
    return 0


def render_png(path, size):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (int(size[0]), int(size[1])), (0, 0, 0))
    d = ImageDraw.Draw(img)
    argb = lambda c: ((c >> 16) & 255, (c >> 8) & 255, c & 255)
    for op in record:
        if op[0] == "r":
            _, x, y, w, h, c = op
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=argb(c))
        else:
            _, x, y, s, c, sc, bold = op
            f = pil_font(20.0 * sc)
            d.text((x, y), s, fill=argb(c), font=f)
            if bold:
                d.text((x + 1, y), s, fill=argb(c), font=f)
    img.save(path)


def m_pos(out, kind):
    ac.uc.mem_write(out, struct.pack("<ii", *touch["pos"]))
    return 0


def m_down(out, kind):
    ac.uc.mem_write(out, struct.pack("<ii", *touch["down"]))
    return 0


def m_name(out, cap, info, width, a, b):
    pid = ac.pid_of(info)
    text = f"Player {pid}"[: max(0, ac.s32(cap) - 1)]
    ac.uc.mem_write(out, text.encode("utf-16-le") + b"\0\0")
    return 0


team_names = {}


def m_team(team, a, b):
    team = ac.s32(team)
    if team not in team_names:
        addr = ac.alloc(64)
        ac.uc.mem_write(addr, f"Club {team}".encode("utf-16-le") + b"\0\0")
        team_names[team] = addr
    return team_names[team]


UI_MOCKS = {
    0x1C06AC: (3, lambda size, a, b: ac.alloc(size)),
    0x23B52D: (1, lambda s: 0),
    0x23B5AD: (2, lambda s, i: 0),
    0x23B74D: (2, lambda s, b: 0),
    0x23B75D: (2, lambda s, b: 0),
    0x25EFED: (1, lambda s: f2i(screen_size[0])),
    0x25EFF1: (1, lambda s: f2i(screen_size[1])),
    0x28AF85: (5, m_rect),
    0x294945: (4, m_setup),
    0x38E8C9: (3, m_print),
    0x293E29: (4, m_bold),
    0x38F3C9: (1, m_width),
    0x38F2A1: (2, m_dims),
    0x202B21: (2, m_pos),
    0x202B05: (2, m_down),
    0x202AE5: (1, lambda k: touch["touching"]),
    0x202B79: (1, lambda k: touch["pressed"]),
    0x202B8D: (1, lambda k: touch["released"]),
    0x29910D: (1, lambda b: stats.__setitem__("back", stats["back"] + 1) or 0),
    0x2938CD: (6, m_name),
    0x20C0C9: (3, m_team),
    0x2609B5: (0, lambda: 0),                          # CFEEntityManager::GetMessageBoxQueue: none open
    0x38E301: (1, lambda a: align.append(ac.s32(a)) or 0),   # FTTFont_SetAlign
    0x28BA65: (8, lambda *a: check_coords("triangle", *a[:6]) or 0),
    0x2609C5: (0, lambda: header[0]),                  # CFEEntityManager::GetHeaderMenu
}
align = []
header = [0]
for fn in (0x2B3829, 0x2B383D, 0x2B38A1, 0x2B3851, 0x2B3815, 0x2B38C9, 0x2B3865, 0x2B38B5, 0x2B3801, 0x2B37D9, 0x2B37ED):
    UI_MOCKS[fn] = (1, lambda info: 72)
ac.MOCKS.update(UI_MOCKS)


def symbols(path):
    out = subprocess.run(["wsl", "-e", "nm", path.replace("F:", "/mnt/f").replace("\\", "/")],
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def call(addr, *a, timeout=30_000_000):
    regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
    sp = ac.STACK + ac.STACK_SIZE - 0x100
    for r, v in zip(regs, a[:4]):
        ac.uc.reg_write(r, v & 0xFFFFFFFF)
    ac.uc.reg_write(UC_ARM_REG_SP, sp)
    ac.uc.reg_write(UC_ARM_REG_LR, ac.STOP | 1)
    ac.uc.emu_start(addr | 1, ac.STOP, timeout=timeout)
    pc = ac.uc.reg_read(11 + 0) if False else None
    return ac.uc.reg_read(UC_ARM_REG_R0)


def main():
    lib, symlib, dataset = sys.argv[1], sys.argv[2], sys.argv[3]
    shots = sys.argv[4] if len(sys.argv) > 4 else None
    syms = symbols(symlib)
    sys.argv = ["arm_crosscheck.py", lib, dataset, "1"]
    ac.main()                                          # market state after one season
    exports = ac.load_so.__globals__.get("_exports") or None
    # into next season's pre-season window (turns 17-18), as the game would be when the user opens transfers
    on_turn = ac.LIB + syms["career_market_on_turn"]
    for turn in (17, 18):
        ac.uc.mem_write(ac.SEASON, struct.pack("<3i", 1, turn, 56))
        call(on_turn, ac.SEASON, 0, ac.FAKE)
    for off in UI_MOCKS:
        if off & 1:
            ac.uc.mem_write(ac.FAKE + (off & ~1), b"\x70\x47")
        else:
            ac.uc.mem_write(ac.FAKE + off, struct.pack("<I", 0xE12FFF1E))
    ok = True
    for size in ((2880.0, 1800.0), (1024.0, 640.0), (1422.0, 800.0)):
        screen_size[0], screen_size[1] = size
        s = size[1] / 640.0
        dw = size[0] / s
        ctx = ac.alloc(0x40)
        ac.uc.mem_write(ctx, bytes(0x40))
        ac.wr32(ctx + 4 + 4, 0x19)                     # r[1] = screen id
        ac.wr32(ctx + 4 + 13 * 4, 0x4321)              # lr
        res = call(ac.LIB + syms["career_market_new_screen_hook"], ctx, ac.FAKE)
        screen = ac.rd32(ctx + 4)
        if res != 0x4321 or not screen:
            print(f"FAIL new_screen_hook returned {res:#x}, screen {screen:#x}")
            return 1
        vt = ac.rd32(screen)
        init, process, render_post = ac.rd32(vt + 3 * 4), ac.rd32(vt + 5 * 4), ac.rd32(vt + 36 * 4)
        call(init, screen)

        def frame():
            record.clear()
            call(render_post, screen)
            call(process, screen)

        def snap(name):
            if shots and size[0] == 2880.0:
                frame()
                render_png(f"{shots}/{name}.png", size)

        def tap(x, y):
            p = (int(x * s), int(y * s))
            touch.update(pos=p, down=p, touching=1, pressed=1, released=0)
            frame()
            touch.update(touching=0, pressed=0, released=1)
            frame()
            touch.update(released=0)
            frame()

        def drag(x0, y0, x1, y1, steps=6):
            d = (int(x0 * s), int(y0 * s))
            touch.update(pos=d, down=d, touching=1, pressed=1, released=0)
            frame()
            touch["pressed"] = 0
            for k in range(1, steps + 1):
                touch["pos"] = (int((x0 + (x1 - x0) * k / steps) * s), int((y0 + (y1 - y0) * k / steps) * s))
                frame()
            touch.update(touching=0, released=1)
            frame()
            touch.update(released=0)
            frame()

        before = dict(stats)
        frame()
        for tab in (1, 2, 3, 4, 0):
            tap(80, 56 + 10 + tab * 48 + 23)
        for pos in range(5):                           # position filter chips
            main_w = dw - 176 - 320
            search_w = main_w - 24 - (5 * 42 + 4) - 104 - 16
            tap(176 + 12 + search_w + 8 + 2 + pos * 42 + 20, 56 + 8 + 18)
        drag(176 + 200, 400, 176 + 200, 180)           # scroll the list
        main_w = dw - 176 - 320
        search_w = main_w - 24 - (5 * 42 + 4) - 104 - 16
        tap(176 + 12 + search_w + 8 + 2 + 20, 56 + 8 + 18)                    # position: All
        tap(176 + 12 + search_w + 8 + (5 * 42 + 4) + 8 + 52, 56 + 26)        # sort -> lowest price
        tap(176 + 150, 56 + 52 + 6 + 29)               # first row = cheapest player
        snap("1_scout")
        tap(176 + main_w - 54 + 23, 56 + 52 + 6 + 29)  # Save the first row
        tap(80, 56 + 10 + 1 * 48 + 23)                 # Shortlist tab
        snap("4_shortlist")
        tap(80, 56 + 10 + 4 * 48 + 23)                 # My Squad tab
        snap("5_squad")
        tap(80, 56 + 10 + 1 * 48 + 23)                 # back to the shortlist; bid from there
        tap(176 + 150, 56 + 52 + 6 + 29)
        tap(dw - 320 + 16 + 60, 640 - 60 + 8 + 22)      # Make offer
        snap("2_sheet")
        dx = 176 + (dw - 176) - 400
        px, pw = dx + 18, 400 - 36
        drag(px + pw * 0.5, 56 + 16 + 66 + 12, px + pw * 0.6, 56 + 16 + 66 + 12)   # fee slider
        tap(px + pw * 0.5, 56 + 16 + 66 + 46 + 17)      # + 5%
        tap(px + 40, 56 + 16 + 66 + 46 + 46 + 16 + 18)  # 1 year
        tap(px + 60, 640 - 128 + 50 + 23)               # Submit offer
        snap("3_after_bid")
        tap(px + 60, 640 - 128 + 50 + 23)               # Submit again (counter)
        tap(px + pw - 50, 640 - 128 + 50 + 23)          # Walk away / Leave
        for _ in range(3):
            frame()
        # Android back: with the sheet open it closes the sheet, then it leaves the screen (CFE::Back)
        if not header[0]:
            header[0] = ac.alloc(0x400)
            ac.uc.mem_write(header[0], bytes(0x400))
        tap(dw - 320 + 16 + 60, 640 - 60 + 8 + 22)      # Make offer (opens the sheet again if allowed)
        backs = stats["back"]
        ac.wr32(header[0] + 0x310, 1)
        frame()
        if ac.rds32(header[0] + 0x310) != -1:
            stats["bad"].append("physical back not consumed")
        ac.wr32(header[0] + 0x310, 1)
        frame()
        ac.wr32(header[0] + 0x310, 1)
        frame()
        if stats["back"] <= backs:
            stats["bad"].append("physical back never left the screen")
        ac.wr32(header[0] + 0x310, 0xFFFFFFFF)
        drawn = stats["rect"] - before["rect"]
        text = stats["print"] + stats["bold"] - before["print"] - before["bold"]
        print(f"screen {int(size[0])}x{int(size[1])}: {drawn} rects, {text} texts, back taps {stats['back']}")
        if drawn < 100 or text < 50:
            ok = False
    if stats["bad"]:
        ok = False
        print("problems:", stats["bad"][:10])
    wanted = ["TRANSFER MARKET", "Scout", "My Squad", "MARKET PULSE", "YOUR FEE OFFER", "CONTRACT LENGTH",
              "CLUB'S PATIENCE", "Chance he joins", "ASKING PRICE"]
    missing = [w for w in wanted if w not in seen]
    replies = [t for t in seen if t.startswith(("We offer", "Close.", "Too low", "That is not", "Deal", "Fee agreed",
                                                "Signed", "We are wasting", "You already", "He does not", "His club"))]
    print("club/you log lines seen:", sorted(replies)[:8])
    print("sentences:", sorted(t for t in seen if t.endswith(".") and len(t) > 18)[:12])
    import re
    for t in seen:
        m = re.search(r"player (\d+)\)", t)
        if m:
            pid = int(m.group(1))
            print("player", pid, "dataset (pos, rating, value):", ac.players.get(pid))
            view = ac.alloc(32)
            count = ac.s32(call(ac.LIB + syms["career_market_get_player_count"]))
            for i in range(count):
                if call(ac.LIB + syms["career_market_get_player"], i, view) and ac.rds32(view) == pid:
                    print("market view (id, club, pos, rating, value, wage):", struct.unpack("<6i", ac.uc.mem_read(view, 24)))
    print("window:", [t for t in seen if "WINDOW" in t or "Window" in t][:4],
          "buttons:", [t for t in seen if t in ("Make offer", "Continue talks", "Window closed", "Shortlist",
                                                 "Submit offer", "Not enough coins", "Talks over", "Leave", "Walk away")])
    if missing:
        ok = False
        print("never drawn:", missing)
    if not any(t.startswith("We offer") for t in replies):
        ok = False
        print("the Submit tap never reached the negotiation")
    if not any(not t.startswith("We offer") for t in replies):
        ok = False
        print("the club never answered on screen")
    if any("code -5" in t for t in seen):
        ok = False
        print("a finished signing left Submit enabled")
    if any(a != 0 for a in align) or not align:
        ok = False
        print("text drawn without left alignment:", set(align))
    print("ALL OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
