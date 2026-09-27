# Model file inventory and static mesh decode

The `models.pak` and `env.pak` packages contain 2,380 decoded `.ftm` files totaling 200,736,100 bytes. Their originals remain under `unpacked/kpx/models/` and `unpacked/kpx/env/`. At offset 8, 2,319 files contain `FTTM` and 61 contain `XGSM`.

## Static mesh layout recovered

`analysis/decode_assets.py models` recognizes and exports 155 files with the same validated static-mesh layout. For these files, the decoded fields are:

- Two little-endian 16-bit counts at `0x7c`: vertex count and triangle count.
- Float32 XYZ positions beginning at `0xc4`.
- A triangle index stream after the position array's 8-byte strip header. Each index is a little-endian 16-bit vertex index.
- Float32 XYZ normals and UV pairs at offsets addressed relative to `0x70` by the words at `0x90` and `0x9c`.

The decoder checks that positions and attributes are finite and every face index is within the vertex array before exporting. It writes OBJ files and small JSON metadata sidecars under `decoded_assets/models/`, with a source-to-output inventory in `decoded_assets/models/index.csv`. Three representative previews are under `previews/models/`.

Examples include the ball (`752` vertices, `1,080` triangles), the English Cup trophy (`2,681` vertices, `2,620` triangles), and several goal and stadium pieces. The exported OBJ files contain positions, normals, UVs, and triangle faces, so the mesh data is directly inspectable in common 3D tools.

## Player model rig maps, hierarchy, and remaining geometry

The other 2,225 files do not match this static-mesh layout. There are 2,205 player model files; 2,203 declare a 42-byte map at `0x54` (the count is at `0x4c`). `decoded_assets/models/player_rig_maps.csv` preserves those signed map bytes and nearby header values without assigning unconfirmed meanings to them.

The 42-bone parent hierarchy, names, and static local transforms are decoded for `body_0_1.ftm` in `analysis/preview_sat_skeleton.py`. Chunk `0x25` supplies the first-child/next-sibling hierarchy, chunk `0x1e` supplies bone names, and chunk `0x1c` supplies local scale/quaternion/translation records. `analysis/preview_skinned_player.py` also decodes the four body geometry sections and their 56-byte, four-influence skin records, and confirms the native 56-byte vertex stride and influence-weight layout. A fully skinned preview currently has an unresolved coordinate mismatch between the bind/deformation matrices and mesh positions, so it is not yet a reliable visual check of an animated player. The remaining non-player files also include helpers and other model types. No `.ftm` source file has been modified.
