# IFCFilter

Prepares an IFC model for federation. It takes one model and splits it into these, under the `--out` folder:

| Output | Holds |
|---|---|
| `federate/DISCIPLINE/STEM.federate.ifc` | What should be federated, with tessellation explosions fixed and elements merged by site ID or design tag where appropriate |
| `parts/DISCIPLINE/STEM.federate.ifc` | With `--combine`, the federation file of a model that's combined with others, instead of in `federate` (see [Combining models](#combining-models)) |
| `federate/DISCIPLINE/STEM.federate.manifest.csv` | Beside each federation IFC, one row per contributing source element, with the output GUID, source file and GUID, and project package code and name, from the `project-set` set's `project-code` and `project-long-name` properties |
| `non-federate/STEM.hidden.ifczip` | Geometry filtered out because it isn't visible or important to the federation, such as reinforcement inside a slab |
| `non-federate/STEM.envelopes.ifczip` | Space-proofing and clash shapes that aren't physical, typically transparent envelopes around things |
| `manifest/STEM.manifest.csv` | Where each original element went, its new GUID, what was done and why |
| `register.csv` | What the outputs were built from: each model's file name, path, size, dates and SHA-256 hash |

`STEM.original.ifc` is a link to the input, to compare in a viewer. Windows only allows links in developer mode, so without it this file is left out, with a note.

The federation file is plain `.ifc`, as that's the format Bentley's list of supported formats names. Federation files are in a folder for each discipline, named after the folder the input is in, as there are hundreds of them; a run moves any it finds where an older run put them, straight under `federate`. The other two are ifczips, each a zip holding one IFC file, and `--no-zip` writes those as plain `.ifc` too.

The federation target is Bentley Infrastructure Cloud, under IFC4, so an IFC2X3 model is written as IFC4. Geometry and properties are initially copied as they are, and what IFC4 changed is rewritten before the federation properties are reduced as described below:

- Attributes IFC4 added to the end of a type are written as `$`.
- `IfcRelConnectsPortToElement` becomes `IfcRelNests`, with the same GUID, as IFC4 only lets the old one relate to distribution elements, and ports are often on proxies.
- A port goes in whichever file what it's on goes in, and when parts merge their ports are on the asset. A port on nothing, as some exporters leave, is left out. A port is also contained in its host's storey, or its assembly's, as well as nested on the host, as some checkers don't follow nesting and list ports as unreferenced otherwise.
- A predefined type IFC4 doesn't have, such as `.TELEPHONE.`, becomes `.USERDEFINED.`, with the old value as the element type if there isn't one.

Everything is then in metres. A model in another SI unit, such as millimetres, is written again first, with every length, area and volume scaled, and its units made metres, square metres and cubic metres, so that models combine and compare as they are. What a number measures is found from the schema: attributes typed as length, area or volume measures, such as coordinates, depths and profile sizes, and values that name their type, such as a property's `IFCLENGTHMEASURE(1500.)`. A context's precision and a trim along a line are lengths too. A value with a unit of its own, such as a property that names one, keeps it, and a map conversion with a map unit of its own keeps its offsets, with its scale changed to match. A model whose units aren't SI, such as feet, is left in them, with a warning. The log says how many lines changed.

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

`ifcfilter cache` writes a viewing cache of a model, an `.ifcc` file a viewer can load in pieces: its meshes once each, placed where they're used, in spatial tiles, with the tree and properties in indexed tables. Extrusions are meshed from their profiles, and each mesh has simplified versions for far away, its points snapped to 5 cm, 25 cm and 1 m grids. It's a prototype, read by [IFCViewer](https://github.com/jwmarsden/IFCViewer); `docs/cache-format.md` is the format.

```sh
ifcfilter cache MODEL.ifc --out MODEL.ifcc
ifcfilter cache A.ifc B.ifc... --out GROUP.ifcc
```

Given several models, it writes them into one cache, each under an element of its own named after its file, with `--out` required. Some details of how a cache is built:

- Mesh points are snapped to a 0.1 mm grid in metres before meshes are compared, so shapes that are the same but for rounding, such as a tunnel's rings, are stored once and placed many times.
- A straight extrusion along its own axis is stored as one of 1 m and stretched to its length where it's placed, so every pipe or beam of one profile shares a mesh.
- Tiles are a loose octree, split past 4,000 instances or 400,000 triangles.
- A model of things that sit behind other things, such as pipes or cable trays, also has its tiles split once they're more than 150 m across, so a viewer can leave out what's hidden or off screen. `--tile-spread M` sets another width, and 0 doesn't split by width.
- Width alone decides. A cluster such as a building or a room of plant stays in a few big tiles, which are few draws for a viewer, while a long run such as a road's lights or a drainage network is split along its length.
- Simulated on a large project's caches in Nov 2026, this needed half the tiles of the old rule (more than 40 m across with more than 64 instances), and no tile of what gets hidden was more than about 300 m across. Models whose file name has `-LA-`, `-RW-`, `-TU-`, `-CV-` or `-CC-` in it are ground, road, tunnel and concrete, which hide rather than get hidden, so their tiles aren't split by size. Keep that code in a cache's name, as the viewer goes by it too.
- A model with nothing to draw has no cache written, and an older one is removed.
- On Linux, Open CASCADE can crash outright cutting some elements, in its Boolean operations, every time. So the geometry build cuts in a child process: if it crashes, what's left is cut one at a time in four processes, and an element it crashes on is named in a warning and drawn uncut. The rest of the cache is unaffected. Windows cuts in the process itself, as before.
- An element whose openings or Boolean cuts are too big to cut quickly (see the limits under [Windows](#windows)) is drawn uncut and marked approximate. `--detail` tries those again with no limits, and `--detail-element GUID` tries only that one; if a named element still can't be cut, the cache isn't replaced. Both need the geometry build.

`scripts\mirror.ps1` keeps a copy of the output folder up to date elsewhere, such as in a synced folder. It won't run on a folder with a run going on, `-List` says what would change, and `-Keep` names files and folders of the copy's own that it leaves alone, such as `-Keep update.md,rules.txt,test`. Navisworks' `.nwc` cache files are never copied.

No GUID is written twice in a run, across all its outputs:

- Whatever is in only one output keeps its GUID. Anything in several, such as a split element or the project and spatial structure each output needs, keeps it in the federation file and has one made from it in the others.
- A GUID in more than one model changes in all of them. That goes for anything with a GUID, including a project, site or building that several models share, so they no longer match across federated files.
- A GUID repeated inside a model stays on the first thing that has it.

GUIDs are made from the old GUID and the model's file name, so they're the same on every run. A run can only see the models it's given, so models to be federated together should go through together. The manifest lists each element's GUID in each output.

Elements with the same site ID (`site-id`, in the `site-set` sets) are one asset, so the federation file has them as one element (`--no-merge` skips this):

- It's an `IfcBuildingElementProxy` named after the site, with a GUID made from the site ID and the model's file name. Its body is the elements' shapes, written again where they were: meshes and breps with their points moved, solids with their position, and booleans and maps followed down to what they hold, with each shape's colours. Open IFC Viewer won't select an element by clicking it when its body is many mapped items, until it's been opened from the tree, so clicking any part of a gantry or a rail selects the whole of it this way. If a part has a shape that can't be moved like this, such as a swept disk, the asset's body is a mapped item of each element's body instead, placed where the element was, and the log says how many and why. Geometry and colours are as they were either way, though a shape repeated within an asset is written for each time it's used.
- Original element property sets, including quantities and inherited type property sets, are removed from the federation output. The project set and IFCFilter set described below replace them. The adjacent manifest says which source elements went into each asset.
- A blank, `N/A`, placeholder, `TBA`, `TBC` or `No Linear Assets Req` site ID isn't an asset, nor is one ending in `xx`, which stands for a range of sites, or in six zeros, such as `ABC000000`, a class of asset with no site given yet, nor is one with no digits in it, such as `ABC` or `ABCXXXXXX`, which is a class of asset too. A site with one element stays as it is.
- A shape with no style of its own takes its colour from its element's material, which isn't kept, so it comes out in a viewer's default colour.
- Openings that cut a part now cut the asset. They reach other parts only where those meet what they cut: on one model's gantries, all they'd take was 0.3 litres from 40 bolts and nuts, presumably inside their holes, which can't be seen.
- A site is only merged within a model, so one in two models stays as one element in each.
- In the federation file the building has a level for each asset type, a building storey named after the asset type its site IDs start with: their letters, as in `AB120101`, or `RC1`, `RC2` and `RC3`, whose number is part of the type, in alphabetical order, and last a level called `Non-Managed Types` for everything with no site ID. A site that's one asset, or one element, is on its type's level as itself, renamed to its site ID (Open IFC Viewer won't select an element alone on a level by clicking it, so sites aren't levels of their own). A site still in several pieces is a partial level on its type's level, named after the site ID, holding them. The model's own levels are left out once they're empty, which they all are unless something such as a space is made of them. Each new level has a placement of its own where the building is, and what's on it is placed from it, where it was, by changing its own placement so that its ports and openings follow: a viewer finds what a clicked thing is on through placements, and Open IFC Viewer can't when levels share the building's.
- An element with no site ID goes by its design tag (`design-tag`, in the `design-set` sets), if that's an ID: not a placeholder, with a digit in it, as `LO` or `BOLLARD` hasn't, with no spaces, as a description has, and not on more than half the model's tagged elements, as a code for the whole model is. If its tag is only on one site's parts, it joins that site; otherwise elements with the same tag are an asset, named after it, and the manifest says `design tag X`. A tag can still name a type rather than one thing, as TU's deck units' do: each has a mark of its own (`mark`, in the `mark-set` sets), so elements with a tag only go together if their marks are the same, or they have none. Where pieces of a type have no marks, as in BS, the 5 m gap keeps panels of one type apart only where they don't touch.
- A placeholder site ID isn't an asset, but for an asset type named by a `merge-runs` rule, such as a placeholder type for pipework, its elements are merged as runs instead: each group of parts that touch, within 5 cm of one another, is one asset, named `run of` the placeholder. A pipe network modelled as thousands of separate segments and fittings comes out as a few dozen connected runs, each selectable as a whole.
- A part with no site ID of its own takes its assembly's, where a `site-from-assembly` rule names the assembly's IFC class (or `*` for any): a Revit stair (`site-from-assembly IfcStair`) has the site ID, while its runs and landings, which are its shape, have none, so without the rule they stay out of their site and the rest of it can be too far apart to merge. A part with its own site ID, such as a railing on the stair, keeps it.
- Parts of a site are only merged if each is within 5 m of another (`--merge-gap M`, and 0 doesn't check), as two things far apart with one site ID, such as two cameras, are a mistake in the data. The gap is between boxes around the parts, so it's never more than the real gap, and parts that can't merge still count as what joins the rest. Groups that are far apart are merged as separate assets, a part on its own stays as it is, and the manifest says which. A part whose reach can't be worked out stays as it is too.

Every element of the federation file has an `IFCFilter` property set saying where it came from, so that what's found in a federated model can be traced back and sorted:

| Property | Holds |
|---|---|
| `SourceFile` | The model's file name |
| `Zone` | Its zone (`zone-property`, in any set), if that's one of the `zone` lines; otherwise the zone in the model's file name, if there is one. A model of a group of zones, gives none of its own |
| `SiteID` | Its site ID, as merging reads it |
| `Asset Type` | How the site ID starts, as its level is named, such as `ABC` for `ABC301102` |
| `Location` | Its location (`location-property`, in any set), such as `06203-L003-SL011` |

A value that's blank, `N/A` or a placeholder is left out, and a merged element has each of its parts' values, joined with `; `. Source GUIDs are in the adjacent manifest, not in this set. Each federation element also gets the most common complete `project-set` from its source model, before `IFCFilter`; a model without that set has no project set to copy. Both sets go on after the mapping. The manifest connects each output GUID to each source file, source GUID and its project package code and name; the original model remains the source of other removed properties.

Each output is then tidied:

- `cull-set` sets are left out, once merging has used their design tags, along with what only they had.
- What the `duplicate-set` repeats from another set on the same elements, with the same value, is left out of it, and it goes if that's all it had. What only it says stays.
- Property sets are in alphabetical order, ignoring case: their relationships are written last, numbered in that order, as viewers list sets in the order of those.
- The header names the output and the time it was written, gives IFCFilter as the preprocessor with the one before it in brackets, and its description notes the source. The originating system and author stay as they were. Owner histories are marked `.MODIFIED.` then, by an IFCFilter application.
- The project is named after the model, with its `project-long-name` as its long name, the site as `site-name`, and the building after its `building-name`, both in the `project-set`, the values most of the model has. Without a `project-set` rule there's no project set: nothing is copied, and combining doesn't look for one. Levels lose their Revit level type.
- Elements with a site ID, and what they're made of, are named after it and described as their `asset-component`. An `asset-type` that isn't how the site ID starts is corrected to it, with a property of its own, as the old one may be on other sets; a set on elements of different types is left as it is. Anything else is named as its family and type, without the Revit element ID Revit adds to the end, as that's its tag already.

Faceted breps, which Revit writes for most families, and surface models made of shells of faces, which Civil 3D writes, are first written as polygon face sets of the same faces, so that instancing and compact writing work on them too (part of compact writing, so `--no-compact` leaves them). A brep with voids, or not made of plain faces and loops, stays as it is, as does one of an element an opening cuts, and any model that isn't IFC4.

Face sets that are the same shape, moved and turned, are written once and placed with mapped items (`--no-instance` skips this). A copy has the same faces and colours as what it's placed from, and every point within `--precision-mm`, with no mirroring. This goes before compact writing, so copies can't drift apart by being rounded differently. It saves most where a model repeats parts, such as a gantry's bolts and signs, and next to nothing on road meshes, which are all different.

A body of several face sets isn't instanced set by set, as Open IFC Viewer won't select an element by clicking it when its body is many mapped items. A whole body that's the same as another, moved and turned, is placed from it instead, as one mapped item, and so is the map of several sets that Revit makes an element's body of.

Where a family doesn't say what a thing is, as with a road's meshes, `keep-if`, `hidden-if` and `envelope-if` rules go by a property's value instead: `SET.PROPERTY=VALUE`. `keep-if` keeps the geometry in the federation even when another rule or transparency would put it elsewhere; for example, an exposed pavement layer labelled Base can be kept while other Base layers are hidden. A `hidden-small-parts-if` rule picks out assemblies the same way, and puts their parts under a quarter of the assembly's size in the hidden file: a sign face's text, where the exporter left broken pieces of letters over the real ones, while its plate, border, symbols and the real text stay. A `transparent-site SITEID T` rule draws that site's elements see-through, from 0, as they are, to 1, invisible: the styles of what only they use are copied with that transparency, and their colour maps get its opacity, so other elements keep their colours. A shape another element shares, such as one an instance maps, isn't changed, but a mapped item of the site's own is given a transparent style of its own. The log says how many styles and maps changed. A `cull-at-origin METRES` rule leaves out any body within that many metres of the world's 0,0 across, but only where fewer than 1% of the model's bodies are there, so a model drawn about 0,0 keeps everything: Civil 3D can leave a dummy surface there whose placement cancels the site's, stretching the model's box to thousands of kilometres. What's culled is in no output, so verification lists it as an input product not in the manifest. A `forward-if SET.PROPERTY=VALUE MM` rule moves such assemblies forward by that many millimetres, off the post a sign face is often modelled flush against, so the two don't flicker in a viewer. Forward is the side of the plate its legend is on, or, where the legend is in the plate's plane, away from what touches it.

`hidden-tree-if SET.PROPERTY=VALUE` also hides the descendants of a matching assembly, for example a sign footing whose geometry is held by child elements. Bracketed property-set names containing dots, such as `[Generic].[Graphic3d]`, can be used in property rules. A `keep-if` rule still takes precedence for an individual element.

Any transparent geometry of an `IfcDoor` goes to the envelope file, even below the general transparency threshold: Revit's semi-transparent `System-Zones` door boxes are often 50% transparent. A `physical` family rule still keeps transparent physical parts such as chainwire, while transparent `System-Zones` parts go to envelopes even on those families. A `keep-if` rule takes precedence.

Each output is then written compactly (`--no-compact` skips this); federation element properties are reduced afterwards:

- Coordinates in point lists are rounded to 0.1 mm (`--precision-mm`), from the project's length unit. Placements aren't touched.
- Polygon face sets become triangle lists, holes included. A set stays as polygons if it has a polygonal texture map or a face that can't be cut into triangles of the same area. Areas are measured from each face's first point, as products of world coordinates, such as N6124295, lose more than a small face's area.
- Detail smaller than the rounding goes: on one model's light fittings, 0.1 mm rounding loses 0.17% of the surface, in faces a few tenths of a millimetre across. `--precision-mm 0.01` keeps it, for a file about 6% bigger.
- Faces and triangles with no area once rounded are left out, along with their colour map entries. A set made of nothing else keeps them, with a warning, as a face set can't be empty.
- A face set of an element an opening cuts, or that an opening is made of, isn't changed in any way, nor instanced: all of the element's, not only those the opening reaches. Taking a solid from a mesh is fragile: rounding or triangles were seen to change what IfcOpenShell cuts, and in some elements to leave next to nothing, and on a merged gantry, rounding the parts an opening doesn't reach changed what it took from those it does by up to 55 litres. These elements come out as the input's do, whether that's right or not, which makes cut assets, such as gantries, bigger.
- Identical property values and sets are written once, with one relationship per set listing everything the set is on. A shared set keeps the first one's GUID.

Then, in the federation file only, triangle meshes with more points than their shape needs, such as pipes and fittings drawn with 24-sided circles, are simplified within 2 mm (`--simplify-mm`, and 0 doesn't). Points are only taken away, never moved, so what's left is rounded as before:

- A point that's kept is within the tolerance of the planes of all the faces it stands for, each plane counting the same (OpenMesh's own weights them by area, which in metres let points move hundreds of millimetres), and a point that goes is within it of the triangles that take its place.
- Open edges stay where they are, as do the corners of any triangle the mesh can't take, such as where three or more share an edge; those triangles are written as they were.
- Points written the same are one point first, so that pieces that meet are joined: Revit's jet fans in TV repeat each point three times over.
- A mesh an opening cuts isn't simplified, nor one with a different colour or texture for different triangles, or normals of its own. A colour map with more entries than triangles, all the same colour, as Revit writes for the jet fans, is written again with one each.

Measured as how far each point of the original is from the simplified surface, over the 40 products each that lost the most: a drainage model at most 1.04 mm, a hydraulics model 1.38 mm and a retaining wall model 1.92 mm. They went from 9.1 to 3.0 MB, 25.1 to 11.4 MB and 17.8 to 9.6 MB, and a tunnel ventilation model, which is mostly its jet fans, from 389.7 to 81.2 MB. It uses OpenMesh's multiple choice decimater, which picks each collapse from random samples, with the same seed for every mesh, so a run comes out the same each time: its other decimater, which keeps every collapse in order, took up to a minute a set on Revit's fans of thousands of triangles around a point, and a bound on the distance from all that's been taken away (ModHausdorffT) took 491 s on one of DU's sets.

## Combining models

`ifcfilter combine` writes federation files as one, such as all of a zone's decks:

```sh
ifcfilter combine FEDERATE_FOLDER --out OUTPUT_FOLDER --register combine.csv
```

`combine.csv` says, model by model, what each goes into; `combine.csv.example` shows the form, with two models each going into a file of decks and one of tunnel for one zone, one left out with a blank name, and another zone's decks. Like `rules.txt` and `mapping.csv`, it's the project's, so it isn't in the repository. The first time, it's written listing every federation file in the folders given, with its discipline, zone, size, how many elements it has and the three names most of them have, such as `DEK TYPE 1A MC02 - 2400 (412)`, to tell decks from rings, and an empty `combined` column, and nothing else is done. Fill that in, in Excel, with the name of the file each model goes into, and run it again: each name is written as `NAME.federate.ifc`, from the models that have it. A model left blank goes into nothing. A run adds models it hasn't seen to the register and keeps what's filled in. Without `--register`, each zone goes into a file of its own, by the third part of the file names (`--name-part`).

`ifcfilter split --combine combine.csv` does it as part of a run from the models: a model with a name in the register has its federation file written to `parts/DISCIPLINE` instead of `federate`, and after the run each name is written to `federate/DISCIPLINE/NAME.federate.ifc` from its models' files, so what's combined is only federated once. A combined file is written again if it's missing, or older than any of its models' files or than the register. A model taken out of a group goes back to `federate` the next time it's done.

What's combined has one project, named after the file, one set of units and contexts, one site and one building, from the model with the most contributing source elements (ties go by file path). Its project set is assigned to every federation element. The adjacent manifests are required and combined into one alongside the new IFC, keeping every element's original project information. Levels of the same name in any model are one level, in alphabetical order with `Non-Managed Types` last; a site's level stays under its type's. What the others' site, building and levels placed is placed from what took their place, where it was, as models can have their site in different places, so nothing moves. Everything else is copied as it is, renumbered; GUIDs are already unique across a run, and an application such as Revit is only written once. Models whose length, area or volume units differ aren't combined.

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
6. Tessellation fixes: simplifying within a tolerance. Done; merging flat faces isn't.
7. Combining models into one federation file each, by a register. Done.
8. `ifcfilter cache`, the viewing cache. A prototype.

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

To apply opening cuts and Boolean operations in viewing caches, build the Open CASCADE submodule and enable IfcOpenShell geometry. This also needs the vcpkg packages `eigen3`, `boost-program-options`, `boost-regex` and `boost-foreach`:

```powershell
git submodule update --init
vcpkg install eigen3 boost-program-options boost-regex boost-foreach --triplet x64-windows
cmake -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DIFCFILTER_BUILD_OCCT=ON
cmake --build build --config Release --target ifcfilter_occt
cmake -B build -DIFCFILTER_BUILD_OCCT=OFF -DIFCFILTER_WITH_GEOMETRY=ON "-DOpenCASCADE_DIR=$PWD/build/occt-install/cmake"
cmake --build build --config Release --target ifcfilter
```

Keep the geometry kernel, mapping and Open CASCADE DLLs copied beside the executable as well.

On Linux, in `nix develop`, the same two steps into a folder of their own, so the parser-only `build` stays as it is (Open CASCADE takes about an hour on four cores):

```sh
git submodule update --init --depth 1
cmake -B build-geometry -DIFCFILTER_BUILD_OCCT=ON
cmake --build build-geometry --target ifcfilter_occt -j3
cmake -B build-geometry -DIFCFILTER_BUILD_OCCT=OFF -DIFCFILTER_WITH_GEOMETRY=ON -DOpenCASCADE_DIR=$PWD/build-geometry/occt-install/lib/cmake/opencascade
cmake --build build-geometry --target ifcfilter -j3
python3 tests/cache_cuts.py build-geometry/ifcfilter
```

Open CASCADE's libraries are installed to find one another beside them (`$ORIGIN`), as the geometry kernel loads them as a plugin; without that, every cut falls back to uncut with `libTKFillet.so.7.9: cannot open shared object file`. By default an element stays uncut and is marked approximate if it has more than 32 opening relationships, more than 192 opening-and-shape-item combinations, or an opening mesh with more than 512 faces, as very large cuts can take hours to process. Set `IFCFILTER_MAX_OPENINGS`, `IFCFILTER_MAX_CUT_WORK` or `IFCFILTER_MAX_OPENING_FACES` at CMake configuration to change a limit, or to `0` to disable it. The parser-only build leaves opening cuts and Boolean subtractions uncut and marks affected elements approximate.

With a geometry-enabled build, `python tests/cache_cuts.py build/Release/ifcfilter.exe` checks opening subtraction, Boolean differences and the default complex-opening fallback.

Paths are safest in plain ASCII, as command line arguments come in using the system's code page.

### Property sets and Uniclass

`IFC_DATA_DIR` points both IFCFilter and IFCViewer to the shared reference directory. On this workstation it defaults to `%USERPROFILE%\OneDrive\Work\IFCData`; set the environment variable to the appropriate path on other machines. `site-id-attributes.xlsx` in that directory is the authoritative list of allowed property sets and their property names. Pass another with `split --property-schema PATH` if required. The repository's `scripts/regenerate.bat` specifies the shared workbook explicitly. Each row names a set and property; the `Element` rows refer to standard IFC element fields rather than a property set. The output uses the listed names, fills missing properties with blank values and excludes other property sets. The viewer cache follows the same list. Editing the workbook changes the build fingerprint, so affected federation outputs are regenerated on the next run. The filter stops if it cannot find a property-schema workbook.

Values for workbook-listed properties are taken from the source IFC when available. The `Classification` properties use source Uniclass codes and IFC classification references, and the corresponding active-code titles from the shared directory's `uniclass` folder when a description is missing.

`IFC_DATA_DIR/site-id-lookup.xlsx` supplies optional SiteID-specific corrections. Non-empty cells override matching listed properties; blank cells leave their source values unchanged. The schema workbook controls which properties are actually written, so a correction column has no output unless its property is listed there. `IFC_Filter.Model_File`, if listed, is the file name of the model each element came from, such as `MODEL.ifc`; a combined file keeps each element's own, and a lookup can't change it. Site IDs have an alphabetic asset type followed by exactly six digits (RC1, RC2 and RC3 are also valid types); `000000` placeholders are not single sites. Invalid lookup IDs and duplicate rows stop the run. Changes to either workbook invalidate incremental outputs.

For individual source elements, `IFC_DATA_DIR/site-id-corrections/DISCIPLINE/MODEL.xlsx` is read before grouping. The first worksheet must have `Asset Type`, `Site ID`, `Review status`, `Source GlobalId` and `Source model` columns. Only rows marked `Approved` with a non-empty Asset Type or Site ID are applied. The source model must match the IFC filename, the GlobalId must uniquely identify an IFC product, and a supplied Site ID must be valid and agree with the supplied Asset Type. Missing GlobalIds are reported for review rather than guessed. A blank field leaves its existing value in place; Site IDs are not generated. An optional `Transparency` column, from 0 to 1, needs a Site ID and draws that site see-through in the model's federate file, as `transparent-site` does; it overrides the rule for that model. Corrected Site IDs participate in normal within-model grouping and the resulting IFC_Filter properties, after which the SiteID lookup can supply its usual overrides. Changing, adding or removing a model's workbook invalidates that model's incremental output. A `join-across-models TYPE [DISCIPLINE]` rule (`*` for every type) joins a site's elements across the models a combined file is made of: the element from the model of that discipline (the fourth part of its file name), or else the first model's, keeps its own properties and place, and the others' bodies are placed in its body by mapped items, where they were, keeping their styles, so a transparency from a correction stays on just those shapes. The others go, with what only they had, and the combined manifest traces their source elements to the element they joined. An element that holds or places others isn't joined, and the log says so. Automatic rematching after a GlobalId changes is not implemented.

`IFC_DATA_DIR/location-referencing.xlsx` is matched against the complete `IFC_Filter.Location` value after SiteID corrections. A unique `LocationHierarchyDescription` populates `Location_Description`; unique Co, En and SL codes and titles from that row take precedence for the corresponding classification properties. Missing or conflicting values are not guessed. Changes to the workbook invalidate incremental outputs. A legacy CSV is used if the workbook is absent. The filter removes disconnected, unlisted property definitions while retaining definitions referenced by other IFC objects.

`IFC_DATA_DIR/site-id-invalid.xlsx` retains the excluded invalid rows for review. `IFC_DATA_DIR/site-uniclass-review.xlsx` lists source classification conflicts and codes not found as active entries in the supplied tables. `ifcfilter site-uniclass-inventory INPUT --rules rules.txt --out scan.csv` scans source IFCs and writes the results and a separate review CSV; it does not overwrite the lookup. `IFC_DATA_DIR/site-register.xlsx` is the earlier cache snapshot; it is a report rather than the correction lookup. Values exceeding Excel's cell limit are stored in numbered pieces on its Overflow sheet and restored by the register reader. `scripts/site-merge-candidates.py` creates a separate review list of cross-file source elements from that snapshot and the federation manifests. Its output is only a candidate list: the cross-file geometry merge and manual GlobalId assignment are not implemented yet.



