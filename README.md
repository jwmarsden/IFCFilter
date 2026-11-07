# BIMPass

Prepares an IFC model for federation. It takes one model and splits it into these, under the `--out` folder:

| Output | Holds |
|---|---|
| `federate/STEM.federate.ifczip` | What should be federated, with tessellation explosions fixed and elements merged by site ID where appropriate |
| `non-federate/STEM.hidden.ifczip` | Geometry filtered out because it isn't visible or important to the federation, such as reinforcement inside a slab |
| `non-federate/STEM.envelopes.ifczip` | Space-proofing and clash shapes that aren't physical, typically transparent envelopes around things |
| `manifest/STEM.manifest.csv` | Where each original element went, its new GUID, what was done and why |
| `register.csv` | What the outputs were built from: each model's file name, path, size, dates and SHA-256 hash |

`STEM.original.ifc` is a link to the input, to compare in a viewer. Windows only allows links in developer mode, so without it this file is left out, with a note.

An ifczip is a zip holding one IFC file. `--no-zip` writes plain `.ifc` files instead, for a tool that won't take a zip: Bentley's list of supported formats only names `.ifc`, so the zip hasn't been shown to federate.

The federation target is Bentley Infrastructure Cloud, under IFC4.

Every part of the input must land in exactly one output. An element can be split across outputs: one Hume slab element holds both its concrete and its steel, and electrical families draw their clearance inside the device's element.

`bimpass split` takes any number of models and folders of them, with `--out` saying where everything goes:

```sh
bimpass split MODELS_FOLDER --out OUTPUT_FOLDER --rules rules.txt
```

Models go through at the same time, in a process each, up to one per processor (`--jobs N` sets another limit, and 1 goes one at a time). They start biggest first, and only when there's room in memory for them, at 10 times their file size. Output doesn't depend on how many run at once.

`bimpass check` compares models and folders with the register, to see if anything upstream has moved on, without changing anything. It lists what's new, changed or missing, and exits with 1 if there's any. The hash decides, so a file that was only copied again doesn't count as changed. A model only goes in the register once it has been processed, so the register always says what the outputs were built from.

```sh
bimpass check MODELS_FOLDER --out OUTPUT_FOLDER
```

No GUID is written twice in a run, across all its outputs:

- Whatever is in only one output keeps its GUID. Anything in several, such as a split element or the project and spatial structure each output needs, keeps it in the federation file and has one made from it in the others.
- A GUID in more than one model changes in all of them. That goes for anything with a GUID, including a project, site or building that several models share, so they no longer match across federated files.
- A GUID repeated inside a model stays on the first thing that has it.

GUIDs are made from the old GUID and the model's file name, so they're the same on every run. A run can only see the models it's given, so models to be federated together should go through together. The manifest lists each element's GUID in each output.

Elements with the same `SiteID` are one asset, so the federation file has them as one element (`--no-merge` skips this):

- It's an `IfcBuildingElementProxy` named after the site, with a GUID made from the site ID and the model's file name. Each element's body becomes a mapped item of it, placed where the element was, so geometry and colours are as they were.
- It has the properties that have the same value on every element it's made from. Other properties, types and materials aren't kept on it. The manifest says which element went into which asset.
- A blank, `N/A`, placeholder or `No Linear Assets Req` site ID isn't an asset, nor is one ending in `xx`, which stands for a range of sites. A site with one element stays as it is.
- A shape with no style of its own takes its colour from its element's material, which isn't kept, so it comes out in a viewer's default colour.
- An element that openings cut stays separate, as a void on a merged element would cut everything else in that space.
- A site is only merged within a model, so one in two models stays as one element in each.

Face sets that are the same shape, moved and turned, are written once and placed with mapped items (`--no-instance` skips this). A copy has the same faces and colours as what it's placed from, and every point within `--precision-mm`, with no mirroring. This goes before compact writing, so copies can't drift apart by being rounded differently. It saves most where a model repeats parts, such as a gantry's bolts and signs, and next to nothing on road meshes, which are all different.

Where a family doesn't say what a thing is, as with a road's meshes, `hidden-if` and `envelope-if` rules go by a property's value instead: `SET.PROPERTY=VALUE`.

Each output is then written compactly, keeping every element and property (`--no-compact` skips this):

- Coordinates in point lists are rounded to 0.1 mm (`--precision-mm`), from the project's length unit. Placements aren't touched.
- Polygon face sets become triangle lists, holes included. A set stays as polygons if it has a polygonal texture map or a face that can't be cut into triangles of the same area.
- Faces and triangles with no area once rounded are left out, along with their colour map entries. A set made of nothing else keeps them, with a warning, as a face set can't be empty.
- Identical property values and sets are written once, with one relationship per set listing everything the set is on. A shared set keeps the first one's GUID.

Face counts stay as they were, as the shapes aren't simplified.

## Status

Planned:

1. `bimpass report`: where a model's bytes go, explosions, envelopes and site ID use. Suspiciously large elements are listed by GUID, to look at before anything culls them. Done.
2. Split into the three files and the manifest. Things enclosed in other things, like slab reinforcement, and things out of sight, like buried piles, go to the hidden file: by rule so far, with no geometric test. Done.
3. Compact writing. Done.
4. Merge by site ID. Done.
5. Instancing repeated shapes. Done.
6. Tessellation fixes: merging flat faces, then simplifying within a tolerance.

## Build

```sh
nix develop
cmake -B build && cmake --build build
./build/bimpass report MODEL.ifc --csv families.csv --suspects suspects.csv
```

The flake builds IfcOpenShell's parser from the `ifcopenshell-v0.9.0` tag, with the IFC2X3 and IFC4 schemas and no geometry engine.

Without an installed IfcOpenShell, CMake builds the same thing from the `third_party/IfcOpenShell` submodule, pinned to that tag. Clone with `--recurse-submodules`, or run `git submodule update --init` afterwards.

### Windows

This builds with Visual Studio 2022. Dependencies come from [vcpkg](https://vcpkg.io), and IfcOpenShell from the submodule. In PowerShell, with `VCPKG_ROOT` set:

```powershell
git submodule update --init
vcpkg install boost-algorithm boost-config boost-dll boost-dynamic-bitset boost-filesystem boost-lexical-cast boost-locale boost-logic boost-math boost-multi-index boost-optional boost-property-tree boost-uuid boost-variant zlib --triplet x64-windows

cmake -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
.\build\Release\bimpass.exe split MODELS_FOLDER --out OUTPUT_FOLDER --rules rules.txt
```

The parser's schemas are plugin DLLs, loaded from beside `bimpass.exe`, so keep the DLLs in `build\Release` with it.

Paths are safest in plain ASCII, as command line arguments come in using the system's code page.
# BIMPass
