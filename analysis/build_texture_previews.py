from __future__ import annotations

import csv
import hashlib
import pathlib
import struct
import sys
import zlib

from PIL import Image


PROJECT = pathlib.Path(__file__).resolve().parents[1]
UNPACKED = PROJECT / "unpacked"
OUTPUT = PROJECT / "previews" / "textures"
VENDOR = pathlib.Path(__file__).resolve().parent / "vendor"
sys.path.insert(0, str(VENDOR))
import texture2ddecoder


FORMAT_NAMES = {
    0: "unknown-0",
    1: "BGR565",
    2: "ABGR4444",
    3: "RGBA32",
    8: "A8-gray",
    14: "ETC1-RGB",
    33: "ETC2-RGBA",
    256: "unknown-256",
}

DLS26_FORMAT_NAMES = {
    0: "TF5650",
    1: "TF5551",
    2: "TF4444",
    3: "TF8888",
    8: "TF8",
    9: "TF88",
    14: "TFETC1",
    33: "TFETC2-RGBA",
    256: "TF5650-cubemap",
    270: "TFETC1-cubemap",
}
TEXTURE_PROFILE = "dls18"


def decode_pixels(fmt: int, width: int, height: int, payload: bytes) -> Image.Image | None:
    if TEXTURE_PROFILE == "dls26" and fmt & 0x100:
        return decode_cubemap(fmt & 0xFF, width, height, payload)

    count = width * height
    if TEXTURE_PROFILE == "dls26" and fmt == 0:
        out = bytearray(count * 3)
        for i in range(count):
            value = struct.unpack_from("<H", payload, i * 2)[0]
            red = ((value >> 11) & 31) * 255 // 31
            green = ((value >> 5) & 63) * 255 // 63
            blue = (value & 31) * 255 // 31
            out[i * 3:i * 3 + 3] = bytes((red, green, blue))
        return Image.frombytes("RGB", (width, height), bytes(out))
    if fmt == 1:
        if TEXTURE_PROFILE == "dls26":
            out = bytearray(count * 4)
            for i in range(count):
                value = struct.unpack_from("<H", payload, i * 2)[0]
                red = ((value >> 11) & 31) * 255 // 31
                green = ((value >> 6) & 31) * 255 // 31
                blue = ((value >> 1) & 31) * 255 // 31
                alpha = (value & 1) * 255
                out[i * 4:i * 4 + 4] = bytes((red, green, blue, alpha))
            return Image.frombytes("RGBA", (width, height), bytes(out))
        out = bytearray(count * 3)
        for i in range(count):
            value = struct.unpack_from("<H", payload, i * 2)[0]
            blue = ((value >> 11) & 31) * 255 // 31
            green = ((value >> 5) & 63) * 255 // 63
            red = (value & 31) * 255 // 31
            out[i * 3:i * 3 + 3] = bytes((red, green, blue))
        return Image.frombytes("RGB", (width, height), bytes(out))
    if fmt == 2:
        out = bytearray(count * 4)
        for i in range(count):
            value = struct.unpack_from("<H", payload, i * 2)[0]
            red = (value & 15) * 17
            green = ((value >> 4) & 15) * 17
            blue = ((value >> 8) & 15) * 17
            alpha = ((value >> 12) & 15) * 17
            out[i * 4:i * 4 + 4] = bytes((red, green, blue, alpha))
        return Image.frombytes("RGBA", (width, height), bytes(out))
    if fmt == 3:
        return Image.frombytes("RGBA", (width, height), payload[:count * 4])
    if fmt == 8:
        return Image.frombytes("L", (width, height), payload[:count])
    if TEXTURE_PROFILE == "dls26" and fmt == 9:
        gray = bytes(payload[index] for index in range(0, count * 2, 2))
        return Image.frombytes("L", (width, height), gray)
    if fmt == 14:
        decoded = texture2ddecoder.decode_etc1(payload[:((width + 3) // 4) * ((height + 3) // 4) * 8], width, height)
        return Image.frombytes("RGBA", (width, height), decoded, "raw", "BGRA")
    if fmt == 33:
        decoded = texture2ddecoder.decode_etc2a8(payload[:((width + 3) // 4) * ((height + 3) // 4) * 16], width, height)
        return Image.frombytes("RGBA", (width, height), decoded, "raw", "BGRA")
    return None


def decode_cubemap(fmt: int, width: int, height: int, payload: bytes) -> Image.Image | None:
    if fmt in (0, 1, 2):
        face_bytes = width * height * 2
    elif fmt == 3:
        face_bytes = width * height * 4
    elif fmt == 14:
        face_bytes = ((width + 3) // 4) * ((height + 3) // 4) * 8
    else:
        return None
    if len(payload) < face_bytes * 6:
        return None

    faces = [
        decode_pixels(fmt, width, height, payload[index * face_bytes:(index + 1) * face_bytes])
        for index in range(6)
    ]
    if any(face is None for face in faces):
        return None
    assert all(face is not None for face in faces)
    mode = "RGBA" if any(face.mode == "RGBA" for face in faces if face is not None) else "RGB"
    atlas = Image.new(mode, (width * 3, height * 2), (0, 0, 0, 0) if mode == "RGBA" else (0, 0, 0))
    for index, face in enumerate(faces):
        assert face is not None
        if face.mode != mode:
            face = face.convert(mode)
        atlas.paste(face, ((index % 3) * width, (index // 3) * height))
    return atlas


def main() -> None:
    sources = [
        UNPACKED / "apk" / "assets/data",
        UNPACKED / "kpx" / "env",
        UNPACKED / "kpx" / "models",
    ]
    files = sorted(p for source in sources for p in source.rglob("*.ftc"))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    catalog_path = PROJECT / "catalog" / "ftc_textures.csv"
    catalog_path.parent.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    rows: list[dict[str, object]] = []
    for index, source in enumerate(files, 1):
        raw = source.read_bytes()
        if len(raw) < 20 or raw[:4] not in (b"FTC3", b"FTC4"):
            rows.append({"source_path": source.relative_to(UNPACKED).as_posix(), "status": "unknown-header", "source_sha256": hashlib.sha256(raw).hexdigest()})
            continue
        raw_size = struct.unpack_from("<I", raw, 4)[0]
        flags = raw[8:12].hex()
        width, height, fmt, mip_count = struct.unpack_from("<4H", raw, 12)
        payload = zlib.decompress(raw[20:])
        try:
            image = decode_pixels(fmt, width, height, payload)
            status = "previewed" if image else "format-not-mapped"
        except Exception as exc:
            image = None
            status = f"decode-error:{type(exc).__name__}"
        relative = source.relative_to(UNPACKED)
        output_relative = relative.with_suffix(".png")
        output = OUTPUT / output_relative
        if image is not None:
            output.parent.mkdir(parents=True, exist_ok=True)
            image.save(output, format="PNG", optimize=False, compress_level=1)
        counts[status] = counts.get(status, 0) + 1
        rows.append({
            "source_path": relative.as_posix(),
            "preview_path": output.relative_to(PROJECT).as_posix() if image else "",
            "magic": raw[:4].decode("ascii", "replace"),
            "file_bytes": len(raw),
            "decoded_payload_bytes": len(payload),
            "header_raw_size": raw_size,
            "flags_hex": flags,
            "width": width,
            "height": height,
            "format_code": fmt,
            "format_name": (DLS26_FORMAT_NAMES if TEXTURE_PROFILE == "dls26" else FORMAT_NAMES).get(fmt, f"unknown-{fmt}"),
            "mip_count": mip_count,
            "status": status,
            "payload_size_check": "exact" if len(payload) == raw_size else f"declared {raw_size}; decompressed {len(payload)}",
            "source_sha256": hashlib.sha256(raw).hexdigest(),
        })
        if index % 500 == 0:
            print(f"Processed {index}/{len(files)} FTC files", flush=True)
    columns = ["source_path", "preview_path", "magic", "file_bytes", "decoded_payload_bytes", "header_raw_size", "flags_hex", "width", "height", "format_code", "format_name", "mip_count", "status", "payload_size_check", "source_sha256"]
    with catalog_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"FTCs={len(files)}; previewed={counts.get('previewed', 0)}; unmapped={counts.get('format-not-mapped', 0)}; catalog={catalog_path}")


if __name__ == "__main__":
    main()
