# BIMPass

Prepares an IFC model for federation. It takes one model and splits it into these, under the `--out` folder:

| Output | Holds |
|---|---|
| `federate/STEM.federate.ifczip` | What should be federated, with tessellation explosions fixed and elements merged by site ID where appropriate |
| `non-federate/STEM.hidden.ifczip` | Geometry filtered out because it isn't visible or important to the federation, such as reinforcement inside a slab |
| `non-federate/STEM.envelopes.ifczip` | Space-proofing and clash shapes that aren't physical, typically transparent envelopes around things |
| `STEM.manifest.csv` | Where each original element went, its new GUID, what was done and why |

`STEM.original.ifc` is a link to the input, to compare in a viewer. Windows only allows links in developer mode, so without it this file is left out, with a note.

An ifczip is a zip holding one IFC file. `--no-zip` writes plain `.ifc` files instead, for a tool that won't take a zip: Bentley's list of supported formats only names `.ifc`, so the zip hasn't been shown to federate.

The federation target is Bentley Infrastructure Cloud, under IFC4.

Every part of the input must land in exactly one output. An element can be split across outputs: one Hume slab element holds both its concrete and its steel, and electrical families draw their clearance inside the device's element.

Each output is then written compactly, keeping every element and property (`--no-compact` skips this):

- Coordinates in point lists are rounded to 0.1 mm (`--precision-mm`), from the project's length unit. Placements aren't touched.
- Polygon face sets become triangle lists, holes included. A set stays as polygons if it has a polygonal texture map or a face that can't be cut into triangles of the same area.
- Faces and triangles with no area once rounded are left out, along with their colour map entries. A set made of nothing else keeps them, with a warning, as a face set can't be empty.
- Identical property values and sets are written once, with one relationship per set listing everything the set is on. A shared set keeps the first one's GUID.

Face counts stay as they were, as the shapes aren't simplified.

## Status

Planned:

1. `bimpass report`: where a model's bytes go, explosions, envelopes and site ID use. Suspiciously large elements are listed by GUID, to look at before anything culls them. Done.
2. Split into the three files and the manifest. Things enclosed in other things, like slab reinforcement, go to the hidden file: by rule so far, with no geometric test. Done.
3. Compact writing. Done.
4. Merge by site ID, only where elements are close together and their metadata matches.
5. Tessellation fixes: merging flat faces, then simplifying within a tolerance.

## Build

```sh
nix develop
cmake -B build && cmake --build build
./build/bimpass report MODEL.ifc --csv families.csv --suspects suspects.csv
```

The flake builds IfcOpenShell's parser from the `ifcopenshell-v0.9.0` tag, with the IFC2X3 and IFC4 schemas and no geometry engine.

### Windows

The source is written to build with Visual Studio 2022, but that hasn't been tried yet. Dependencies come from [vcpkg](https://vcpkg.io), and IfcOpenShell is built from source with the same options as the flake. In PowerShell, with `VCPKG_ROOT` set:

```powershell
vcpkg install boost eigen3 zlib --triplet x64-windows
$toolchain = "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

git clone --depth 1 --branch ifcopenshell-v0.9.0 https://github.com/IfcOpenShell/IfcOpenShell
cmake -S IfcOpenShell/cmake -B ifcopenshell-build $toolchain -DCMAKE_INSTALL_PREFIX=C:/ifcopenshell `
  "-DSCHEMA_VERSIONS=2x3;4" -DBUILD_IFCGEOM=OFF -DBUILD_IFCPYTHON=OFF -DBUILD_CONVERT=OFF -DBUILD_GEOMSERVER=OFF `
  -DBUILD_EXAMPLES=OFF -DWITH_OPENCASCADE=OFF -DWITH_CGAL=OFF -DCOLLADA_SUPPORT=OFF -DHDF5_SUPPORT=OFF
cmake --build ifcopenshell-build --config Release --target install

cmake -B build $toolchain -DCMAKE_PREFIX_PATH=C:/ifcopenshell
cmake --build build --config Release
.\build\Release\bimpass.exe split MODEL.ifc --out out --rules rules.txt
```

Paths are safest in plain ASCII, as command line arguments come in using the system's code page.
# BIMPass
