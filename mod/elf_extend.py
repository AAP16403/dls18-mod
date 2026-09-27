"""Give libDLS18.so room to grow: two new PT_LOAD segments after the existing image.

  CAVE3   R+X  code_size bytes of new code (file-backed, placed at the end of the file)
  MODDATA R+W  data_size bytes of zero-filled mod data (memsz only, no file bytes)

The program header table has no free slot (8 entries, the notes follow it), so it is moved into the
new code segment with two more entries, the way patchelf adds segments. The loader (bionic
ElfReader) needs: PT_LOAD entries in ascending vaddr order, p_offset % p_align == p_vaddr % p_align,
file ranges inside the file, and PT_PHDR inside a PT_LOAD. check() verifies all of that.

The existing image is not moved, so every address in the game, the caves and the market stays valid.
CAVE3 is within +/-16 MB of .text (b.w / bl reach), so cave code there uses the same PC-relative
rules as CAVE and CAVE2 (mod/PATCHING_RULES.md).
"""
import struct

PT_LOAD, PT_PHDR = 1, 6
PF_X, PF_W, PF_R = 1, 2, 4
PAGE = 0x1000
PHDR_SIZE = 32
MARKER = 0x4850434D                 # "MCPH" at the old table offset: moved-table/CAVE3/MODDATA vaddrs


def _phdrs(b):
    phoff, = struct.unpack_from("<I", b, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", b, 0x2A)
    assert phentsize == PHDR_SIZE
    return [list(struct.unpack_from("<8I", b, phoff + i * PHDR_SIZE)) for i in range(phnum)]


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def extend(lib, code_size=0x40000, data_size=0x10000):
    """Returns (new_lib, layout). layout: cave3_va, cave3_off, cave3_size (usable, after the phdrs),
    moddata_va, moddata_size, phdr_va."""
    b = bytearray(lib)
    ph = _phdrs(b)
    if any(p[0] == PT_LOAD and p[5] & PF_X and p[2] > 0x800000 for p in ph):
        raise SystemExit("elf_extend: library already extended")
    loads = [p for p in ph if p[0] == PT_LOAD]
    end_va = max(p[2] + p[5] for p in loads)
    seg_va = align(end_va)
    seg_off = align(len(b))
    # keep vaddr and offset congruent modulo the page size
    seg_va += (seg_off - seg_va) % PAGE
    new_count = len(ph) + 2
    table_size = new_count * PHDR_SIZE
    cave3_va = seg_va + align(table_size, 16)
    cave3_off = seg_off + (cave3_va - seg_va)
    seg_size = (cave3_va - seg_va) + code_size
    data_va = align(seg_va + seg_size)
    b.extend(b"\0" * (seg_off + seg_size - len(b)))
    for p in ph:
        if p[0] == PT_PHDR:
            p[1], p[2], p[3], p[4], p[5] = seg_off, seg_va, seg_va, table_size, table_size
    code_seg = [PT_LOAD, seg_off, seg_va, seg_va, seg_size, seg_size, PF_R | PF_X, PAGE]
    data_seg = [PT_LOAD, 0, data_va, data_va, 0, data_size, PF_R | PF_W, PAGE]
    # insert the new loads right after the last existing PT_LOAD (loads stay sorted by vaddr)
    last = max(i for i, p in enumerate(ph) if p[0] == PT_LOAD)
    ph[last + 1:last + 1] = [code_seg, data_seg]
    for i, p in enumerate(ph):
        struct.pack_into("<8I", b, seg_off + i * PHDR_SIZE, *p)
    struct.pack_into("<I", b, 0x1C, seg_off)
    struct.pack_into("<H", b, 0x2C, new_count)
    # the old table at 0x34 is no longer read by the loader but stays mapped (first page, read-only):
    # leave a marker there so runtime code finds the moved table and CAVE3 without scanning memory
    old_off = struct.unpack_from("<I", lib, 0x1C)[0]
    b[old_off:old_off + len(ph) * PHDR_SIZE - 2 * PHDR_SIZE] = bytes(1) * (len(ph) * PHDR_SIZE - 2 * PHDR_SIZE)
    struct.pack_into("<IIII", b, old_off, MARKER, seg_va, cave3_va, data_va)
    layout = dict(cave3_va=cave3_va, cave3_off=cave3_off, cave3_size=code_size,
                  moddata_va=data_va, moddata_size=data_size, phdr_va=seg_va)
    check(b, layout)
    return b, layout


def check(b, layout=None):
    ph = _phdrs(b)
    loads = [p for p in ph if p[0] == PT_LOAD]
    prev_end = -1
    for p in loads:
        typ, off, va, pa, fsz, msz, flg, al = p
        assert al == PAGE, "p_align"
        assert off % al == va % al, "offset/vaddr congruence"
        assert off + fsz <= len(b), "file range outside the file"
        assert fsz <= msz, "filesz > memsz"
        assert va >= prev_end, "PT_LOAD not in ascending vaddr order / overlapping"
        prev_end = va + msz
    phdr = [p for p in ph if p[0] == PT_PHDR]
    assert len(phdr) == 1
    pva, psz = phdr[0][2], phdr[0][5]
    assert any(p[2] <= pva and pva + psz <= p[2] + p[4] for p in loads), "PT_PHDR not inside a PT_LOAD"
    phoff, = struct.unpack_from("<I", b, 0x1C)
    assert phoff == phdr[0][1], "e_phoff != PT_PHDR offset"
    if layout:
        c = layout["cave3_va"]
        assert any(p[2] <= c and c + layout["cave3_size"] <= p[2] + p[4] and p[6] & PF_X for p in loads)
        assert c - 0x1C0000 < 0x1000000, "CAVE3 out of b.w range from .text"
    return True


def va2off(b, va):
    """File offset of a virtual address, via the (possibly extended) program headers."""
    for p in _phdrs(b):
        if p[0] == PT_LOAD and p[2] <= va < p[2] + p[4]:
            return p[1] + (va - p[2])
    raise KeyError(hex(va))


def load_image(b, base, size=0xA00000):
    """Memory image as the loader maps it (for Unicorn tests): each PT_LOAD at base + vaddr."""
    img = bytearray(size)
    for p in _phdrs(b):
        if p[0] == PT_LOAD and p[4]:
            img[p[2]:p[2] + p[4]] = b[p[1]:p[1] + p[4]]
    return img


if __name__ == "__main__":
    import sys
    src = open(sys.argv[1], "rb").read()
    out, lay = extend(src)
    print({k: hex(v) for k, v in lay.items()})
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(out)
