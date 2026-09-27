# Database binary layout notes

The decoded `.dat` payloads are preserved byte-for-byte under `unpacked/db_decoded/`. `catalog/db_records.jsonl` exposes each inferred fixed-size record as raw hex plus little-endian 16-bit, unsigned/signed 32-bit, and float32 interpretations. Numeric vectors are aligned to record offset zero; array index `n` corresponds to byte offset `2*n` for u16 or `4*n` for 32-bit views. These are representations, not semantic field names.

Player names below are candidate UTF-16LE fields inferred from repeated readable records. The rest of each record is deliberately labeled only by byte offset until confirmed against game behavior or a trusted schema.

| Payload | Header interpretation | Header bytes | Records | Record bytes | Record array starts | Name/id candidates |
|---|---|---:|---:|---:|---:|---|
| `players.dat.decoded.bin` | header: u32 version, u32 revision, u32 record_count; words=[5060, 8, 5816] | 12 | 5816 | 180 | 12 | u16 id@0; UTF-16LE first-name candidate@2:34; last-name candidate@36:68 |
| `players_3050.dat.decoded.bin` | header: u32 version, u32 revision, u32 record_count; words=[3050, 6, 3514] | 12 | 3514 | 184 | 12 | u16 id@0; UTF-16LE first-name candidate@2:34; last-name candidate@36:68 |
| `managers.dat.decoded.bin` | header: u32 version, u32 record_count; words=[5060, 574] | 8 | 574 | 88 | 8 | none confirmed |
| `teams.dat.decoded.bin` | header: u32 version, u32 revision, u32 record_count; words=[5060, 9, 232] | 12 | 232 | 4092 | 12 | none confirmed |
| `teamplayerlinks_0.dat.decoded.bin` | opaque blob; leading u32 words=[5060, 233, 4294967295, 0, 0, 24, 70154, 65810, 3739659, 68104, 332558, 197894, 67348, 66072, 69895, 65569, 67869, 287, 13, 533, 4369, 2083, 1296, 514] | no split asserted | 1 blob view | 61264 | 0 | none confirmed |

`db_record_index.csv` is the quick lookup for record number, byte offset, inferred ID/name candidates, and the corresponding JSONL source. In the JSONL numeric arrays, values are offset-aligned interpretations and can be strings for non-finite float32 bit patterns. The original decoded binaries remain authoritative.

For a direct byte/word view of any file, use `python analysis/inspect_binary.py <path> --offset <bytes> --length <bytes>`; it prints the exact bytes, hex, printable characters, and u8/u16/u32/i32/f32 interpretations by offset.
