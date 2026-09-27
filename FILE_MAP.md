# DLS 18 v5.064 file map

Source files are preserved in `original/`. `unpacked/` contains the APK/OBB ZIP contents and every file decoded from the four KPX packages. CSV catalogs include sizes, storage method, offsets, SHA-256, CRC32, and the first 16 bytes in hex.

- APK outer entries: **1016**, uncompressed total **73,693,416 bytes**.
- OBB outer entries: **4**, uncompressed total **306,062,445 bytes**.
- KPX inner entries: **8316**, decoded total **402,945,597 bytes**.

## Four KPX packages

| Package | Marker | Files | Decoded bytes | Main contents |
|---|---:|---:|---:|---|
| `anims.pak` | `00KPX` | 2,536 | 17,740,966 | `.sat` animation clips and `animdb.adb` animation index |

**anims.pak extension counts:** .sat: 2535, .adb: 1

| `nis.pak` | `00KPX` | 199 | 2,361,539 | XML sequences/cut-scene definitions |

**nis.pak extension counts:** .xml: 199

| `env.pak` | `00KPX` | 159 | 47,304,944 | stadium, dressing-room, camera, and environment assets |

**env.pak extension counts:** .ftm: 136, .ftc: 20, .png: 3

| `models.pak` | `00KPX` | 5,422 | 335,538,148 | models, player/ball/stadium/crowd assets, and textures |

**models.pak extension counts:** .ftc: 3155, .ftm: 2244, .png: 20, .bmp: 2, .fta: 1

## APK folders to start with

- `unpacked/apk/assets/data/db/`: compressed player, team, manager, and roster-link databases. `unpacked/db_decoded/*.decoded.bin` are the zlib-decoded payloads; see `DATABASE_LAYOUT.md` and the per-record numeric views.
- `unpacked/kpx/anims/`: per-action `.sat` animation payloads and `animdb.adb` index.
- `unpacked/kpx/nis/`: compressed XML scene sequences and animation references.
- `unpacked/kpx/models/` and `unpacked/kpx/env/`: `.ftm` models and `.ftc` textures.
- `unpacked/apk/assets/data/shaders/`: shader variants (`.gl2`, `.gl3`, `.vk`).
- `unpacked/apk/assets/data/audio/`: menu music (`.m4a`) and sound-event bank (`.bnk`).
- `unpacked/obb/data/audio/`: commentary and crowd banks.
- `unpacked/apk/assets/data/text/`: language/name tables (`.xlc`), fonts, and blacklist.
- `unpacked/apk/assets/data/game/`: achievements, leaderboards, ad boards, server/certificate, and static network geometry.

The largest APK data families are frontend assets (`fe`, 192 files), shaders (129), text/fonts (27), effects (11), audio (9), game configuration (6), databases (5), and HUD (4). `catalog/file_family_summary.csv` gives exact counts and byte totals for each container/path-family/extension; `catalog/all_files.csv` has one row for every APK, OBB, and KPX member.

## Format notes

- The APK and OBB are ZIP containers. The OBB itself contains two `XBNK` audio banks plus `env.pak` and `models.pak`.
- All four `.pak` files use the `00KPX` header in this build. The KPX table supplies folder/file names, byte offsets, decoded and packed lengths, and a per-file metadata word. Compression flag `1` is zlib; flag `0` stores bytes directly.
- The KPX metadata word repeatedly maps to 2018 Unix timestamps in this sample; it is cataloged as `metadata_u32`/`metadata_utc`, not treated as a content checksum. The catalog's `crc32` column is computed over the decoded payload.
- `.dat` database payloads are zlib-compressed binary records. Version/count headers and fixed record boundaries are inferred for players, managers, and teams; player ID/name offsets are candidates. All record bytes and aligned u16/u32/i32/f32 views are in `catalog/db_records.jsonl`; semantics for most fields remain unknown.
- FTC3 texture headers are indexed in `catalog/ftc_textures.csv`; supported format codes have PNG previews under `previews/textures/`. See `TEXTURE_LAYOUT.md` for byte offsets and unsupported codes.
- `.ftm`, `.sat`, `.adb`, `.xlc`, and `.bnk` interiors are not assigned unverified semantic field names. File hashes, paths, and available raw header/word views are cataloged. Use `analysis/inspect_binary.py` for any byte range; original bytes remain in `unpacked/`.

## Catalog files

- `catalog/source_manifest.csv`: size and SHA-256 for the preserved source APK/OBB files.
- `catalog/apk_entries.csv`: every APK member.
- `catalog/obb_entries.csv`: every OBB member.
- `catalog/pak_entries.csv`: every decoded KPX member.
- `catalog/all_files.csv`: combined list of every outer and KPX entry.
- `catalog/file_family_summary.csv`: grouped file counts and decoded byte totals by container, major path family, and extension.
- `catalog/db_payloads.csv`: zlib-decoded database payloads.
- `catalog/ftc_textures.csv` and `previews/textures/`: FTC header/value inventory and PNG previews for mapped texture formats.
- `catalog/ftm_models.csv`: FTM file sizes, hashes, first 256 bytes, and raw u32/f32 interpretations.
- `TEXTURE_LAYOUT.md` and `MODEL_LAYOUT.md`: texture header fields, preview coverage, model inventory, and current format limits.
- `catalog/animation_clips.csv`, `catalog/animdb_words.csv`, and `catalog/nis_sequences.csv`: clip headers, full animation-index word views, and XML sequence structure/references.
- `catalog/db_record_index.csv` and `catalog/db_records.jsonl`: per-record database offsets, candidate player identity strings, raw bytes, and numeric views; see `DATABASE_LAYOUT.md`.
- `analysis/inspect_binary.py`: read-only offset-based hex and numeric inspector for opaque binary formats.
- `ANIMATION_LAYOUT.md`: animation/sequence inventory and decoding limits.
- `analysis/dls20-reference.bms`: public QuickBMS format reference; this build's `00KPX` marker is recorded above.

## Tactical research

Detailed reports on AI runs, space, formation layouts, player attributes, OVR, team rating, and practical tactics are indexed in [analysis/tactics_notes/README.md](analysis/tactics_notes/README.md). The reports separate extracted-code evidence from tactical interpretation and distinguish the stock game from the current sprint/close-control mod.
