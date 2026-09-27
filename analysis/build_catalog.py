from __future__ import annotations

import csv
import hashlib
import pathlib
import shutil
import struct
import zipfile
from collections import Counter
from datetime import datetime, timezone


PROJECT = pathlib.Path(__file__).resolve().parents[1]
ORIGINAL = PROJECT / "original"
UNPACKED = PROJECT / "unpacked"
CATALOG = PROJECT / "catalog"
APK = ORIGINAL / "DLS18_5.064.apk"
OBB = ORIGINAL / "main.82.com.firsttouchgames.dls3.obb"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_destination(root: pathlib.Path, relative: str) -> pathlib.Path:
    path = pathlib.PurePosixPath(relative)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError(f"Unsafe archive path: {relative!r}")
    result = root.joinpath(*path.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes output directory: {relative!r}")
    return result


def extract_zip(archive: pathlib.Path, destination: pathlib.Path, records: list[dict]) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            relative = info.filename.replace("\\", "/")
            output = safe_destination(destination, relative)
            output.parent.mkdir(parents=True, exist_ok=True)
            data = zf.read(info)
            output.write_bytes(data)
            records.append({
                "container": archive.name,
                "path": relative,
                "kind": "outer-archive-entry",
                "size": len(data),
                "stored_size": info.compress_size,
                "compression": {0: "stored", 8: "deflate"}.get(info.compress_type, str(info.compress_type)),
                "offset": "",
                "metadata_u32": "",
                "metadata_utc": "",
                "crc32": f"{info.CRC:08x}",
                "sha256": sha256_bytes(data),
                "magic_hex": data[:16].hex(" "),
                "extracted_path": str(output.relative_to(PROJECT)).replace("\\", "/"),
            })


def kpx_name(data: bytes, names_offset: int, names_size: int, offset: int) -> str:
    start = names_offset + offset
    end_limit = names_offset + names_size
    if offset >= names_size or start < names_offset or start >= end_limit:
        raise ValueError(f"Name offset outside string table: {offset}")
    end = data.find(b"\0", start, end_limit)
    if end < 0:
        raise ValueError(f"Unterminated KPX name at offset {offset}")
    return data[start:end].decode("utf-8", errors="replace")


def parse_kpx(data: bytes) -> tuple[bytes, list[dict]]:
    if data[:4] not in (b"\x00KPX", b"\x01KPX"):
        raise ValueError(f"Unsupported KPX marker: {data[:4]!r}")
    if len(data) < 16:
        raise ValueError("KPX header is truncated")
    folder_count, file_count, names_size = struct.unpack_from("<III", data, 4)
    folder_offset = 16
    file_offset = folder_offset + folder_count * 20
    names_offset = file_offset + file_count * 24
    if names_offset + names_size > len(data):
        raise ValueError("KPX tables/string table exceed archive length")

    folders = [struct.unpack_from("<5I", data, folder_offset + i * 20) for i in range(folder_count)]
    files = [struct.unpack_from("<6I", data, file_offset + i * 24) for i in range(file_count)]
    output: list[dict] = []
    visiting: set[int] = set()

    def walk(folder_index: int, parent: pathlib.PurePosixPath) -> None:
        if folder_index in visiting:
            raise ValueError(f"KPX folder cycle at index {folder_index}")
        if not 0 <= folder_index < folder_count:
            raise ValueError(f"KPX folder index out of range: {folder_index}")
        visiting.add(folder_index)
        name_offset, file_count_here, child_count, file_pos, child_pos = folders[folder_index]
        folder_name = kpx_name(data, names_offset, names_size, name_offset)
        # Folder zero is the KPX root sentinel: its name offset aliases the
        # first entry name in these archives, so only child folder names form paths.
        current = parent if folder_index == 0 else (parent / folder_name if folder_name else parent)
        if file_pos + file_count_here > file_count or child_pos + child_count > folder_count:
            raise ValueError(f"KPX folder ranges out of bounds at index {folder_index}")
        for index in range(file_pos, file_pos + file_count_here):
            name_off, size, offset, compressed, meta, packed_size = files[index]
            filename = kpx_name(data, names_offset, names_size, name_off)
            if compressed not in (0, 1):
                raise ValueError(f"Unknown KPX compression marker {compressed} for {filename}")
            read_size = packed_size if compressed else size
            if offset + read_size > len(data):
                raise ValueError(f"KPX payload out of bounds for {filename}")
            stored = data[offset:offset + read_size]
            if compressed:
                import zlib
                try:
                    decoded = zlib.decompress(stored)
                except zlib.error:
                    decoded = zlib.decompress(stored, -15)
            else:
                decoded = stored
            if len(decoded) != size:
                raise ValueError(f"Decoded size mismatch for {filename}: {len(decoded)} != {size}")
            relative = (current / filename).as_posix()
            output.append({
                "path": relative,
                "size": size,
                "stored_size": read_size,
                "compression": "zlib" if compressed else "stored",
                "offset": offset,
                "metadata_u32": meta,
                "metadata_utc": datetime.fromtimestamp(meta, timezone.utc).isoformat() if 1_200_000_000 <= meta <= 2_100_000_000 else "",
                "crc32": f"{__import__('zlib').crc32(decoded) & 0xffffffff:08x}",
                "sha256": sha256_bytes(decoded),
                "magic_hex": decoded[:16].hex(" "),
                "data": decoded,
                "folder_index": folder_index,
                "file_index": index,
            })
        for child_index in range(child_pos, child_pos + child_count):
            walk(child_index, current)
        visiting.remove(folder_index)

    if folder_count:
        walk(0, pathlib.PurePosixPath())
    if len(output) != file_count:
        raise ValueError(f"KPX walk found {len(output)} of {file_count} file records")
    return data[:4], output


def write_csv(path: pathlib.Path, rows: list[dict], columns: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if columns is None:
        columns = ["container", "path", "kind", "size", "stored_size", "compression", "offset", "metadata_u32", "metadata_utc", "crc32", "sha256", "magic_hex", "extracted_path"]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_family_summary(rows: list[dict]) -> None:
    groups: dict[tuple[str, str, str], dict[str, int]] = {}
    for row in rows:
        path = pathlib.PurePosixPath(row["path"])
        extension = path.suffix.lower() or "[none]"
        # Three path components keep major game asset folders visible without
        # producing a row for every individual filename.
        family = "/".join(path.parts[:3]) or "."
        key = (row["container"], family, extension)
        item = groups.setdefault(key, {"file_count": 0, "decoded_bytes": 0})
        item["file_count"] += 1
        item["decoded_bytes"] += int(row["size"])
    summary_rows = [
        {"container": container, "path_family": family, "extension": ext, **stats}
        for (container, family, ext), stats in sorted(groups.items())
    ]
    write_csv(
        CATALOG / "file_family_summary.csv",
        summary_rows,
        ["container", "path_family", "extension", "file_count", "decoded_bytes"],
    )


def main() -> None:
    if not APK.is_file() or not OBB.is_file():
        raise FileNotFoundError("Expected original APK and OBB under DLS18_project/original")
    CATALOG.mkdir(parents=True, exist_ok=True)
    apk_rows: list[dict] = []
    obb_rows: list[dict] = []
    pak_rows: list[dict] = []
    extract_zip(APK, UNPACKED / "apk", apk_rows)
    extract_zip(OBB, UNPACKED / "obb", obb_rows)

    pak_sources = [
        ("anims.pak", UNPACKED / "apk" / "assets/data/anims/anims.pak"),
        ("nis.pak", UNPACKED / "apk" / "assets/data/nis.pak"),
        ("env.pak", UNPACKED / "obb" / "data/env.pak"),
        ("models.pak", UNPACKED / "obb" / "data/models.pak"),
    ]
    pak_summaries: list[dict] = []
    for pak_name, pak_path in pak_sources:
        marker, entries = parse_kpx(pak_path.read_bytes())
        destination = UNPACKED / "kpx" / pak_name.removesuffix(".pak")
        destination.mkdir(parents=True, exist_ok=True)
        seen: Counter[str] = Counter()
        ext_counts: Counter[str] = Counter()
        decoded_bytes = 0
        for entry in entries:
            relative = entry["path"]
            seen[relative.casefold()] += 1
            target_relative = relative
            if seen[relative.casefold()] > 1:
                p = pathlib.PurePosixPath(relative)
                target_relative = (p.parent / f"{p.stem}__duplicate_{seen[relative.casefold()]}{p.suffix}").as_posix()
            target = safe_destination(destination, target_relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(entry.pop("data"))
            decoded_bytes += entry["size"]
            ext_counts[pathlib.PurePosixPath(relative).suffix.lower() or "[none]"] += 1
            pak_rows.append({
                "container": pak_name,
                "path": relative,
                "kind": "kpx-entry",
                "size": entry["size"],
                "stored_size": entry["stored_size"],
                "compression": entry["compression"],
                "offset": entry["offset"],
                "metadata_u32": entry["metadata_u32"],
                "metadata_utc": entry["metadata_utc"],
                "crc32": entry["crc32"],
                "sha256": entry["sha256"],
                "magic_hex": entry["magic_hex"],
                "extracted_path": str(target.relative_to(PROJECT)).replace("\\", "/"),
            })
        pak_summaries.append({"name": pak_name, "marker": f"{marker[0]:02x}KPX", "files": len(entries), "decoded_bytes": decoded_bytes, "extensions": ext_counts})

    write_csv(CATALOG / "apk_entries.csv", apk_rows)
    write_csv(CATALOG / "obb_entries.csv", obb_rows)
    write_csv(CATALOG / "pak_entries.csv", pak_rows)
    write_csv(CATALOG / "all_files.csv", apk_rows + obb_rows + pak_rows)
    write_family_summary(apk_rows + obb_rows + pak_rows)
    source_rows = [
        {"filename": path.name, "path": str(path.relative_to(PROJECT)).replace("\\", "/"), "size_bytes": path.stat().st_size, "sha256": sha256_file(path)}
        for path in (APK, OBB)
    ]
    write_csv(CATALOG / "source_manifest.csv", source_rows, ["filename", "path", "size_bytes", "sha256"])

    # A prior exploratory extraction used the wrong KPX root-sentinel handling.
    # Keep it intact for provenance, but make the canonical location unambiguous.
    preliminary = UNPACKED / "paks"
    if preliminary.exists():
        preliminary.mkdir(parents=True, exist_ok=True)
        (preliminary / "READ_ME_PRELIMINARY.txt").write_text(
            "This is a preliminary KPX extraction from an earlier parser pass. Its root-sentinel path handling was incorrect. Use ../kpx/ for the canonical extraction and catalog/pak_entries.csv for authoritative paths. This folder is retained as-is for provenance.\n",
            encoding="utf-8",
        )

    import zlib
    db_rows: list[dict] = []
    db_output = UNPACKED / "db_decoded"
    db_output.mkdir(parents=True, exist_ok=True)
    for source in sorted((UNPACKED / "apk" / "assets/data/db").glob("*.dat")):
        compressed = source.read_bytes()
        decoded = zlib.decompress(compressed)
        target = db_output / f"{source.name}.decoded.bin"
        target.write_bytes(decoded)
        db_rows.append({
            "container": "assets/data/db/" + source.name,
            "path": target.name,
            "kind": "zlib-decoded-database-payload",
            "size": len(decoded),
            "stored_size": len(compressed),
            "compression": "zlib",
            "offset": 0,
            "metadata_u32": "",
            "metadata_utc": "",
            "crc32": f"{zlib.crc32(decoded) & 0xffffffff:08x}",
            "sha256": sha256_bytes(decoded),
            "magic_hex": decoded[:16].hex(" "),
            "extracted_path": str(target.relative_to(PROJECT)).replace("\\", "/"),
        })
    write_csv(CATALOG / "db_payloads.csv", db_rows)

    report = [
        "# DLS 18 v5.064 file map",
        "",
        "Source files are preserved in `original/`. `unpacked/` contains the APK/OBB ZIP contents and every file decoded from the four KPX packages. CSV catalogs include sizes, storage method, offsets, SHA-256, CRC32, and the first 16 bytes in hex.",
        "",
        f"- APK outer entries: **{len(apk_rows)}**, uncompressed total **{sum(r['size'] for r in apk_rows):,} bytes**.",
        f"- OBB outer entries: **{len(obb_rows)}**, uncompressed total **{sum(r['size'] for r in obb_rows):,} bytes**.",
        f"- KPX inner entries: **{len(pak_rows)}**, decoded total **{sum(r['size'] for r in pak_rows):,} bytes**.",
        "",
        "## Four KPX packages",
        "",
        "| Package | Marker | Files | Decoded bytes | Main contents |",
        "|---|---:|---:|---:|---|",
    ]
    descriptions = {
        "anims.pak": "`.sat` animation clips and `animdb.adb` animation index",
        "nis.pak": "XML sequences/cut-scene definitions",
        "env.pak": "stadium, dressing-room, camera, and environment assets",
        "models.pak": "models, player/ball/stadium/crowd assets, and textures",
    }
    for s in pak_summaries:
        ext_text = ", ".join(f"{k}: {v}" for k, v in s["extensions"].most_common())
        report.append(f"| `{s['name']}` | `{s['marker']}` | {s['files']:,} | {s['decoded_bytes']:,} | {descriptions[s['name']]} |")
        report.append("")
        report.append(f"**{s['name']} extension counts:** {ext_text}")
        report.append("")
    report.extend([
        "## APK folders to start with",
        "",
        "- `unpacked/apk/assets/data/db/`: compressed player, team, manager, and roster-link databases. `unpacked/db_decoded/*.decoded.bin` are the zlib-decoded payloads; see `DATABASE_LAYOUT.md` and the per-record numeric views.",
        "- `unpacked/kpx/anims/`: per-action `.sat` animation payloads and `animdb.adb` index.",
        "- `unpacked/kpx/nis/`: compressed XML scene sequences and animation references.",
        "- `unpacked/kpx/models/` and `unpacked/kpx/env/`: `.ftm` models and `.ftc` textures.",
        "- `unpacked/apk/assets/data/shaders/`: shader variants (`.gl2`, `.gl3`, `.vk`).",
        "- `unpacked/apk/assets/data/audio/`: menu music (`.m4a`) and sound-event bank (`.bnk`).",
        "- `unpacked/obb/data/audio/`: commentary and crowd banks.",
        "- `unpacked/apk/assets/data/text/`: language/name tables (`.xlc`), fonts, and blacklist.",
        "- `unpacked/apk/assets/data/game/`: achievements, leaderboards, ad boards, server/certificate, and static network geometry.",
        "",
        "## Format notes",
        "",
        "- The APK and OBB are ZIP containers. The OBB itself contains two `XBNK` audio banks plus `env.pak` and `models.pak`.",
        "- All four `.pak` files use the `00KPX` header in this build. The KPX table supplies folder/file names, byte offsets, decoded and packed lengths, and a per-file metadata word. Compression flag `1` is zlib; flag `0` stores bytes directly.",
        "- The KPX metadata word repeatedly maps to 2018 Unix timestamps in this sample; it is cataloged as `metadata_u32`/`metadata_utc`, not treated as a content checksum. The catalog's `crc32` column is computed over the decoded payload.",
        "- `.dat` database payloads are zlib-compressed binary records. Version/count headers and fixed record boundaries are inferred for players, managers, and teams; player ID/name offsets are candidates. All record bytes and aligned u16/u32/i32/f32 views are in `catalog/db_records.jsonl`; semantics for most fields remain unknown.",
        "- FTC3 texture headers are indexed in `catalog/ftc_textures.csv`; supported format codes have PNG previews under `previews/textures/`. See `TEXTURE_LAYOUT.md` for byte offsets and unsupported codes.",
        "- `.ftm`, `.sat`, `.adb`, `.xlc`, and `.bnk` interiors are not assigned unverified semantic field names. File hashes, paths, and available raw header/word views are cataloged. Use `analysis/inspect_binary.py` for any byte range; original bytes remain in `unpacked/`.",
        "",
        "## Catalog files",
        "",
        "- `catalog/apk_entries.csv`: every APK member.",
        "- `catalog/source_manifest.csv`: size and SHA-256 for the preserved source APK/OBB files.",
        "- `catalog/obb_entries.csv`: every OBB member.",
        "- `catalog/pak_entries.csv`: every decoded KPX member.",
        "- `catalog/all_files.csv`: combined list of every outer and KPX entry.",
        "- `catalog/file_family_summary.csv`: grouped file counts and decoded byte totals by container, major path family, and extension.",
        "- `catalog/db_payloads.csv`: zlib-decoded database payloads.",
        "- `catalog/ftc_textures.csv` and `previews/textures/`: FTC header/value inventory and PNG previews for mapped texture formats.",
        "- `catalog/ftm_models.csv`: FTM file sizes, hashes, first 256 bytes, and raw u32/f32 interpretations.",
        "- `catalog/animation_clips.csv`, `catalog/animdb_words.csv`, and `catalog/nis_sequences.csv`: clip headers, full animation-index word views, and XML sequence structure/references.",
        "- `catalog/db_record_index.csv` and `catalog/db_records.jsonl`: per-record database offsets, candidate player identity strings, raw bytes, and numeric views; see `DATABASE_LAYOUT.md`.",
        "- `analysis/inspect_binary.py`: read-only offset-based hex and numeric inspector for opaque binary formats.",
        "- `ANIMATION_LAYOUT.md`: animation/sequence inventory and decoding limits.",
        "- `TEXTURE_LAYOUT.md` and `MODEL_LAYOUT.md`: texture header fields, preview coverage, model inventory, and current format limits.",
        "- `analysis/dls20-reference.bms`: public QuickBMS format reference; this build's `00KPX` marker is recorded above.",
        "",
    ])
    (PROJECT / "FILE_MAP.md").write_text("\n".join(report), encoding="utf-8")
    print(f"APK={len(apk_rows)} OBB={len(obb_rows)} KPX={len(pak_rows)}")
    print(f"Catalogs: {CATALOG}")
    print(f"Unpacked: {UNPACKED}")
    print(f"Report: {PROJECT / 'FILE_MAP.md'}")


if __name__ == "__main__":
    main()
