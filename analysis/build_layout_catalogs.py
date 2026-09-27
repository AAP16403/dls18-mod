from __future__ import annotations

import csv
import hashlib
import json
import math
import pathlib
import struct
import xml.etree.ElementTree as ET
from collections import Counter


PROJECT = pathlib.Path(__file__).resolve().parents[1]
UNPACKED = PROJECT / "unpacked"
CATALOG = PROJECT / "catalog"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def utf16_field(data: bytes, start: int, length: int) -> str:
    raw = data[start:start + length]
    raw = raw[: len(raw) - len(raw) % 2]
    return raw.decode("utf-16le", errors="replace").split("\0", 1)[0]


def typed_views(data: bytes) -> dict[str, list]:
    u16_len = len(data) // 2
    u32_len = len(data) // 4
    u16 = list(struct.unpack_from("<" + "H" * u16_len, data, 0)) if u16_len else []
    u32 = list(struct.unpack_from("<" + "I" * u32_len, data, 0)) if u32_len else []
    i32 = list(struct.unpack_from("<" + "i" * u32_len, data, 0)) if u32_len else []
    f32 = list(struct.unpack_from("<" + "f" * u32_len, data, 0)) if u32_len else []
    # JSON has no finite representation for NaN and infinities; encode those as strings.
    f32_json = [value if math.isfinite(value) else repr(value) for value in f32]
    return {"u16le": u16, "u32le": u32, "i32le": i32, "f32le": f32_json}


def write_csv(path: pathlib.Path, rows: list[dict], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def database_views() -> list[str]:
    database_specs = {
        # name: (header_bytes, count_word_index, record_description)
        "players.dat.decoded.bin": (12, 2, "header: u32 version, u32 revision, u32 record_count"),
        "players_3050.dat.decoded.bin": (12, 2, "header: u32 version, u32 revision, u32 record_count"),
        "managers.dat.decoded.bin": (8, 1, "header: u32 version, u32 record_count"),
        "teams.dat.decoded.bin": (12, 2, "header: u32 version, u32 revision, u32 record_count"),
    }
    index_rows: list[dict] = []
    layout_lines = [
        "# Database binary layout notes",
        "",
        "The decoded `.dat` payloads are preserved byte-for-byte under `unpacked/db_decoded/`. `catalog/db_records.jsonl` exposes each inferred fixed-size record as raw hex plus little-endian 16-bit, unsigned/signed 32-bit, and float32 interpretations. Numeric vectors are aligned to record offset zero; array index `n` corresponds to byte offset `2*n` for u16 or `4*n` for 32-bit views. These are representations, not semantic field names.",
        "",
        "Player names below are candidate UTF-16LE fields inferred from repeated readable records. The rest of each record is deliberately labeled only by byte offset until confirmed against game behavior or a trusted schema.",
        "",
        "| Payload | Header interpretation | Header bytes | Records | Record bytes | Record array starts | Name/id candidates |",
        "|---|---|---:|---:|---:|---:|---|",
    ]
    record_output = CATALOG / "db_records.jsonl"
    record_output.parent.mkdir(parents=True, exist_ok=True)
    with record_output.open("w", encoding="utf-8", newline="\n") as out:
        for name, (header_bytes, count_word, header_note) in database_specs.items():
            path = UNPACKED / "db_decoded" / name
            data = path.read_bytes()
            header_words = list(struct.unpack_from("<" + "I" * (header_bytes // 4), data, 0))
            record_count = header_words[count_word]
            body = data[header_bytes:]
            if record_count == 0 or len(body) % record_count:
                raise ValueError(f"Cannot infer whole-record layout for {name}: body={len(body)} count={record_count}")
            record_size = len(body) // record_count
            if record_size % 4:
                raise ValueError(f"Record size for {name} is not 4-byte aligned: {record_size}")
            name_candidate = name.startswith("players")
            name_note = "u16 id@0; UTF-16LE first-name candidate@2:34; last-name candidate@36:68" if name_candidate else "none confirmed"
            for index in range(record_count):
                record_offset = header_bytes + index * record_size
                record = data[record_offset:record_offset + record_size]
                first_name = utf16_field(record, 2, 32) if name_candidate and record_size >= 68 else ""
                last_name = utf16_field(record, 36, 32) if name_candidate and record_size >= 68 else ""
                row = {
                    "source_file": name,
                    "record_index": index,
                    "absolute_offset": record_offset,
                    "record_bytes": record_size,
                    "record_id_u16_candidate": struct.unpack_from("<H", record, 0)[0] if name_candidate else "",
                    "first_name_utf16_candidate": first_name,
                    "last_name_utf16_candidate": last_name,
                    "raw_hex": record.hex(),
                    **typed_views(record),
                }
                out.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
                index_rows.append({
                    "source_file": name,
                    "record_index": index,
                    "absolute_offset": record_offset,
                    "record_bytes": record_size,
                    "record_id_u16_candidate": row["record_id_u16_candidate"],
                    "first_name_utf16_candidate": first_name,
                    "last_name_utf16_candidate": last_name,
                    "record_jsonl": "catalog/db_records.jsonl",
                })
            layout_lines.append(f"| `{name}` | {header_note}; words={header_words} | {header_bytes} | {record_count} | {record_size} | {header_bytes} | {name_note} |")

        # The roster-link payload has no confidently inferred record schema. Preserve
        # the complete byte sequence and provide aligned numeric views from offset 0.
        link_name = "teamplayerlinks_0.dat.decoded.bin"
        link_path = UNPACKED / "db_decoded" / link_name
        link_data = link_path.read_bytes()
        link_header_u32 = [struct.unpack_from("<I", link_data, offset)[0] for offset in range(0, min(len(link_data), 96), 4)]
        blob_row = {
            "source_file": link_name,
            "layout_confidence": "opaque blob; no fixed record schema claimed",
            "length_bytes": len(link_data),
            "raw_hex": link_data.hex(),
            **typed_views(link_data),
        }
        out.write(json.dumps(blob_row, ensure_ascii=False, separators=(",", ":")) + "\n")
    write_csv(CATALOG / "db_record_index.csv", index_rows, [
        "source_file", "record_index", "absolute_offset", "record_bytes",
        "record_id_u16_candidate", "first_name_utf16_candidate", "last_name_utf16_candidate", "record_jsonl",
    ])
    layout_lines.extend([
        f"| `{link_name}` | opaque blob; leading u32 words={link_header_u32} | no split asserted | 1 blob view | {len(link_data)} | 0 | none confirmed |",
        "",
        "`db_record_index.csv` is the quick lookup for record number, byte offset, inferred ID/name candidates, and the corresponding JSONL source. In the JSONL numeric arrays, values are offset-aligned interpretations and can be strings for non-finite float32 bit patterns. The original decoded binaries remain authoritative.",
        "",
        "For a direct byte/word view of any file, use `python analysis/inspect_binary.py <path> --offset <bytes> --length <bytes>`; it prints the exact bytes, hex, printable characters, and u8/u16/u32/i32/f32 interpretations by offset.",
    ])
    (PROJECT / "DATABASE_LAYOUT.md").write_text("\n".join(layout_lines) + "\n", encoding="utf-8")
    return ["Database layouts: DATABASE_LAYOUT.md", "Database record index: catalog/db_record_index.csv", "Database values: catalog/db_records.jsonl"]


def animation_views() -> list[str]:
    clips: list[dict] = []
    sat_files = sorted((UNPACKED / "kpx" / "anims").rglob("*.sat"))
    for path in sat_files:
        data = path.read_bytes()
        u16_count = min(len(data) // 2, 48)
        u16 = struct.unpack_from("<" + "H" * u16_count, data, 0) if u16_count else ()
        u32_count = min(len(data) // 4, 24)
        u32 = struct.unpack_from("<" + "I" * u32_count, data, 0) if u32_count else ()
        row = {
            "clip_path": path.relative_to(UNPACKED / "kpx" / "anims").as_posix(),
            "clip_id_from_filename_candidate": path.stem if path.stem.isdigit() else "",
            "bytes": len(data),
            "sha256": sha256(data),
            "header_hex_96": data[:96].hex(" "),
        }
        row.update({f"u16_at_{offset:03d}": u16[offset // 2] if offset // 2 < len(u16) else "" for offset in range(0, 96, 2)})
        row.update({f"u32_at_{offset:03d}": u32[offset // 4] if offset // 4 < len(u32) else "" for offset in range(0, 96, 4)})
        clips.append(row)
    columns = ["clip_path", "clip_id_from_filename_candidate", "bytes", "sha256", "header_hex_96"]
    columns += [f"u16_at_{offset:03d}" for offset in range(0, 96, 2)]
    columns += [f"u32_at_{offset:03d}" for offset in range(0, 96, 4)]
    write_csv(CATALOG / "animation_clips.csv", clips, columns)

    adb_path = UNPACKED / "kpx" / "anims" / "animdb.adb"
    adb = adb_path.read_bytes()
    adb_rows: list[dict] = []
    for offset in range(0, len(adb) - (len(adb) % 4), 4):
        word = struct.unpack_from("<I", adb, offset)[0]
        signed = struct.unpack_from("<i", adb, offset)[0]
        float_value = struct.unpack_from("<f", adb, offset)[0]
        adb_rows.append({
            "offset": offset,
            "bytes_hex": adb[offset:offset + 4].hex(" "),
            "u32le": word,
            "i32le": signed,
            "f32le": repr(float_value) if not math.isfinite(float_value) else float_value,
        })
    write_csv(CATALOG / "animdb_words.csv", adb_rows, ["offset", "bytes_hex", "u32le", "i32le", "f32le"])

    nis_rows: list[dict] = []
    for path in sorted((UNPACKED / "kpx" / "nis").rglob("*.xml")):
        raw = path.read_bytes()
        root = ET.fromstring(raw)
        counts = Counter(element.tag for element in root.iter())
        anim_refs: list[str] = []
        for element in root.iter():
            if "anim" in element.tag.casefold() or any("anim" in key.casefold() for key in element.attrib):
                value = (element.text or "").strip()
                if value:
                    anim_refs.append(value)
                for key, value in element.attrib.items():
                    if "anim" in key.casefold():
                        anim_refs.append(value)
        depth = 0
        stack = [(root, 1)]
        while stack:
            element, level = stack.pop()
            depth = max(depth, level)
            stack.extend((child, level + 1) for child in element)
        nis_rows.append({
            "xml_path": path.relative_to(UNPACKED / "kpx" / "nis").as_posix(),
            "bytes": len(raw),
            "sha256": sha256(raw),
            "root_tag": root.tag,
            "root_attributes_json": json.dumps(root.attrib, ensure_ascii=False, sort_keys=True),
            "node_count": sum(counts.values()),
            "max_depth": depth,
            "tag_counts_json": json.dumps(dict(sorted(counts.items())), ensure_ascii=False, sort_keys=True),
            "animation_references_json": json.dumps(anim_refs, ensure_ascii=False),
        })
    write_csv(CATALOG / "nis_sequences.csv", nis_rows, [
        "xml_path", "bytes", "sha256", "root_tag", "root_attributes_json", "node_count", "max_depth", "tag_counts_json", "animation_references_json",
    ])

    (PROJECT / "ANIMATION_LAYOUT.md").write_text("\n".join([
        "# Animation and sequence layout",
        "",
        f"The extracted animation package has {len(sat_files):,} `.sat` clips. `catalog/animation_clips.csv` lists every clip's exact relative path, byte length, SHA-256, first 96 bytes as hex, and both u16/u32 interpretations for offsets 0–95. The ID parsed from a numeric filename is labeled as a candidate; the `.sat` header layout and keyframe field meanings are not claimed as decoded.",
        "",
        f"`animdb.adb` is {len(adb):,} bytes. Its complete 4-byte aligned word stream is in `catalog/animdb_words.csv`, with raw bytes and u32/i32/f32 interpretations per offset. These are raw interpretations, not semantic field assignments.",
        "",
        f"All {len(nis_rows):,} NIS XML sequence files are retained under `unpacked/kpx/nis/`. `catalog/nis_sequences.csv` summarizes each XML tree, byte hash, element counts, depth, and extracted animation-reference text such as `AnimID`; the XML remains the editable source for scene timing/actions/camera/player nodes.",
        "",
        "Use the file-level inspector documented in `DATABASE_LAYOUT.md` to examine any `.sat` or `.adb` range by byte offset. No animation payload was rewritten.",
        "",
    ]) + "\n", encoding="utf-8")
    return ["Animation layout: ANIMATION_LAYOUT.md", "Clip inventory: catalog/animation_clips.csv", "Animation index values: catalog/animdb_words.csv", "NIS sequence index: catalog/nis_sequences.csv"]


def model_views() -> list[str]:
    rows: list[dict] = []
    for root_name in ("models", "env"):
        root = UNPACKED / "kpx" / root_name
        for path in sorted(root.rglob("*.ftm")):
            data = path.read_bytes()
            limit = min(len(data), 256)
            u32_count = limit // 4
            u32 = struct.unpack_from("<" + "I" * u32_count, data, 0) if u32_count else ()
            f32 = struct.unpack_from("<" + "f" * u32_count, data, 0) if u32_count else ()
            rows.append({
                "source_path": path.relative_to(UNPACKED).as_posix(),
                "bytes": len(data),
                "sha256": sha256(data),
                "first_16_hex": data[:16].hex(" "),
                "first_256_hex": data[:256].hex(" "),
                "u32le_first_256": json.dumps(u32),
                "f32le_first_256": json.dumps([v if math.isfinite(v) else repr(v) for v in f32]),
            })
    write_csv(CATALOG / "ftm_models.csv", rows, ["source_path", "bytes", "sha256", "first_16_hex", "first_256_hex", "u32le_first_256", "f32le_first_256"])
    return ["Model inventory: catalog/ftm_models.csv"]


def main() -> None:
    CATALOG.mkdir(parents=True, exist_ok=True)
    written = database_views() + animation_views() + model_views()
    print("\n".join(written))
    print(f"Indexed {len(list((UNPACKED / 'kpx' / 'anims').rglob('*.sat')))} SAT clips and {len(list((UNPACKED / 'kpx' / 'nis').rglob('*.xml')))} NIS XML files")
    print(f"Indexed {len(list((UNPACKED / 'kpx').rglob('*.ftm')))} FTM model files")


if __name__ == "__main__":
    main()
