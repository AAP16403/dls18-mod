"""Emulate the stock-screen and economy hooks: hook site -> cave stub -> dlopen/dlsym -> market fn ->
skip exit. usage: python test_hooks.py <libDLS18.so>"""
import json
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R4, UC_ARM_REG_R6,
                               UC_ARM_REG_R7, UC_ARM_REG_SP, UC_ARM_REG_R3, UC_ARM_REG_IP)

lib = open(sys.argv[1], "rb").read()
B = 0x40000000
STUBS = {0x1D90FC: "dlopen", 0x1D855C: "dlsym", 0x1D9138: "dlclose", 0x1C35F8: "flow",
         0x36AA08: "next_season"}
FN = 0x10010000
CARD, INFO_SCOUT, TM, SEASON = 0x10020000, 0x10030000, 0x10040000, 0x10060000
IDENTITY = b"\x70\x47"                 # bx lr (returns r0 unchanged: whatever arg1 the caller passed)
RETURN_1 = b"\x01\x20\x70\x47"         # movs r0,#1 ; bx lr (simulates "the mod fully handled this")

# (name, hook, skip, initial regs, fn_stub, expected fn hits [(r0,r1),...], expected dlsym symbols [in order])
cases = [
    ("serialize", 0x36BC42, 0x36BC46, {UC_ARM_REG_R4: 0x10050000}, IDENTITY,
     [(4294953468, 268763136)], ["career_market_on_serialize"]),
    ("turn", 0x36A078, 0x36A07C, {UC_ARM_REG_R4: 0x10050000}, IDENTITY,
     [(268763136, 0)], ["career_market_on_turn"]),
    ("screen", 0x276490, 0x276494, {}, IDENTITY,
     [(0, 0)], ["career_market_on_turn"]),
    ("buy search", 0x277006, 0x2770E0, {UC_ARM_REG_R4: CARD}, IDENTITY,
     [(268567188, 77)], ["career_market_stock_action"]),
    ("buy scout", 0x250A3C, 0x250AC2, {UC_ARM_REG_R6: CARD, UC_ARM_REG_R7: INFO_SCOUT}, IDENTITY,
     [(268632064, 77)], ["career_market_stock_action"]),
    ("sell", 0x23E404, 0x23E426, {UC_ARM_REG_R4: TM}, IDENTITY,
     [(268567188, 4294967295)], ["career_market_stock_action"]),
    # econ hooks (v6, data/ECONOMY_HOOKS.md 1.6/3.2): mod handles it (fn returns 1) -> bx lr, back at hook+4
    ("econ match handled", 0x23A274, 0x23A278, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 12345}, RETURN_1,
     [(0, 0)], ["career_market_on_match_awards"]),
    # mod declines (fn returns 0, the arg1 it was called with) -> falls through to the stock SetMatchCredits tail call
    ("econ match fallback", 0x23A274, 0x3776A4, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 12345}, IDENTITY,
     [(0, 0)], ["career_market_on_match_awards"]),
    # season: dispatch(pre) -> stubbed CSeason::NextSeason -> dispatch(post), one dlopen/dlsym/dlclose each
    ("econ season", 0x29900A, 0x29900E, {UC_ARM_REG_R0: SEASON, UC_ARM_REG_R1: 1}, IDENTITY,
     [(SEASON, 1), (0, 0)], ["career_market_on_season", "career_market_on_season"]),
    # v7 money routing: CMyProfile::SubtractCredits / AddCredits entries -> fn(amount, caller return offset)
    ("spend direct", 0x377A18, 0x377A1C, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 60,
                                          UC_ARM_REG_LR: B + 0x2504AD}, IDENTITY,
     [(60, 0x2504AD)], ["career_market_on_spend"]),
    ("spend via CCredits", 0x377A18, 0x377A1C, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 40,
                                                UC_ARM_REG_LR: B + 0x2633D9}, IDENTITY,
     [(40, 0x23E6E3)], ["career_market_on_spend"]),
    ("income direct", 0x377984, 0x37798A, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 25,
                                           UC_ARM_REG_R2: 0, UC_ARM_REG_R3: 1, UC_ARM_REG_LR: B + 0x263B49}, IDENTITY,
     [(25, 0x263B49)], ["career_market_on_income"]),
    ("income via CCredits", 0x377984, 0x37798A, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 92,
                                                 UC_ARM_REG_R2: 0, UC_ARM_REG_R3: 1, UC_ARM_REG_LR: B + 0x2634E7}, IDENTITY,
     [(92, 0x298CBB)], ["career_market_on_income"]),
]
# expected exit state for the money hooks: sp delta, and registers the resumed stock code relies on
MONEY_EXIT = {"spend direct": (0, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 60, UC_ARM_REG_R2: 0xA7CC}),
              "spend via CCredits": (0, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 40, UC_ARM_REG_R2: 0xA7CC}),
              "income direct": (-8, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 25, UC_ARM_REG_R2: 0,
                                     UC_ARM_REG_R3: 1, UC_ARM_REG_IP: 0xA7CC}),
              "income via CCredits": (-8, {UC_ARM_REG_R0: 0x10070000, UC_ARM_REG_R1: 92, UC_ARM_REG_R2: 0,
                                           UC_ARM_REG_R3: 1, UC_ARM_REG_IP: 0xA7CC})}
ok = True
for name, hook, skip, regs, fn_stub, expect_fn, expect_syms in cases:
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mu.mem_map(B, 0x800000)
    mu.mem_write(B, lib[:0x800000])
    mu.mem_map(0x10000000, 0x100000)
    mu.mem_write(FN, fn_stub)
    mu.mem_write(CARD + 0x344, (77).to_bytes(4, "little"))
    mu.mem_write(TM + 0x100, CARD.to_bytes(4, "little"))
    sp = 0x100F0000
    log = []
    # the CCredits wrappers pushed {r4, lr}: the original caller's return address sits at [sp+4]
    mu.mem_write(sp + 4, ((B + (0x23E6E3 if name.startswith("spend") else 0x298CBB)) & 0xFFFFFFFF).to_bytes(4, "little"))
    exit_regs = {}

    def hk(uc, a, s, u):
        off = a - B
        th = uc.reg_read(UC_ARM_REG_CPSR) & 0x20
        if off in STUBS:
            nm = STUBS[off]
            if nm == "dlsym":
                sym = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R1), 40)).split(b"\0")[0].decode()
                log.append(("dlsym", sym))
            elif nm == "dlopen":
                lib_name = bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R0), 40)).split(bytes([0]))[0].decode()
                log.append(("dlopen", lib_name, "ARM" if not th else "THUMB!"))
            else:
                log.append((nm, "ARM" if not th else "THUMB!"))
            uc.reg_write(UC_ARM_REG_R0, {"dlopen": 0x9A18CA01, "dlsym": FN | 1, "dlclose": 0,
                                          "flow": 7, "next_season": 0}[nm])
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            return
        if a == FN:
            log.append(("fn", uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2) - B))
        in_fn = FN <= a < FN + len(fn_stub)
        if off == skip:
            log.append(("exit", hex(off), uc.reg_read(UC_ARM_REG_SP) - sp))
            for r in MONEY_EXIT.get(name, (0, {}))[1]:
                exit_regs[r] = uc.reg_read(r)
            uc.emu_stop()
        elif not (0x1FC6F0 <= off < 0x1FCD7C or 0x381368 <= off < 0x3817C0) and not in_fn and off != hook:
            log.append(("STRAY", hex(off)))
            uc.emu_stop()

    mu.hook_add(UC_HOOK_CODE, hk)
    mu.reg_write(UC_ARM_REG_SP, sp)
    for r, v in regs.items():
        mu.reg_write(r, v)
    try:
        mu.emu_start((B + hook) | 1, B + 0x7FFFFF, count=3000)
    except UcError as e:
        log.append(("EMU ERROR", str(e), hex(mu.reg_read(UC_ARM_REG_PC)),
                    hex(mu.reg_read(UC_ARM_REG_CPSR)), hex(mu.reg_read(UC_ARM_REG_LR)),
                    hex(mu.reg_read(UC_ARM_REG_SP))))
    fn_hits = [(x[1], x[2]) for x in log if x[0] == "fn" and x[3] == 0]
    dlsym_syms = [x[1] for x in log if x[0] == "dlsym"]
    good = (fn_hits == expect_fn and dlsym_syms == expect_syms and
            log.count(("dlopen", "libCareerMarket.so", "ARM")) == len(expect_syms) and
            log and log[-1][0] == "exit" and
            log[-1][2] == (32 if name == "turn" else MONEY_EXIT.get(name, (0, {}))[0]) and
            all(exit_regs.get(r) == v for r, v in MONEY_EXIT.get(name, (0, {}))[1].items()))
    ok &= bool(good)
    print(f"{name:20s} {'OK ' if good else 'BAD'} {log}")

layout_path = Path(sys.argv[1] + ".layout.json")
if layout_path.exists():
    layout = json.loads(layout_path.read_text())
    screen_hook = layout.get("hooks", {}).get("career_market_new_screen")
    good = screen_hook is not None
    if good:
        data = lib
        phoff, = struct.unpack_from("<I", data, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)

        def va_to_off(va, size=1):
            for i in range(phnum):
                p_type, p_off, p_va, _, p_filesz, _, _, _ = struct.unpack_from(
                    "<8I", data, phoff + i * phentsize)
                if p_type == 1 and p_va <= va and va + size <= p_va + p_filesz:
                    return p_off + va - p_va
            return None

        cave3 = layout["cave3_va"]
        desc_off = va_to_off(cave3, 32)
        hooks = layout.get("hooks", {})

        def fnv(name):
            h = 2166136261
            for byte in name.encode():
                h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
            return h

        by_hash = {fnv(name): (name, info) for name, info in hooks.items()}
        if desc_off is None:
            good = False
        else:
            magic, count, slots, version = struct.unpack_from("<4I", data, desc_off)
            good = (magic == 0x3144434D and count == len(hooks) and count >= 1 and
                    slots == layout["moddata_va"] and version == 1)
            seen = set()
            for i in range(count if good else 0):
                site, resume, name_hash, length = struct.unpack_from("<4I", data, desc_off + 16 + 16 * i)
                name, info = by_hash.get(name_hash, (None, None))
                site_off = va_to_off(site, 4)
                if name is None or site_off is None or name in seen or length < 4:
                    good = False
                    break
                seen.add(name)
                patch_ins = list(Cs(CS_ARCH_ARM, CS_MODE_THUMB).disasm(data[site_off:site_off + 4], site))
                target = int(patch_ins[0].op_str.lstrip("#"), 0) if patch_ins else -1
                good &= (patch_ins[0].mnemonic == "b.w" and target == int(info["stub"], 16) and
                         resume == int(info["resume"], 16) | 1 and int(info["slot"], 16) == i)
            good &= "career_market_new_screen" in seen
    ok &= bool(good)
    print(f"market screen factory {'OK' if good else 'BAD'} "
          f"{screen_hook if screen_hook else 'runtime hook descriptor missing'}")

bridge_source = (Path(__file__).resolve().parents[1] / "native_bridge.c").read_text(encoding="utf-8")
page_checks = [
    "UI_SCREEN_BUTTON_PAGE_PREVIOUS",
    "UI_SCREEN_BUTTON_PAGE_NEXT",
    "market_ui_page_window(g_ui_screen_total_rows",
    "market_ui_page_turn_offset(g_ui_screen_total_rows",
    "market_ui_screen_page_footer_height",
    'ui_text_append_ascii(&page, " / ")',
    "UI_SCREEN_LIST_MARKET", "UI_SCREEN_LIST_SALES", "UI_SCREEN_LIST_INBOX",
    "UI_SCREEN_LIST_SQUAD", "UI_SCREEN_LIST_HISTORY", "UI_SCREEN_LIST_FINANCES",
    "UI_SCREEN_LIST_SHORTLIST",
]
good = all(check in bridge_source for check in page_checks)
ok &= bool(good)
print(f"full-screen market pagination wiring {'OK' if good else 'BAD'} "
      f"({sum(check in bridge_source for check in page_checks)}/{len(page_checks)} checks)")
detail_checks = [
    "static void market_ui_screen_action_layout",
    "market_ui_uses_player_cards(), &option_columns",
    "market_ui_draw_body(g_ui_description, action_x + width * 0.020f",
    "action_detail_h - height * 0.030f",
    "action_option_top - height * 0.040f",
]
detail_wired = (all(check in bridge_source for check in detail_checks) and
                bridge_source.count("market_ui_screen_action_layout(") >= 3)
ok &= bool(detail_wired)
print(f"full-screen selected-detail pane wiring {'OK' if detail_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in detail_checks)}/{len(detail_checks)} checks)")
filter_checks = [
    "UI_SCREEN_BUTTON_FILTER_POSITION",
    "UI_SCREEN_BUTTON_FILTER_CLUB",
    "UI_SCREEN_BUTTON_FILTER_SEARCH",
    "market_ui_screen_filter_height(height, g_ui_screen_list_mode)",
    'ui_text_append_ascii(&position_builder, "Position: ")',
    'ui_text_append_ascii(&club_builder, "Club: ")',
    '"Search: Active" : "Search by name"',
]
filter_wired = all(check in bridge_source for check in filter_checks)
ok &= bool(filter_wired)
print(f"full-screen market filter toolbar wiring {'OK' if filter_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in filter_checks)}/{len(filter_checks)} checks)")
focus_ranges = [
    ("static void ui_show_sales_player(uint32_t base, int32_t index) {",
     "static void ui_show_quick_sale(uint32_t base, int32_t index) {"),
    ("static void ui_show_shortlist_player(uint32_t base, int32_t index) {",
     "static void ui_show_shortlist(uint32_t base) {"),
    ("static void ui_show_roster(uint32_t base) {",
     "static void ui_show_contract_renewal(uint32_t base) {"),
]
focus_blocks = []
for start_text, end_text in focus_ranges:
    start = bridge_source.find(start_text)
    end = bridge_source.find(end_text, start + len(start_text))
    focus_blocks.append(bridge_source[start:end] if start >= 0 and end > start else "")
# v31 option boxes: at most 3 buttons per box; list pages page through players themselves.
focus_checks = [
    bool(focus_blocks[0]) and '{"List / Unlist", "Quick Sale", "Back"}' in focus_blocks[0] and
        '"Next Player", "Sale Options", "Previous Player", "Position Filter", "Back"' in focus_blocks[0],
    bool(focus_blocks[1]) and '"Make Offer", "Next Player", "Remove", "Previous Player", "Back"' in focus_blocks[1],
    bool(focus_blocks[2]) and '"Next player", "Renew contract", "Previous player"' in focus_blocks[2],
    "static int32_t ui_box_chunk_layout" in bridge_source and "#define MARKET_UI_BOX_BUTTONS 3" in bridge_source,
]
focus_wired = all(focus_checks)
ok &= bool(focus_wired)
print(f"card-list focused action wiring {'OK' if focus_wired else 'BAD'} "
      f"({sum(focus_checks)}/{len(focus_checks)} checks)")
scheme_checks = [
    "static int32_t market_ui_option_scheme",
    '"Continue", "Make Offer", "Confirm Sale", "Submit Offer"',
    '"Accept fee", "Accept counter", "Accept player wage"',
    '"Renew contract", "Submit renewal"',
    "market_ui_option_scheme(g_ui_options[i])",
]
scheme_wired = all(check in bridge_source for check in scheme_checks)
ok &= bool(scheme_wired)
print(f"primary action colour-scheme wiring {'OK' if scheme_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in scheme_checks)}/{len(scheme_checks)} checks)")
back_checks = [
    "static int32_t market_ui_screen_is_root_page",
    "static void market_ui_screen_go_back",
    "market_ui_screen_go_back(g_ui_base)",
    "g_ui_page_callback == ui_callback_notice",
    "g_ui_quick_sale_player_id = -1",
    "g_ui_renewal_stage = 0",
]
back_wired = all(check in bridge_source for check in back_checks)
ok &= bool(back_wired)
print(f"contextual back navigation wiring {'OK' if back_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in back_checks)}/{len(back_checks)} checks)")
selection_checks = [
    "static void market_ui_draw_card_outline",
    "if (market_ui_row_is_selected(row))",
    "market_ui_draw_card_outline(card_x, card_y, card_w, card_h",
    "3.0f, 0xFF4FAE91u, draw_rect",
    "draw_rect(x - thickness, y - thickness",
]
selection_wired = all(check in bridge_source for check in selection_checks)
ok &= bool(selection_wired)
print(f"selected-card outline wiring {'OK' if selection_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in selection_checks)}/{len(selection_checks)} checks)")
sort_checks = [
    "UI_SCREEN_BUTTON_SORT",
    "static void market_ui_sort_market_targets",
    "market_ui_sort_market_targets();",
    'ui_text_append_ascii(&sort_builder, "Sort: ")',
    "g_ui_market_sort = (g_ui_market_sort + 1) % 3",
    "g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0",
]
sort_wired = all(check in bridge_source for check in sort_checks)
ok &= bool(sort_wired)
print(f"market result sorting wiring {'OK' if sort_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in sort_checks)}/{len(sort_checks)} checks)")
sales_filter_checks = [
    "static int32_t g_ui_sales_position_filter = -1;",
    "static int32_t ui_screen_sales_player_visible",
    "if (ui_screen_sales_player_visible(i))",
    'g_ui_browser_mode == 1 ? "Sales Position" : "Market Position"',
    'ui_title_matches(title, "Sales Position")',
    "g_ui_sales_position_filter = selection == 0 ? -1 : selection - 1;",
    'ui_text_append_ascii(&position_builder, "Squad position: ")',
    "g_ui_screen_list_mode == UI_SCREEN_LIST_SALES",
]
tab_start = bridge_source.find("static void market_ui_select_tab(int32_t tab) {")
tab_end = bridge_source.find("static void ui_screen_clear_rows", tab_start)
tab_block = bridge_source[tab_start:tab_end] if tab_start >= 0 and tab_end > tab_start else ""
sales_filter_results = [check in bridge_source for check in sales_filter_checks]
sales_filter_results.append(bool(tab_block) and "g_ui_position_filter = -1" not in tab_block and
                            "g_ui_sales_position_filter = -1" not in tab_block)
sales_filter_wired = all(sales_filter_results)
ok &= bool(sales_filter_wired)
print(f"independent Sales position filter wiring {'OK' if sales_filter_wired else 'BAD'} "
      f"({sum(sales_filter_results)}/{len(sales_filter_results)} checks)")
squad_toolbar_checks = [
    "UI_SCREEN_BUTTON_CLUB_PREVIOUS",
    "UI_SCREEN_BUTTON_CLUB_NEXT",
    'ui_text_append_ascii(&previous_builder, "Previous club")',
    'ui_text_append_ascii(&next_builder, "Next club")',
    "g_ui_screen_list_mode == UI_SCREEN_LIST_SQUAD",
    '"Next player", "Club finances", "Previous player", "Next club", "Previous club", "Back"',
    '"Next player", "Renew contract", "Previous player", "Next club", "Previous club", "Back"',
    '"Club squad", "Finance activity", "Season books"',
]
squad_toolbar_wired = all(check in bridge_source for check in squad_toolbar_checks)
ok &= bool(squad_toolbar_wired)
print(f"inline Squad club navigation wiring {'OK' if squad_toolbar_wired else 'BAD'} "
      f"({sum(check in bridge_source for check in squad_toolbar_checks)}/{len(squad_toolbar_checks)} checks)")
collector_start = bridge_source.find("static void ui_screen_collect_targets(uint32_t base) {")
collector_end = bridge_source.find("static void ui_screen_add_history_row", collector_start)
collector = bridge_source[collector_start:collector_end] if collector_start >= 0 and collector_end > collector_start else ""
list_modes = ["UI_SCREEN_LIST_MARKET", "UI_SCREEN_LIST_SALES", "UI_SCREEN_LIST_INBOX",
              "UI_SCREEN_LIST_SQUAD", "UI_SCREEN_LIST_HISTORY", "UI_SCREEN_LIST_FINANCES",
              "UI_SCREEN_LIST_SHORTLIST"]
collected = bool(collector) and all(mode in collector for mode in list_modes)
ok &= bool(collected)
print(f"market list target collection {'OK' if collected else 'BAD'} "
      f"({sum(mode in collector for mode in list_modes)}/{len(list_modes)} modes)")
print("ALL OK" if ok else "FAILED")
