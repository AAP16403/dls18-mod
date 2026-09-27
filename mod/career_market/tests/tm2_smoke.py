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
    0x2609D5: (0, lambda: footer[0]),                  # CFEEntityManager::GetFooterMenu
    0x25FD09: (2, lambda e, on: inputs.append((e, on)) or 0),   # CFEEntity::EnableInput
    0x24607D: (2, lambda m, b: m_remove_button(m, b)),          # CFEFooterMenu::RemoveButton
    0x23B5B3: (1, lambda s: s),                                  # CFEScreen::~CFEScreen (complete)
    0x5C15C9: (1, lambda p: deleted.append(p) or 0),             # operator delete veneer
}
USER_TEAM_ID = 0x102
league_calls = [0]


def m_league_pos(league, team):
    # a 16-club league: the user 6th, every 13th team id fills the other places
    league_calls[0] += 1
    team = ac.s32(team)
    if team == USER_TEAM_ID:
        return 5
    if team % 13 == 0 and team < 13 * 16:
        return (team // 13) % 16
    return -1


UI_MOCKS[0x36CC41] = (1, lambda season: 4)      # CSeason::GetUserLeagueInTree: Division 3
UI_MOCKS[0x36202F] = (2, m_league_pos)          # CTournament::GetTeamLeaguePos
UI_MOCKS[0x241FB5] = (1, lambda field: 0)       # CFETextField::GetText (keyboard never confirmed here)
tmg = {"swaps": [], "saves": 0, "forwards": [], "tm": 0, "link": 0}


def m_tmg_swap(tm, a, b, force, x=0, y=0):
    a, b = ac.s32(a), ac.s32(b)
    tmg["swaps"].append((a, b))
    ids = [ac.rd32(tm + 0x142 + 2 * i) & 0xFFFF for i in range(32)]
    if a in ids and b in ids:
        ia, ib = ids.index(a), ids.index(b)
        ac.uc.mem_write(tm + 0x142 + 2 * ia, struct.pack("<H", b))
        ac.uc.mem_write(tm + 0x142 + 2 * ib, struct.pack("<H", a))
    return 0


def m_tmg_save(tm, really):
    tmg["saves"] += 1 if really else 0
    return 0


UI_MOCKS[0x203835] = (0, lambda: 0)                                  # CCore::InGame: menus
UI_MOCKS[0x2F27D9] = (4, m_tmg_swap)                                 # CTeamManagement::SwapPlayersByID
UI_MOCKS[0x2F2C85] = (2, m_tmg_save)                                 # CTeamManagement::Save
UI_MOCKS[0x20934D] = (1, lambda team: tmg["link"])                   # CDataBase::GetTeamLink
UI_MOCKS[0x2F0A2D] = (3, lambda self, a, b: abs(ac.s32(a) - ac.s32(b)))   # PlayerPositionSuitability
UI_MOCKS[0x2984CD] = (6, lambda *a: tmg["forwards"].append(ac.s32(a[0])) or 0)   # CFE::Forward
deleted = []
footer = [0]
inputs = []
removed = []


def m_remove_button(menu, bid):
    bid = ac.s32(bid)
    removed.append(bid)
    off = 0x108 if bid < 32 else 0x10C
    ac.wr32(menu + off, ac.rd32(menu + off) & ~(1 << (bid % 32)))
    return 0
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
        if off >= 0x500000:                            # arm_crosscheck only watches the first 5 MB
            from unicorn import UC_HOOK_CODE
            ac.uc.hook_add(UC_HOOK_CODE, ac.on_code, begin=ac.FAKE + (off & ~1), end=ac.FAKE + (off & ~1) + 2)
    ok = True
    for size in ((2880.0, 1800.0), (1024.0, 640.0), (1422.0, 800.0)):
        screen_size[0], screen_size[1] = size
        s = size[1] / 640.0
        dw = size[0] / s
        # the stock footer as the game leaves it for screen 0x19: Scout Players (0x2a) and Sell Player (9)
        footer[0] = ac.alloc(0x400)
        ac.uc.mem_write(footer[0], bytes(0x400))
        ac.wr32(footer[0] + 0x108, 1 << 9)
        ac.wr32(footer[0] + 0x10C, 1 << (0x2A - 32))
        del removed[:]
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

        def find(label):
            # design-space centre of the last drawn text equal to label
            for op in reversed(record):
                if op[0] == "t" and op[3] == label:
                    return (op[1] + text_w(label, op[5]) * 0.5) / s, (op[2] + 8.0 * s) / s
            stats["bad"].append(f"'{label}' not on screen")
            return None

        def tap_label(label):
            frame()
            at = find(label)
            if at:
                tap(*at)
            return at is not None

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
        # Club Hub: the market's top-bar button, every section, a renewal, keep, list, then back to the market
        ac.wr32(ac.FAKE + 0x84A260 + 0x14 + 0x6AC, ac.alloc(16))       # CSeason+0x6AC: the league
        backs_before_hub = stats["back"]
        tap_label("Club Hub")
        tap(80, 56 + 10 + 0 * 48 + 23)                 # Finances (the hub remembers its last section)
        snap("6_hub_finances")
        tap_label("Last season")
        tap_label("This season")
        tap(80, 56 + 10 + 1 * 48 + 23)                 # Board
        snap("7_hub_board")
        tap(80, 56 + 10 + 2 * 48 + 23)                 # Contracts
        list_w = dw - 176 - 320
        drag(176 + 150, 500, 176 + 150, 260)           # scroll the contracts
        drag(176 + 150, 260, 176 + 150, 560)
        tap(176 + 150, 56 + 62 + 1 + 26 + 4 + 25)      # first row: the contract ending soonest
        bx, bw = dw - 320 + 16 + 12, 320 - 32 - 24
        track_y = 56 + 16 + 68 + 56 + 10 + 52 + 8
        drag(bx + bw * 0.5, track_y, bx + bw * 0.25, track_y)   # wage slider down: a short offer
        tap_label("+ 5%")
        tap_label("2 yr")
        snap("8_hub_contracts")
        tap_label("Offer renewal")
        snap("9_hub_reply")
        frame()
        counters = [op[3] for op in record if op[0] == "t" and op[3].startswith("Accept ")]
        if counters:
            tap_label(counters[0])
        tap_label("Keep him")
        tap_label("Kept")
        tap_label("List for sale")
        frame()
        if any(op[0] == "t" and op[3] == "Listed" for op in record):
            tap_label("Listed")
        if league_calls[0] == 0:
            stats["bad"].append("the board never read the league table")
        # Android back in the hub returns to the market, it does not leave the screen
        if not header[0]:
            header[0] = ac.alloc(0x400)
            ac.uc.mem_write(header[0], bytes(0x400))
        ac.wr32(header[0] + 0x310, 1)
        frame()
        frame()
        if stats["back"] != backs_before_hub:
            stats["bad"].append("back in the Club Hub left the transfer screen")
        tap_label("Club Hub")                          # and the Market button does the same
        tap(60, 28)
        frame()
        if not any(op[0] == "t" and op[3] == "TRANSFER MARKET" for op in record):
            stats["bad"].append("the Market button did not return to the transfer market")
        tap(80, 56 + 10 + 4 * 48 + 23)                 # My Squad tab again
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
        if sorted(set(removed)) != [9, 0x2A] or ac.rd32(footer[0] + 0x108) or ac.rd32(footer[0] + 0x10C):
            stats["bad"].append(f"stock footer buttons not removed: {sorted(set(removed))}")
        footer_calls = [on for e, on in inputs if e == footer[0]]
        if not footer_calls or any(footer_calls):
            stats["bad"].append(f"stock footer input not switched off every frame: {footer_calls[:5]}")
        # Back deletes the screen through vtable slot 1 (CFEScreenStack::DeleteTopScreen); the stock slot is a trap
        if ac.rd32(vt + 1 * 4) == ac.rd32(ac.FAKE + 0x71ABD0 + 4):
            stats["bad"].append("deleting destructor still the abstract CFEScreen trap")
        call(ac.rd32(vt + 1 * 4), screen)
        if screen not in deleted:
            stats["bad"].append("screen memory not freed by its deleting destructor")
        if not inputs or inputs[-1][1] != 1:
            stats["bad"].append("stock bars not re-enabled on exit")
        del inputs[:]
        drawn = stats["rect"] - before["rect"]
        text = stats["print"] + stats["bold"] - before["print"] - before["bold"]
        print(f"screen {int(size[0])}x{int(size[1])}: {drawn} rects, {text} texts, back taps {stats['back']}")
        if drawn < 100 or text < 50:
            ok = False
    # ---- Team Management v2 (screen 4) ----
    import os
    real = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "unpacked", "apk", "lib",
                             "armeabi-v7a", "libDLS18.so"), "rb").read()
    def rodata(va, n):
        # the tables sit in the first load segment, file offset == virtual address there
        return real[va:va + n]
    ac.uc.mem_write(ac.FAKE + 0x63A55C, rodata(0x63A55C, 12 * 44))
    ac.uc.mem_write(ac.FAKE + 0x63A76C, rodata(0x63A76C, 12 * 44))
    ac.wr32(ac.FAKE + 0x7C2AAC + 0xFB0, 0xFFFFFFFF)                   # not an online match
    view = ac.alloc(32)
    count = ac.s32(call(ac.LIB + syms["career_market_get_player_count"]))
    user_ids = []
    for i in range(count):
        if call(ac.LIB + syms["career_market_get_player"], i, view) and ac.rds32(view + 4) == USER_TEAM_ID:
            user_ids.append(ac.rds32(view))
    tm = ac.FAKE + 0x84A260 + 0x14 + 0x6E0
    ac.uc.mem_write(tm, bytes(0x200))
    cteam = ac.alloc(0x1100)
    ac.uc.mem_write(cteam, bytes(0x1100))
    ac.uc.mem_write(tm + 0x140, bytes([min(18, len(user_ids))]))
    for i, pid in enumerate(user_ids[:18]):
        ac.uc.mem_write(tm + 0x142 + 2 * i, struct.pack("<H", pid))
    ac.wr32(tm + 0x194, cteam)
    ac.wr32(cteam + 0x1014, tm)
    ac.uc.mem_write(cteam + 0x12F, bytes([6]))
    link = ac.alloc(0x108)
    ac.uc.mem_write(link, bytes(0x108))
    ac.wr32(link + 4, min(16, len(user_ids)))
    for i, pid in enumerate(user_ids[:16]):
        ac.wr32(link + 0x88 + 4 * i, pid)
    tmg["link"] = link
    for off in (0x203835, 0x2F27D9, 0x2F2C85, 0x20934D, 0x2F0A2D, 0x2984CD):
        ac.uc.mem_write(ac.FAKE + (off & ~1), b"\x70\x47")
    for size in ((2880.0, 1800.0), (1024.0, 640.0), (1422.0, 800.0)):
        screen_size[0], screen_size[1] = size
        s = size[1] / 640.0
        dw = size[0] / s
        footer[0] = ac.alloc(0x400)
        ac.uc.mem_write(footer[0], bytes(0x400))
        ctx = ac.alloc(0x40)
        ac.uc.mem_write(ctx, bytes(0x40))
        ac.wr32(ctx + 4 + 4, 4)
        ac.wr32(ctx + 4 + 13 * 4, 0x4321)
        res = call(ac.LIB + syms["career_market_new_screen_hook"], ctx, ac.FAKE)
        screen = ac.rd32(ctx + 4)
        if res != 0x4321 or not screen:
            print(f"FAIL team management screen not built ({res:#x})")
            return 1
        vt = ac.rd32(screen)
        init, process, render_post = ac.rd32(vt + 3 * 4), ac.rd32(vt + 5 * 4), ac.rd32(vt + 36 * 4)
        call(init, screen)

        def frame():
            record.clear()
            call(render_post, screen)
            call(process, screen)

        def tap(x, y):
            p2 = (int(x * s), int(y * s))
            touch.update(pos=p2, down=p2, touching=1, pressed=1, released=0)
            frame()
            touch.update(touching=0, pressed=0, released=1)
            frame()
            touch.update(released=0)
            frame()

        def find(label):
            for op in reversed(record):
                if op[0] == "t" and op[3] == label:
                    return (op[1] + text_w(label, op[5]) * 0.5) / s, (op[2] + 8.0 * s) / s
            stats["bad"].append(f"team management: '{label}' not on screen")
            return None

        def tap_label(label):
            frame()
            at = find(label)
            if at:
                tap(*at)

        before = dict(stats)
        swaps0, saves0 = len(tmg["swaps"]), tmg["saves"]
        frame()
        tap(80, 56 + 10 + 0 * 48 + 23)                      # Lineup (the screen remembers its last section)
        if shots and size[0] == 2880.0:
            render_png(f"{shots}/10_teamman_lineup.png", size)
        tap_label("4-4-2")                                  # formation
        if ac.uc.mem_read(cteam + 0x12F, 1)[0] != 0:
            stats["bad"].append("team management: formation tap did not set the formation")
        tap_label("4-3-3")
        # pick a player on the pitch, Swap, then another
        frame()
        discs = [op for op in record if op[0] == "t" and op[3].startswith("Player ")]
        if len(discs) < 11:
            stats["bad"].append(f"team management: {len(discs)} player names on the pitch")
        else:
            a, b = discs[0], discs[5]
            tap((a[1] + text_w(a[3], a[5]) * 0.5) / s, a[2] / s - 30)
            tap_label("Swap")
            tap((b[1] + text_w(b[3], b[5]) * 0.5) / s, b[2] / s - 30)
        if len(tmg["swaps"]) == swaps0:
            stats["bad"].append("team management: the swap never reached SwapPlayersByID")
        tap(80, 56 + 10 + 1 * 48 + 23)                      # Squad
        if shots and size[0] == 2880.0:
            frame(); render_png(f"{shots}/11_teamman_squad.png", size)
        d0 = (int((176 + 150) * s), int(400 * s))
        touch.update(pos=d0, down=d0, touching=1, pressed=1, released=0); frame()
        touch["pressed"] = 0
        for k in range(1, 7):
            touch["pos"] = (d0[0], int((400 - 40 * k) * s)); frame()
        touch.update(touching=0, released=1); frame(); touch.update(released=0); frame()
        tap(176 + 150, 56 + 28 + 4 + 25)                    # a squad row
        tap(80, 56 + 10 + 2 * 48 + 23)                      # Chemistry
        if shots and size[0] == 2880.0:
            frame(); render_png(f"{shots}/12_teamman_chemistry.png", size)
        forwards0 = len(tmg["forwards"])
        tap_label("Classic")
        if tmg["forwards"][forwards0:] != [4]:
            stats["bad"].append(f"team management: Classic forwarded {tmg['forwards'][forwards0:]}")
        # Classic built the stock screen once
        ctx2 = ac.alloc(0x40); ac.uc.mem_write(ctx2, bytes(0x40)); ac.wr32(ctx2 + 8, 4); ac.wr32(ctx2 + 4 + 13 * 4, 0x4321)
        if call(ac.LIB + syms["career_market_new_screen_hook"], ctx2, ac.FAKE) != 0:
            stats["bad"].append("team management: Classic did not let the stock screen through")
        backs = stats["back"]
        if not header[0]:
            header[0] = ac.alloc(0x400); ac.uc.mem_write(header[0], bytes(0x400))
        ac.wr32(header[0] + 0x310, 1)
        frame()
        if stats["back"] != backs + 1:
            stats["bad"].append("team management: Android back did not leave the screen")
        if tmg["saves"] == saves0:
            stats["bad"].append("team management: leaving did not save the edited lineup")
        call(ac.rd32(vt + 1 * 4), screen)                   # deleting destructor
        drawn = stats["rect"] - before["rect"]
        print(f"team management {int(size[0])}x{int(size[1])}: {drawn} rects, swaps {len(tmg['swaps']) - swaps0}, "
              f"saves {tmg['saves'] - saves0}")
    for w in ("TEAM MANAGEMENT", "Lineup", "Squad", "Chemistry", "TEAM CHEMISTRY", "AVG BOOST", "LAST 5", "BOOST",
              "STARTING XI CHEMISTRY", "LINKS ON THE PITCH", "HOLDING THE XI BACK", "MATCH RATINGS, OLDEST FIRST",
              "Match stat boost", "Position fit"):
        if w not in seen:
            stats["bad"].append(f"team management: '{w}' never drawn")
    if stats["bad"]:
        ok = False
        print("problems:", stats["bad"][:10])
    wanted = ["TRANSFER MARKET", "Scout", "My Squad", "MARKET PULSE", "YOUR FEE OFFER", "CONTRACT LENGTH",
              "CLUB'S PATIENCE", "Chance he joins", "ASKING PRICE",
              "CLUB HUB", "Finances", "Board", "Contracts", "SPENDABLE NOW", "WAGE BILL A SEASON", "INCOME", "SPENDING",
              "BOARD TARGET THIS SEASON", "Board confidence", "WHAT DIVISION 3 PAYS", "PROMOTION IS WORTH",
              "YOUR WAGE OFFER", "HIS PATIENCE", "CLUBS PAY", "WAGE / ASKS", "Offer renewal"]
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
    hub_replies = sorted(t for t in seen if t.startswith(("He signs", "Not enough", "He is tired", "Kept.", "No longer kept",
                                                          "Listed for sale", "Taken off", "Finish around")))
    print("club hub lines seen:", hub_replies[:8])
    if not any(t.startswith(("He signs", "Not enough")) for t in hub_replies):
        ok = False
        print("the renewal offer never got an answer")
    if not any(t.startswith("Finish around") for t in hub_replies):
        ok = False
        print("no board target drawn")
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
