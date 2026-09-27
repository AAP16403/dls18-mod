from __future__ import annotations

import argparse
import math
import pathlib
import struct


def printable(data: bytes) -> str:
    return "".join(chr(value) if 32 <= value < 127 else "." for value in data)


def main() -> None:
    parser = argparse.ArgumentParser(description="Read-only hex and numeric view of a binary file.")
    parser.add_argument("path", type=pathlib.Path, help="Path to an extracted or original file")
    parser.add_argument("--offset", type=lambda s: int(s, 0), default=0, help="Starting byte offset (decimal or 0x-prefixed hex)")
    parser.add_argument("--length", type=lambda s: int(s, 0), default=256, help="Number of bytes to show; use 0 to show to end-of-file")
    args = parser.parse_args()
    if args.offset < 0 or args.length < 0:
        parser.error("offset and length must be non-negative")
    path = args.path.resolve()
    if not path.is_file():
        parser.error(f"not a file: {path}")
    size = path.stat().st_size
    if args.offset > size:
        parser.error(f"offset {args.offset} is past file end ({size} bytes)")
    requested = size - args.offset if args.length == 0 else min(args.length, size - args.offset)
    with path.open("rb") as stream:
        stream.seek(args.offset)
        data = stream.read(requested)
    end = args.offset + len(data)
    print(f"File: {path}")
    print(f"File bytes: {size}; viewed range: [{args.offset}, {end}) ({len(data)} bytes)")
    print("Offset      Bytes (hex)                                      Printable")
    for local in range(0, len(data), 16):
        chunk = data[local:local + 16]
        print(f"0x{args.offset + local:08x}  {chunk.hex(' '):<47}  {printable(chunk)}")
    print("\nAligned little-endian numeric interpretations")
    print("Offset      Bytes          u8 values                         u16 values          u32       i32       f32")
    # Anchor groups to absolute four-byte offsets, keeping the requested range intact.
    cursor = 0
    while cursor < len(data):
        absolute = args.offset + cursor
        aligned_start = absolute - (absolute % 4)
        if absolute != aligned_start:
            chunk = data[cursor:cursor + min(4 - absolute % 4, len(data) - cursor)]
            u16_text = u32_text = i32_text = f32_text = ""
            values = " ".join(f"{value:3d}" for value in chunk)
            print(f"0x{absolute:08x}  {chunk.hex(' '):<14} {values:<32} {u16_text:<19} {u32_text:<9} {i32_text:<9} {f32_text}")
            cursor += len(chunk)
            continue
        chunk = data[cursor:cursor + 4]
        u8_text = " ".join(f"{value:3d}" for value in chunk)
        u16_text = " ".join(str(v) for v in struct.unpack_from("<" + "H" * (len(chunk) // 2), chunk, 0)) if len(chunk) >= 2 else ""
        u32_text = str(struct.unpack_from("<I", chunk, 0)[0]) if len(chunk) == 4 else ""
        i32_text = str(struct.unpack_from("<i", chunk, 0)[0]) if len(chunk) == 4 else ""
        f32_value = struct.unpack_from("<f", chunk, 0)[0] if len(chunk) == 4 else None
        f32_text = (repr(f32_value) if math.isfinite(f32_value) else repr(f32_value)) if f32_value is not None else ""
        print(f"0x{absolute:08x}  {chunk.hex(' '):<14} {u8_text:<32} {u16_text:<19} {u32_text:<9} {i32_text:<9} {f32_text}")
        cursor += len(chunk)


if __name__ == "__main__":
    main()
