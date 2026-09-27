# Texture layout

There are 3,276 FTC files across the extracted APK data and the `models.pak` / `env.pak` packages. They all use the `FTC3` marker and their zlib payloads decompressed to the byte count stored in the header.

## FTC3 header

| Offset | Size | Interpretation |
|---:|---:|---|
| `0x00` | 4 | ASCII magic `FTC3` |
| `0x04` | 4 | decoded payload byte count, little-endian u32 |
| `0x08` | 4 | raw flags; meaning not assigned |
| `0x0C` | 2 | width, little-endian u16 |
| `0x0E` | 2 | height, little-endian u16 |
| `0x10` | 2 | pixel/compression format code, little-endian u16 |
| `0x12` | 2 | mip count, little-endian u16 |
| `0x14` | rest | zlib-compressed pixel/mipmap payload |

## Format codes in this project

These are the interpretations implemented by the preview decoder. They describe the pixel layout used to render previews; they are not game-editing instructions.

| Code | Preview interpretation | Files |
|---:|---|---:|
| 14 | ETC1 RGB | 1,993 |
| 2 | ABGR4444 | 1,191 |
| 8 | A8 grayscale | 37 |
| 1 | BGR565 | 23 |
| 3 | RGBA32 | 6 |
| 0 | unmapped | 15 |
| 256 | unmapped | 9 |
| 270 | unmapped | 2 |

The first five groups have PNG previews under `previews/textures/`; the corresponding raw FTC is retained in `unpacked/`. The previews render the base image level, not a browsable mip chain. The 26 textures with codes 0, 256, and 270 remain available as raw files and are listed in `catalog/ftc_textures.csv` with dimensions, flags, mip count, decompressed byte count, and hash.

## Catalog columns

`catalog/ftc_textures.csv` has the source path, preview path if mapped, original size, decoded payload size, header values, format name, status, and SHA-256. All previews are file-name/path matched to the source with the extension changed to `.png` under `previews/textures/`.
