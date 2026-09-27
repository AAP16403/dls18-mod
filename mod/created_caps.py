"""Raise the created-player limit (data_limits/DATA_LIMITS.md section 1, created_v33).

Stock: created ids are 0xFFFE down to 0xFFDF (32 ids), 0xFFDE is the "empty" sentinel in lineups/stats and
0xFFFF is invalid. The id range, not the u8 count, is what stops a 33rd created player.

New scheme: one contiguous block directly below the sentinel,
    LO = 0xFFDE - N  ..  HI = 0xFFDD          (N = created_max, 33..255; default 255 -> 0xFEDF..0xFFDD)
It contains neither 0xFFDE nor 0xFFFF, and it is far above the highest database id (16316), so every
"is this a created player" test stays a single unsigned range compare: (id - LO) < N.
N is capped at 255 by the u8 CCreatePlayer::ms_uCreatedPlayerCount (AddPlayer stores count+1 with strb,
Serialize writes it as u8). With N <= 255 distinct ids the count can never wrap.

Every code site that decides "is this a created player" by id value (full scan of .text for 0xffdf/0xffde/
0xfffe/0xffe0 as movw/movt/literal, mvn/cmn/add-wrap forms, and every GOT reference to ms_uCreatedPlayerCount /
ms_pCreatedPlayers; see the created_v33 report):

  0x205c6c  CCreatePlayer::GetNextAvailableCreatedPlayerID  movw ip,#0xffdf (floor)      -> #LO
  0x205c7c                                                  movw r0,#0xfffe (first id)   -> #HI
  0x205ca8                                                  movw r0,#0xfffe (empty list) -> #HI
  0x20c5c2  CDataBase::IsCreatedPlayerID      movw r1,#0xffdf ; 0x20c5ca cmp r1,#0x20   -> #LO ; #N
  0x2080b4  CDataBase::GetPlayerInfo          movw r0,#0xffdf ; 0x2080ba cmp r0,#0x1f   -> #LO ; #N-1 (bhi)
  0x20da50  CDataBase::GetPlayerInfoSimple    movw r0,#0xffdf ; 0x20da56 cmp r0,#0x1f   -> #LO ; #N-1 (bhi)
  0x251208  CFEMsgSellPlayer::CFEMsgSellPlayer  id >= 0xffdf && id != 0xffff -> created text (LOC 0x84b)
            movw r1,#0xffdf -> #LO ; 0x251210 mvn/adds/uxth (=0xffff) -> movw r1,#0xffde + 2 nop ;
            0x25121a beq -> bhs            => created iff LO <= id < 0xFFDE
  0x2f1832  CTeamLineup::HaveCreatedPlayerInSquad  (same shape) movw ip -> #LO ; 0x2f183e add.w r0,ip,#0x20 ->
            movw r0,#0xffde ; 0x2f1846 bne(found) -> blo(found)
  0x371c3a  CXNetworkGameFlow::GameFlowProcess (pre-game name filter, 1st loop): movw sl -> #LO ;
            0x371c4a add.w r2,sl,#0x20 -> movw r2,#0xffde ; 0x371c52 beq(skip) -> bhs(skip)
  0x371d12  CXNetworkGameFlow::GameFlowProcess (2nd loop): movw r7 -> #LO ; 0x371d2c -> movw r2,#0xffde ;
            0x371d34 beq(skip) -> bhs(skip)

Not changed (verified): all ~87 uses of 0xFFDE are sentinel stores or ==/!= compares; the 0xFFDF->0xFFDE and
0xFFFF compares in the serializers (CTeamLineup/CTeamRoles/CSeasonPlayerState/TTournamentPlayerStat/
TIndividualPlayerStat) are legacy-sentinel conversions; CCreatePlayer::GetPlayer/DeletePlayer/Verify search the
list by id (no range); CTransfers::CanAddCreatedPlayer (0x21032e, squad cap) is left to squad_caps.py.
"""

SENTINEL = 0xFFDE
HI = SENTINEL - 1
STOCK_MAX = 32

# site: (stock bytes, what it is)
STOCK = {
    0x205C6C: bytes.fromhex("4ff6df7c"),          # movw ip, #0xffdf
    0x205C7C: bytes.fromhex("4ff6fe70"),          # movw r0, #0xfffe
    0x205CA8: bytes.fromhex("4ff6fe70"),          # movw r0, #0xfffe
    0x20C5C2: bytes.fromhex("4ff6df71"),          # movw r1, #0xffdf
    0x20C5CA: bytes.fromhex("2029"),              # cmp r1, #0x20
    0x2080B4: bytes.fromhex("4ff6df70"),          # movw r0, #0xffdf
    0x2080BA: bytes.fromhex("1f28"),              # cmp r0, #0x1f      (followed by bhi 0x208126)
    0x20DA50: bytes.fromhex("4ff6df70"),          # movw r0, #0xffdf
    0x20DA56: bytes.fromhex("1f28"),              # cmp r0, #0x1f      (followed by bhi 0x20da84)
    0x251208: bytes.fromhex("4ff6df71"),          # movw r1, #0xffdf
    0x251210: bytes.fromhex("6ff02001203189b2"),  # mvn r1,#0x20 ; adds r1,#0x20 ; uxth r1,r1
    0x25121A: bytes.fromhex("08d0"),              # beq 0x25122e
    0x2F1832: bytes.fromhex("4ff6df7c"),          # movw ip, #0xffdf
    0x2F183E: bytes.fromhex("0cf12000"),          # add.w r0, ip, #0x20
    0x2F1846: bytes.fromhex("04d1"),              # bne 0x2f1852
    0x371C3A: bytes.fromhex("4ff6df7a"),          # movw sl, #0xffdf
    0x371C4A: bytes.fromhex("0af12002"),          # add.w r2, sl, #0x20
    0x371C52: bytes.fromhex("1cd0"),              # beq 0x371c8e
    0x371D12: bytes.fromhex("4ff6df77"),          # movw r7, #0xffdf
    0x371D2C: bytes.fromhex("07f12002"),          # add.w r2, r7, #0x20
    0x371D34: bytes.fromhex("15d0"),              # beq 0x371d62
}

# the context bytes right after the compares that the new immediates rely on (checked, never written)
CONTEXT = {
    0x205C9C: bytes.fromhex("6045"),              # cmp r0, ip  (then mov r0,r2 ; bhi retry)
    0x2080BC: bytes.fromhex("33d8"),              # bhi 0x208126 (not created -> LoadPlayerROM)
    0x20DA58: bytes.fromhex("14d8"),              # bhi 0x20da84
    0x20C5CC: bytes.fromhex("38bf0120"),          # it lo ; movlo r0,#1
    0x25120C: bytes.fromhex("88420ed3"),          # cmp r0,r1 ; blo 0x25122e
    0x251218: bytes.fromhex("8842"),              # cmp r0, r1
    0x2F1836: bytes.fromhex("3ef812306345"),      # ldrh.w r3,[lr,r2,lsl #1] ; cmp r3,ip
    0x2F1842: bytes.fromhex("80b28342"),          # uxth r0,r0 ; cmp r3,r0
    0x371C46: bytes.fromhex("514521d3"),          # cmp r1,sl ; blo 0x371c8e
    0x371C4E: bytes.fromhex("92b29142"),          # uxth r2,r2 ; cmp r1,r2
    0x371D28: bytes.fromhex("b9421ad3"),          # cmp r1,r7 ; blo 0x371d62
    0x371D30: bytes.fromhex("92b29142"),          # uxth r2,r2 ; cmp r1,r2
}


def id_range(created_max):
    """(LO, HI) of the created-id block for a given capacity."""
    return SENTINEL - created_max, HI


def _check_stock(orig, va2off):
    for table in (STOCK, CONTEXT):
        for site, exp in table.items():
            got = bytes(orig[va2off(site):va2off(site) + len(exp)])
            if got != exp:
                raise SystemExit(f"created_caps: unexpected bytes at {site:#x}: {got.hex()} (expected {exp.hex()})")


def code_patches(orig, va2off, asm, created_max=255):
    """Returns {site: bytes}. created_max 32 = stock (no patches); 33..255 moves the id block below 0xFFDE."""
    if not STOCK_MAX <= created_max <= 255:
        raise SystemExit("--created-max must be 32..255 (u8 ms_uCreatedPlayerCount)")
    _check_stock(orig, va2off)
    if created_max == STOCK_MAX:
        return {}
    lo, hi = id_range(created_max)
    out = {}

    def put(site, src):
        code = asm(src, site)
        if len(code) != len(STOCK[site]):
            raise SystemExit(f"created_caps: {site:#x} re-encoded to {len(code)} bytes, expected {len(STOCK[site])}")
        out[site] = code

    # GetNextAvailableCreatedPlayerID: try HI, HI-1, ... LO, else -1
    put(0x205C6C, f"movw ip, #{lo:#x}")
    put(0x205C7C, f"movw r0, #{hi:#x}")
    put(0x205CA8, f"movw r0, #{hi:#x}")
    # IsCreatedPlayerID: (id - LO) < N
    put(0x20C5C2, f"movw r1, #{lo:#x}")
    put(0x20C5CA, f"cmp r1, #{created_max}")
    # GetPlayerInfo / GetPlayerInfoSimple: (id - LO) > N-1 -> database player
    for mov, cmp in ((0x2080B4, 0x2080BA), (0x20DA50, 0x20DA56)):
        put(mov, f"movw r0, #{lo:#x}")
        put(cmp, f"cmp r0, #{created_max - 1}")
    # CFEMsgSellPlayer ctor: created iff LO <= id < 0xFFDE
    put(0x251208, f"movw r1, #{lo:#x}")
    put(0x251210, f"movw r1, #{SENTINEL:#x}\nnop\nnop")
    put(0x25121A, "bhs 0x25122e")
    # CTeamLineup::HaveCreatedPlayerInSquad: found iff LO <= id < 0xFFDE (0xFFDE = empty slot)
    put(0x2F1832, f"movw ip, #{lo:#x}")
    put(0x2F183E, f"movw r0, #{SENTINEL:#x}")
    put(0x2F1846, "blo 0x2f1852")
    # CXNetworkGameFlow::GameFlowProcess: profanity-filter created players' names in the pre-game data
    put(0x371C3A, f"movw sl, #{lo:#x}")
    put(0x371C4A, f"movw r2, #{SENTINEL:#x}")
    put(0x371C52, "bhs 0x371c8e")
    put(0x371D12, f"movw r7, #{lo:#x}")
    put(0x371D2C, f"movw r2, #{SENTINEL:#x}")
    put(0x371D34, "bhs 0x371d62")
    return out
