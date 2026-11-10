# IFCFilter

Prepares an IFC model for federation. It takes one model and splits it into these, under the `--out` folder:

| Output | Holds |
|---|---|
| `federate/DISCIPLINE/STEM.federate.ifc` | What should be federated, with tessellation explosions fixed and elements merged by site ID where appropriate |
| `non-federate/STEM.hidden.ifczip` | Geometry filtered out because it isn't visible or important to the federation, such as reinforcement inside a slab |
| `non-federate/STEM.envelopes.ifczip` | Space-proofing and clash shapes that aren't physical, typically transparent envelopes around things |
| `manifest/STEM.manifest.csv` | Where each original element went, its new GUID, what was done and why |
| `register.csv` | What the outputs were built from: each model's file name, path, size, dates and SHA-256 hash |

`STEM.original.ifc` is a link to the input, to compare in a viewer. Windows only allows links in developer mode, so without it this file is left out, with a note.

The federation file is plain `.ifc`, as that's the format Bentley's list of supported formats names. Federation files are in a folder for each discipline, named after the folder the input is in, as there are hundreds of them; a run moves any it finds where an older run put them, straight under `federate`. The other two are ifczips, each a zip holding one IFC file, and `--no-zip` writes those as plain `.ifc` too.

The federation target is Bentley Infrastructure Cloud, under IFC4, so an IFC2X3 model is written as IFC4. Geometry and properties are copied as they are, and only what IFC4 changed is rewritten:

- Attributes IFC4 added to the end of a type are written as `$`.
- `IfcRelConnectsPortToElement` becomes `IfcRelNests`, with the same GUID, as IFC4 only lets the old one relate to distribution elements, and ports are often on proxies.
- A predefined type IFC4 doesn't have, such as `.TELEPHONE.`, becomes `.USERDEFINED.`, with the old value as the element type if there isn't one.

Every instance is checked against the IFC4 schema first: its type, what its attributes are called, and that each value fits, down to the type of what it refers to. If anything doesn't fit, such as a type that's gone or an attribute that now means something else, the whole model is written as IFC2X3, with a warning saying why. The header's view definition isn't changed, nor are EXPRESS rules checked. Models in IFC4 or a later schema, such as IFC4X3_ADD2, which 12d writes for survey, go through in their own schema.

Every part of the input must land in exactly one output. An element can be split across outputs: one Hume slab element holds both its concrete and its steel, and electrical families draw their clearance inside the device's element.

`ifcfilter split` takes any number of models and folders of them, with `--out` saying where everything goes:

```sh
ifcfilter split MODELS_FOLDER --out OUTPUT_FOLDER --rules rules.txt
```

The rules file says where things go and what the models call their asset data: which property sets and properties hold the site ID, design tag, asset type and so on. None of that is built in. `rules.txt.example` lists every rule; copy it to `rules.txt` and change it to suit. `rules.txt` and `mapping.csv` are project data, so git ignores them.

Models go through at the same time, in a process each, up to one per processor (`--jobs N` sets another limit, and 1 goes one at a time). They start biggest first, and only when there's room in memory for them, at 10 times their file size. Output doesn't depend on how many run at once.

A run only does a model if something it depends on has changed since its outputs were written: the model, by its hash; the build, which is `ifcfilter.exe`, `rules.txt`, `mapping.csv` and the options that change what's written; or the GUIDs it shares with the other models in the run, as those change in all of them. A model whose outputs aren't all there is done too, and `--all` does every model. The register keeps a hash of the build and of the shared GUIDs for each model. So a new model is done with the models it shares GUIDs with, and a changed rule does everything.

While a run goes, `OUT/progress.html` says how it's going: models done and running, input done, how long is left, and what each model came to. It reloads itself every few seconds until the run is over, so open it in a browser and leave it. Everything the run prints also goes to a log of its own in `OUT/logs`, with when each model started and finished.

`ifcfilter check` compares models and folders with the register, to see if anything upstream has moved on, without changing anything. It lists what's new, changed or missing, and exits with 1 if there's any. The hash decides, so a file that was only copied again doesn't count as changed. A model only goes in the register once it has been processed, so the register always says what the outputs were built from.

```sh
ifcfilter check MODELS_FOLDER --out OUTPUT_FOLDER
```

`ifcfilter cache` writes a viewing cache of a model, an `.ifcc` file a viewer can load in pieces: its meshes once each, placed where they're used, in spatial tiles, with the tree and properties in indexed tables. It's a prototype, for a viewer that doesn't exist yet; `docs/cache-format.md` is the format.

```sh
ifcfilter cache MODEL.ifc --out MODEL.ifcc
```

`scripts\mirror.ps1` keeps a copy of the output folder up to date elsewhere, such as in a synced folder. It won't run on a folder with a run going on, and `-List` says what would change.

No GUID is written twice in a run, across all its outputs:

- Whatever is in only one output keeps its GUID. Anything in several, such as a split element or the project and spatial structure each output needs, keeps it in the federation file and has one made from it in the others.
- A GUID in more than one model changes in all of them. That goes for anything with a GUID, including a project, site or building that several models share, so they no longer match across federated files.
- A GUID repeated inside a model stays on the first thing that has it.

GUIDs are made from the old GUID and the model's file name, so they're the same on every run. A run can only see the models it's given, so models to be federated together should go through together. The manifest lists each element's GUID in each output.

Elements with the same site ID (`site-id`, in the `site-set` sets) are one asset, so the federation file has them as one element (`--no-merge` skips this):

- It's an `IfcBuildingElementProxy` named after the site, with a GUID made from the site ID and the model's file name. Its body is the elements' shapes, written again where they were: meshes and breps with their points moved, solids with their position, and booleans and maps followed down to what they hold, with each shape's colours. Open IFC Viewer won't select an element by clicking it when its body is many mapped items, until it's been opened from the tree, so clicking any part of a gantry or a rail selects the whole of it this way. If a part has a shape that can't be moved like this, such as a swept disk, the asset's body is a mapped item of each element's body instead, placed where the element was, and the log says how many and why. Geometry and colours are as they were either way, though a shape repeated within an asset is written for each time it's used.
- It has the properties that have the same value on every element it's made from. Other properties, types and materials aren't kept on it, apart from the `quantity-set`: its `quantity-height`, `quantity-width` and `quantity-length` are those of the whole asset, in metres (length the longer way across in plan, width the shorter, height up), and other numbers that differ part to part are added up, which won't mean much for some of them. The manifest says which element went into which asset.
- A blank, `N/A`, placeholder, `TBA`, `TBC` or `No Linear Assets Req` site ID isn't an asset, nor is one ending in `xx`, which stands for a range of sites, or in six zeros, such as `ECL000000`, a class of asset with no site given yet, nor is one with no digits in it, such as `HSP` or `WPNXXXXXX`, which is a class of asset too. A site with one element stays as it is.
- A shape with no style of its own takes its colour from its element's material, which isn't kept, so it comes out in a viewer's default colour.
- Openings that cut a part now cut the asset. They reach other parts only where those meet what they cut: on the gantries of 100333, all they'd take was 0.3 litres from 40 bolts and nuts, presumably inside their holes, which can't be seen.
- A site is only merged within a model, so one in two models stays as one element in each.
- In the federation file the building has a level for each asset type, a building storey named after the asset type its site IDs start with: their letters, as in `PU290101`, or `RC1`, `RC2` and `RC3`, whose number is part of the type, in alphabetical order, and last a level called `Non-Managed Types` for everything with no site ID. A site that's one asset, or one element, is on its type's level as itself, renamed to its site ID (Open IFC Viewer won't select an element alone on a level by clicking it, so sites aren't levels of their own). A site still in several pieces is a partial level on its type's level, named after the site ID, holding them. The model's own levels are left out once they're empty, which they all are unless something such as a space is made of them. Each new level has a placement of its own where the building is, and what's on it is placed from it, where it was, by changing its own placement so that its ports and openings follow: a viewer finds what a clicked thing is on through placements, and Open IFC Viewer can't when levels share the building's.
- An element with no site ID goes by its design tag (`design-tag`, in the `design-set` sets), if that's an ID: not a placeholder, with a digit in it, as `LO` or `BOLLARD` hasn't, with no spaces, as a description has, and not on more than half the model's tagged elements, as a code for the whole model is. If its tag is only on one site's parts, it joins that site; otherwise elements with the same tag are an asset, named after it, and the manifest says `design tag X`. A tag can still name a type rather than one thing, as TU's deck units' do: each has a mark of its own (`mark`, in the `mark-set` sets), so elements with a tag only go together if their marks are the same, or they have none. Where pieces of a type have no marks, as in BS, the 5 m gap keeps panels of one type apart only where they don't touch.
- Parts of a site are only merged if each is within 5 m of another (`--merge-gap M`, and 0 doesn't check), as two things far apart with one site ID, such as two cameras, are a mistake in the data. The gap is between boxes around the parts, so it's never more than the real gap, and parts that can't merge still count as what joins the rest. Groups that are far apart are merged as separate assets, a part on its own stays as it is, and the manifest says which. A part whose reach can't be worked out stays as it is too.

Each output is then tidied:

- `cull-set` sets are left out, once merging has used their design tags, along with what only they had.
- What the `duplicate-set` repeats from another set on the same elements, with the same value, is left out of it, and it goes if that's all it had. What only it says stays.
- Property sets are in alphabetical order, ignoring case: their relationships are written last, numbered in that order, as viewers list sets in the order of those.
- The header names the output and the time it was written, gives IFCFilter as the preprocessor with the one before it in brackets, and its description notes the source. The originating system and author stay as they were. Owner histories are marked `.MODIFIED.` then, by a IFCFilter application.
- The project is named after the model, with its `project-long-name` as its long name, the site as `site-name`, and the building after its `building-name`, both in the `project-set`, the values most of the model has. Levels lose their Revit level type.
- Elements with a site ID, and what they're made of, are named after it and described as their `asset-component`. An `asset-type` that isn't how the site ID starts is corrected to it, with a property of its own, as the old one may be on other sets; a set on elements of different types is left as it is. Anything else is named as its family and type, without the Revit element ID Revit adds to the end, as that's its tag already.

Faceted breps, which Revit writes for most families, and surface models made of shells of faces, which Civil 3D writes, are first written as polygon face sets of the same faces, so that instancing and compact writing work on them too (part of compact writing, so `--no-compact` leaves them). A brep with voids, or not made of plain faces and loops, stays as it is, as does one of an element an opening cuts, and any model that isn't IFC4.

Face sets that are the same shape, moved and turned, are written once and placed with mapped items (`--no-instance` skips this). A copy has the same faces and colours as what it's placed from, and every point within `--precision-mm`, with no mirroring. This goes before compact writing, so copies can't drift apart by being rounded differently. It saves most where a model repeats parts, such as a gantry's bolts and signs, and next to nothing on road meshes, which are all different.

A body of several face sets isn't instanced set by set, as Open IFC Viewer won't select an element by clicking it when its body is many mapped items. A whole body that's the same as another, moved and turned, is placed from it instead, as one mapped item, and so is the map of several sets that Revit makes an element's body of.

Where a family doesn't say what a thing is, as with a road's meshes, `hidden-if` and `envelope-if` rules go by a property's value instead: `SET.PROPERTY=VALUE`.

Each output is then written compactly, keeping every element and property (`--no-compact` skips this):

- Coordinates in point lists are rounded to 0.1 mm (`--precision-mm`), from the project's length unit. Placements aren't touched.
- Polygon face sets become triangle lists, holes included. A set stays as polygons if it has a polygonal texture map or a face that can't be cut into triangles of the same area. Areas are measured from each face's first point, as products of world coordinates, such as N6124295, lose more than a small face's area.
- Detail smaller than the rounding goes: on the light fittings of S1S-LT-103601, 0.1 mm rounding loses 0.17% of the surface, in faces a few tenths of a millimetre across. `--precision-mm 0.01` keeps it, for a file about 6% bigger.
- Faces and triangles with no area once rounded are left out, along with their colour map entries. A set made of nothing else keeps them, with a warning, as a face set can't be empty.
- A face set of an element an opening cuts, or that an opening is made of, isn't changed in any way, nor instanced: all of the element's, not only those the opening reaches. Taking a solid from a mesh is fragile: rounding or triangles were seen to change what IfcOpenShell cuts, and in some elements to leave next to nothing, and on a merged gantry, rounding the parts an opening doesn't reach changed what it took from those it does by up to 55 litres. These elements come out as the input's do, whether that's right or not, which makes cut assets, such as gantries, bigger.
- Identical property values and sets are written once, with one relationship per set listing everything the set is on. A shared set keeps the first one's GUID.

Then, in the federation file only, triangle meshes with more points than their shape needs, such as pipes and fittings drawn with 24-sided circles, are simplified within 2 mm (`--simplify-mm`, and 0 doesn't). Points are only taken away, never moved, so what's left is rounded as before:

- A point that's kept is within the tolerance of the planes of all the faces it stands for, each plane counting the same (OpenMesh's own weights them by area, which in metres let points move hundreds of millimetres), and a point that goes is within it of the triangles that take its place.
- Open edges stay where they are, as do the corners of any triangle the mesh can't take, such as where three or more share an edge; those triangles are written as they were.
- Points written the same are one point first, so that pieces that meet are joined: Revit's jet fans in TV repeat each point three times over.
- A mesh an opening cuts isn't simplified, nor one with a different colour or texture for different triangles, or normals of its own. A colour map with more entries than triangles, all the same colour, as Revit writes for the jet fans, is written again with one each.

Measured as how far each point of the original is from the simplified surface, over the 40 products each that lost the most: N5S-DU-100304 at most 1.04 mm, S1S-HY-201002 1.38 mm and S1S-RW-100020 1.92 mm. They went from 9.1 to 3.0 MB, 25.1 to 11.4 MB and 17.8 to 9.6 MB, and PWS-TV-201001, which is mostly its jet fans, from 389.7 to 81.2 MB. It uses OpenMesh's multiple choice decimater, which picks each collapse from random samples, with the same seed for every mesh, so a run comes out the same each time: its other decimater, which keeps every collapse in order, took up to a minute a set on Revit's fans of thousands of triangles around a point, and a bound on the distance from all that's been taken away (ModHausdorffT) took 491 s on one of DU's sets.

## Combining models

`ifcfilter combine` writes federation files as one, such as all of a zone's decks:

```sh
ifcfilter combine FEDERATE_FOLDER --out OUTPUT_FOLDER --register combine.csv
```

`combine.csv` says, model by model, what each goes into; `combine.csv.example` shows the form, with two models each going into a file of decks and one of tunnel for one zone, one left out with a blank name, and another zone's decks. Like `rules.txt` and `mapping.csv`, it's the project's, so it isn't in the repository. The first time, it's written listing every federation file in the folders given, with its discipline, zone, size, how many elements it has and the three names most of them have, such as `DEK TYPE 1A MC02 - 2400 (412)`, to tell decks from rings, and an empty `combined` column, and nothing else is done. Fill that in, in Excel, with the name of the file each model goes into, and run it again: each name is written as `NAME.federate.ifc`, from the models that have it. A model left blank goes into nothing. A run adds models it hasn't seen to the register and keeps what's filled in. Without `--register`, each zone goes into a file of its own, by the third part of the file names (`--name-part`).

`ifcfilter split --combine combine.csv` does it as part of a run from the models: a model with a name in the register has its federation file written to `parts/DISCIPLINE` instead of `federate`, and after the run each name is written to `federate/DISCIPLINE/NAME.federate.ifc` from its models' files, so what's combined is only federated once. A combined file is written again if it's missing, or older than any of its models' files or than the register. A model taken out of a group goes back to `federate` the next time it's done.

What's combined has one project, named after the file, one set of units and contexts, one site and one building, the first model's, and levels of the same name in any of them are one level, in alphabetical order with `Non-Managed Types` last; a site's level stays under its type's. What the others' site, building and levels placed is placed from what took their place, where it was, as models can have their site in different places, so nothing moves. Everything else is copied as it is, renumbered; GUIDs are already unique across a run, and an application such as Revit is only written once. Models whose length, area or volume units differ aren't combined.

## Mapping data

`mapping.csv` lists every class, text attribute, property set and property the models have, to say what the federation file should have instead. `ifcfilter inventory` writes it from the inputs, so its names are always the models' own:

```sh
ifcfilter inventory MODELS_FOLDER... --mapping mapping.csv
```

Each row is a `class`, an `attribute` of a class, a `set` (property or quantity set) or a `property` in a set, with how many elements have it, in how many models and which disciplines, the classes it's on and some example values. Running it again updates the counts and keeps whatever is in `action` and `new_name`, along with rows the models no longer have.

In Excel, fill in `action`, and `new_name` for a rename:

| Row | `delete` | `rename` |
|---|---|---|
| `class` | | Becomes the class in `new_name`, such as `IfcColumn`. Both have to be kinds of `IfcElement`; what only the old class had, such as its predefined type, is left out |
| `attribute` | Left empty, if the schema lets it be | |
| `set` | Gone, with its relationships | Renamed |
| `property` | Gone from that set | Renamed in that set |

A blank `action`, or `keep`, leaves a row as it is. `ifcfilter split --mapping mapping.csv` does what it says to the federation file only, after compact writing; the hidden and envelope files keep the models' data. A set left with no properties goes, as do properties no set has any more. A run stops before it starts if the file has an action it doesn't know.

## Status

Planned:

1. `ifcfilter report`: where a model's bytes go, explosions, envelopes and site ID use. Suspiciously large elements are listed by GUID, to look at before anything culls them. Done.
2. Split into the three files and the manifest. Things enclosed in other things, like slab reinforcement, and things out of sight, like buried piles, go to the hidden file: by rule so far, with no geometric test. Done.
3. Compact writing. Done.
4. Merge by site ID. Done.
5. Instancing repeated shapes. Done.
6. Tessellation fixes: merging flat faces, then simplifying within a tolerance.

## Build

```sh
nix develop
cmake -B build && cmake --build build
./build/ifcfilter report MODEL.ifc --csv families.csv --suspects suspects.csv
```

The flake builds IfcOpenShell's parser from the `ifcopenshell-v0.9.0` tag, with the IFC2X3, IFC4 and IFC4X3_ADD2 schemas and no geometry engine.

Without an installed IfcOpenShell, CMake builds the same thing from the `third_party/IfcOpenShell` submodule, pinned to that tag. Clone with `--recurse-submodules`, or run `git submodule update --init` afterwards.

### Windows

This builds with Visual Studio 2022. Dependencies come from [vcpkg](https://vcpkg.io), and IfcOpenShell from the submodule. In PowerShell, with `VCPKG_ROOT` set:

```powershell
git submodule update --init
vcpkg install boost-algorithm boost-config boost-dll boost-dynamic-bitset boost-filesystem boost-lexical-cast boost-locale boost-logic boost-math boost-multi-index boost-optional boost-property-tree boost-uuid boost-variant zlib openmesh sqlite3[fts5] zstd --triplet x64-windows

cmake -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
.\build\Release\ifcfilter.exe split MODELS_FOLDER --out OUTPUT_FOLDER --rules rules.txt
```

The parser's schemas are plugin DLLs, loaded from beside `ifcfilter.exe`, so keep the DLLs in `build\Release` with it.

Paths are safest in plain ASCII, as command line arguments come in using the system's code page.
