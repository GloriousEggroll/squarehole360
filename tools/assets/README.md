# User-supplied game assets

All game-dependent data comes from the user's ISO. The importer installs the
original disc layout where the current game loader already expects it, and
builds native renderer inputs locally. No original textures, meshes, audio,
movies, shader bytecode or shader translations belong in an application release.
Developer captures also contain game data and are local diagnostic artifacts.

## Usage

From the project root, with Python 3.11 or newer:

```sh
python3 tools/import-assets.py import "/path/to/gow3.iso"
python3 tools/import-assets.py verify
./tools/run-linux.sh
```

On Windows the importer uses the same standard-library implementation; use
`py -3` in place of `python3`. There are no Linux-only extraction APIs or external
extraction executables. Actual Windows validation is still outstanding.

Only the executable supported by this static recompile is accepted:

- Title ID: `4D5308AB`
- `default.xex` SHA-256:
  `4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790`

Other disc layouts with that executable may work, but other game revisions
need their own validated recompile and hooks. The importer does not bypass this
check. Supply an uncompressed Xbox 360 ISO containing an XDVDFS game partition.

The default output is `game/` beside the project. A new installation needs about
8 GB free in addition to the ISO. Progress reports inspection, ISO hashing,
extraction/adoption, preparation and verification. `--json-progress` emits one
JSON object per update for setup frontends.

An existing extraction requires `--adopt-existing`. Every file must match the
ISO, including its path and contents; additional or missing files are rejected.
The importer adds metadata and prepared data without replacing original files:

```sh
python3 tools/import-assets.py import "/path/to/gow3.iso" --adopt-existing
```

Repeated imports from the same ISO verify and reuse an already managed
installation. A different ISO or modified original file requires a separate
`--output` directory; there is no silent overwrite or destructive repair mode.
Saves, user settings and runtime/driver caches are outside the import tree.

The ISO can be moved away after import. `check` is a quick launch preflight
(inventory, file sizes and executable hash); `verify` hashes every original and
prepared file. Fresh imports are extracted to a temporary sibling directory and
renamed into place after verification. Normal errors and Ctrl+C clean up their
temporary data. After a force-killed process or power failure, confirm no importer
is running before removing its `.NAME.import.lock` and abandoned
`.NAME.import-*`/`.NAME.prepare-*` directories. If interrupted during preparation
publication, a `previous` directory under the preparation temporary directory
contains the old `.gow3-assets` directory and can be restored first.

## Native renderer preparation

`game/` retains original UE3 cooked packages (`.xxx`), texture caches (`.tfc`),
movies (`.bik`), executables and supporting files. The current engine already
loads those formats. Import does **not** decode all UE3 meshes/textures into
native resources, implement missing renderer passes, or generate every possible
graphics pipeline in advance.

`game/.gow3-assets/` contains:

- `manifest.json`: ISO provenance, every extracted path/size/SHA-256, preparation
  recipe version, and the checksums of prepared outputs.
- `catalog.json`: stable paths relative to the game root, asset categories,
  sizes and hashes for package/texture readers.
- `native/shaders/source/`: the global cache's 225 unique shader containers and
  a compilation manifest, all extracted locally.
- `native/shaders/compiled/`: optional local HLSL and SPIR-V outputs.

After building the Gears-adapted XenosRecomp and installing its native DXC
dependency as described in [the renderer instructions](../rendering/native/README.md):

```sh
python3 tools/import-assets.py prepare --compile-shaders
```

Alternatively add `--compile-shaders` to the initial import. Compilation failure
fails preparation rather than silently omitting shaders. This covers the global
cache only. The native renderer also captures shader containers as the game's
engine loads level/material caches and links variants, compiles supported
programs in process using XenosRecomp and DXC, and keeps a versioned runtime
shader cache. Encountering unsupported work invokes its compatibility fallback;
importing the global cache does not prove every shader or scene is supported.
The optional prepared outputs serve local validation tools; the live runtime
uses its own compiler-versioned cache. Neither cache replaces Vulkan driver or
graphics-pipeline caches, and import does not precompile the whole game.

`prepare` verifies original files and rebuilds derived data without requiring the
ISO again. It can repair damaged prepared outputs; it never modifies originals.
Without `--compile-shaders`, preparation produces source containers only.

New asset-dependent renderer work must extend `prepare_assets` in `importer.py`:
read the verified game tree, write derived files beneath the supplied staging
directory, fail explicitly for required unsupported data, and bump
`RECIPE_VERSION` when the prepared format/recipe changes. Consumers resolve
catalog/manifest paths relative to the selected game root. GPU-created render
targets remain runtime resources; they are not static disc assets.

## Validation and distribution

```sh
python3 -m unittest discover -s tests -p asset_import_test.py -v
```

These tests contain synthetic data only. They cover malformed discs, path
traversal/case collisions, version mismatch, transactional failures, adoption,
tampering, links, repeated imports and preparation. A full extraction of the
provided ISO was separately checked against all 2,592 existing game files.

Distribute the tool source, runtime and appropriate third-party dependencies.
Do not package `game/`, any custom import destination, ISO files, generated game
code, captures, user caches, or locally generated native shaders. Standard local
asset/build/capture paths are excluded by `.gitignore`; this is not a filter for
arbitrary file-copy or packaging commands. No release packaging pipeline exists
yet. Every release must assemble its files deliberately rather than archive this
development workspace.
