"""Remove the CPU's hidden advantages: the penalty keeper that reads the shot, and difficulty spikes (ai_fair, v34).

Penalties. PM_PenaltyCPUAISave 0x29f164 picks the CPU keeper's dive (a918/a91c, copied to a908/a90c) at
the moment the user's taker strikes (taker action time 2):
  0x29f2c2  XSYS_Random(256) <= 0x5f (37.5%): dive = the user's real aim / 2 (a920/a924)   <- reads the shot
  else      a random dive, redrawn until it is >= 0x400 away from the aim (0x29f34a loop)  <- forced miss
GAI_GKProcessPenaltyAction 0x2a1344 snaps the keeper onto the ball only when the guess lands within reach
(distance^2 < 0x9c4 in 1024 units), so in stock every save came from the 37.5% read, however well the
shot was placed. The CPU taker (PM_PenaltyCPUAITake) aims at random and never sees the user's keeper, and
the user's own keeper dives where the user swipes. The fix makes the CPU keeper guess blind:
  0x29f2cc  bgt -> b   never take the read branch
  0x29f34a  bge -> b   keep the first uniform random dive (no redraw away from the shot)
Saves now depend on placement: a guess inside the keeper's reach saves, a corner beyond it scores.

Difficulty spikes. The CPU's TCPUAIDifficulty (0..400, GDIFF_SetDifficulty 0x299e3e) is set at kick-off
and again at half-time (CPUAI_Init from GL_SwapTeams):
  CPUAI_UpdateDifficulty 0x299c30: base + 8 * (goal margin beyond 5) + dynamic, where
  CPUAI_UpdateDifficultyDynamic 0x299ba0 sets dynamic = +12 when the CPU trails by 2+ goals (-12 when it
  leads by 2+), so a trailing CPU gets a harder second half.
  0x299cc6  cbz -> b   use the base difficulty only (no score or half-time rubber band)
Between matches the profile's dynamic difficulty (CProfileGameSettings, +0x6050 in CMyProfile) moves by
  CCore::ProcessPostMatch 0x2045e0: (goal difference - rating gap / 15) * GetVar(0xf)=5, halved on a win,
so one big win jumps the next opponents up. The step is lowered to `dd_step` (default 2):
  0x2045e0  movs r0,#0xf; blx CConfig::GetVar -> movs r0,#dd_step; nop; nop
GetDynamicDifficulty 0x37a00c adds config var 0x25 Cheat_Difficulty when IsUserCheat() flags the profile
(server cheat rules; a modded economy can trip them), a hidden jump on top of the normal difficulty:
  0x37a016  cbz -> b   never add the cheat bonus (IsUserCheat itself is untouched)
"""

STOCK = {
    0x29F2CC: bytes.fromhex("1bdc"),            # bgt 0x29f306
    0x29F34A: bytes.fromhex("1eda"),            # bge 0x29f38a
    0x299CC6: bytes.fromhex("80b1"),            # cbz r0, 0x299cea
    0x37A016: bytes.fromhex("50b1"),            # cbz r0, 0x37a02e
    0x2045E0: bytes.fromhex("0f20bcf78eee"),    # movs r0,#0xf; blx CConfig::GetVar
}
DD_STEP_STOCK = 5


def code_patches(orig, va2off, asm, dd_step=2):
    """Returns {site: bytes}. Every site is checked against the stock bytes and re-encoded to the same size."""
    if not 0 <= dd_step <= 255:
        raise SystemExit("--dd-step must be 0..255 (movs imm8)")
    for site, exp in STOCK.items():
        got = bytes(orig[va2off(site):va2off(site) + len(exp)])
        if got != exp:
            raise SystemExit(f"ai_fair: unexpected bytes at {site:#x}: {got.hex()} (expected {exp.hex()})")
    src = {
        0x29F2CC: "b 0x29f306",
        0x29F34A: "b 0x29f38a",
        0x299CC6: "b 0x299cea",
        0x37A016: "b 0x37a02e",
    }
    if dd_step != DD_STEP_STOCK:
        src[0x2045E0] = f"movs r0, #{dd_step}\nnop\nnop"
    out = {}
    for site, s in src.items():
        code = asm(s, site)
        if len(code) != len(STOCK[site]):
            raise SystemExit(f"ai_fair: {site:#x} re-encoded to {len(code)} bytes, expected {len(STOCK[site])}")
        out[site] = code
    return out
