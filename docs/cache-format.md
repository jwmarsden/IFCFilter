# IFCFilter cache format (.ifcc)

Draft 0.2, 2026-11-09. `ifcfilter cache` (`src/cache.cpp`) writes it, as a prototype: see Prototype, at the end.

A viewing cache made from one IFC model, the way Navisworks makes an `.nwc`: the IFC is read once, by `ifcfilter cache`, and the viewer only ever reads caches. Everything a viewer needs is laid out to be loaded in pieces: geometry in spatial tiles, each shape stored once, and the tree and properties in indexed tables.

## Goals

- Opening a model takes about the same time whatever its size: the viewer reads the metadata and the tile index, and then only the geometry in view.
- Each shape is stored once and placed where it's used, so the GPU can instance it. IFC mapped items, and what IFCFilter's instancing found, carry straight over.
- Clicking anything selects its element, however it's instanced or nested.
- The tree, properties and filters (all elements with this site ID, all of this asset type) are indexed queries, not walks over the model.
- A federation is a list of caches, loaded into one scene.
- The same input and the same IFCFilter build give the same file.

Not goals: editing, writing IFC back, exact geometry (it's triangles), drawings, textures.

## Container

One SQLite 3 file, with SQLite's usual 4 KiB pages: 64 KiB pages made the FS model's cache 70% bigger, as the small tables and indexes waste most of theirs, and blobs bigger than a page are fine in a chain of them. A viewer can open it read-only, and several viewers can share it.

SQLite because it's one file, it's indexed, every language reads it, and it can be looked into with `sqlite3` while this format is young. If blob reads ever become the bottleneck, geometry can move to a second file of plain blocks, addressed by offset, without changing anything else.

All integers are little-endian. Blobs are compressed with zstd unless their table says otherwise; the backend decompresses them before they reach the frontend.

## Coordinates

- Metres, Z up, as the model's length unit converted.
- The cache has one **origin**, in double precision: the centre of the model's box, rounded to the metre. All geometry and transforms are stored relative to it in float32, so models in a projected grid, with coordinates in the hundreds of thousands, keep millimetre precision on the GPU.
- In a federation, the viewer picks one scene origin and moves each cache by the difference between its origin and the scene's, in double precision, before going to float32.
- Where the model has an `IfcMapConversion`, it's kept in `meta`, so caches can be placed on a map. Otherwise caches federate in the coordinates the models share.

## Tables

### meta

| key | value |
|---|---|
| `format` | `ifcc` |
| `format_version` | `1` |
| `generator` | `IFCFilter 0.1.0` |
| `created` | UTC time, ISO 8601 |
| `source_name` | the IFC's file name |
| `source_sha256` | the IFC's hash: a cache is rebuilt when this or `generator` changes |
| `schema` | `IFC4`, `IFC2X3` |
| `origin` | `x y z`, metres |
| `bbox` | `minx miny minz maxx maxy maxz`, relative to the origin |
| `map_conversion` | eastings, northings, height, x axis, scale and CRS name, if the model has one |
| `element_count`, `mesh_count`, `tile_count`, `triangle_count` | for the viewer to size things up front |

`meta(key TEXT PRIMARY KEY, value TEXT)`

### strings

Every name, property name and text value, once.

`strings(id INTEGER PRIMARY KEY, text TEXT UNIQUE)`

### elements

Every `IfcProduct` and spatial structure element, the project included. `id` is dense, from 0, and is the element's picking ID.

| column | |
|---|---|
| `id` | INTEGER PRIMARY KEY |
| `guid` | TEXT UNIQUE |
| `class` | string id: `IfcBuildingElementProxy` |
| `name`, `description`, `object_type`, `tag` | string ids, or NULL |
| `parent` | element id in the tree: an element's container or the whole it's part of (`IfcRelContainedInSpatialStructure`, `IfcRelAggregates`, `IfcRelNests`); NULL for the project |
| `sort` | its order among its parent's children: the IFC's order, or by name |
| `type_name` | string id of its type object's name, or NULL |
| `minx` … `maxz` | REAL, its box relative to the origin; NULL if it has no geometry |
| `tile` | the tile its instances are in, or NULL |
| `flags` | bit 0 has geometry; 1 hidden by default (spaces); 2 envelope; 3 merged asset; 4 drawn approximately (boxes, or what an opening or clipping takes away left on) |

Indexes: `(parent, sort)`, `(class)`, `(guid)`.

The tree is the model's own: project, site, building, then IFCFilter's asset type levels, then sites. The viewer doesn't regroup by class.

### Properties

Property sets are stored once and shared, as IFCFilter's compact writing does: most sets are on many elements with the same values.

| table | columns |
|---|---|
| `psets` | `id`, `name` (string id), `kind` (0 property set, 1 quantity set, 2 type's set, 3 attributes and materials) |
| `pset_values` | `pset`, `name` (string id), `value`; keyed on all three, without a rowid, so a set's values are stored together |
| `values` | `id`, `type` (0 text, 1 real, 2 integer, 3 boolean, 4 enumeration), `text` (string id), `number` (REAL), `unit` (string id) |
| `element_psets` | `element`, `pset`; keyed on both, without a rowid |

Indexes: `element_psets(pset)`, `pset_values(name, value)`, `values(text)`, `values(number)`. A table keyed on its columns is its own index for them, so these are all the lookups need.

A filter such as `SiteID = PU290101` is: the string id of each, then `pset_values` by `(name, value)`, then `element_psets` by `pset`. That's three index lookups, whatever the model's size.

An element's materials, type and classification go in a set of kind 3, so the viewer shows them the same way as properties.

A full-text index over `strings`, `strings_fts` (`fts5`, with `strings` as its content), gives the search box: it finds string ids, and names, tags and values lead from them to elements.

### materials

The colours shapes are drawn in, from `IfcStyledItem`, `IfcIndexedColourMap` and, where a shape has none, its element's material.

`materials(id INTEGER PRIMARY KEY, r REAL, g REAL, b REAL, a REAL, name TEXT)`

Material 0 is the default grey.

### meshes

Each shape once. Blob layout below.

| column | |
|---|---|
| `id` | INTEGER PRIMARY KEY |
| `vertices`, `triangles` | INTEGER |
| `minx` … `maxz` | REAL, in the mesh's own coordinates |
| `lod` | always 0: simplified versions are in `mesh_lods` |
| `blob` | BLOB, zstd |

### mesh_lods

Simplified versions of the meshes with 48 triangles or more, for far away. Each level snaps the full mesh's points to a grid, each cell's points to their average, and drops triangles left with no area or the same as another; meta `lod_cells` gives the grids, in metres, from level 1 (0.05, 0.25 and 1 m). A level is kept only where it has at most 70% of the triangles of the level before; one that isn't kept draws as the level before it. A level with no triangles has no blob: the mesh isn't drawn that far off. The points stay in the mesh's own frame, so the same instances place them.

`mesh_lods(mesh INTEGER, lod INTEGER, triangles INTEGER, blob BLOB, PRIMARY KEY(mesh, lod)) WITHOUT ROWID`

The blob is a mesh blob, as below, with no edges.

### tile_lods

What each tile draws at each level, so a viewer can budget for it: `tile_lods(tile INTEGER, lod INTEGER, triangles INTEGER, PRIMARY KEY(tile, lod)) WITHOUT ROWID`, for every tile and every level from 1.

### tiles

A loose octree over the model's box. An element goes in the deepest node whose box, doubled, still holds the element's box, so large things (a road mesh, a wall) sit near the top and small things deep down. A node is split once it holds more than about 2,000 instances or 200,000 triangles.

| column | |
|---|---|
| `id` | INTEGER PRIMARY KEY; 0 is the root |
| `parent` | INTEGER, NULL for the root |
| `level` | INTEGER |
| `minx` … `maxz` | REAL, the box of everything in it and below it, so a branch out of view can be skipped whole |
| `size` | REAL, the largest element's diagonal in it: the viewer uses this to decide what's too small on screen to load yet |
| `instances`, `triangles` | INTEGER |
| `meshes` | BLOB, the mesh ids its instances use, u32 each, uncompressed |
| `blob` | BLOB, its instances, zstd |

The whole `tiles` table, without the blobs, is the tile index the viewer reads when it opens a cache: 100,000 tiles are about 5 MB.

## Blobs

### Instance (a tile's blob)

A tile's blob is a header followed by its instances, sorted by mesh, so instances of one mesh are together.

```
u32  magic 'IFCI'
u16  version 1
u16  flags (0)
u32  count
then count times, 64 bytes each:
u32  element      element id
u32  mesh         mesh id
u32  material     material id, or 0xFFFFFFFF for the default
u32  reserved
f32  transform[12]  3 columns of rotation and scale, then the translation, relative to the origin; the mesh's
                    centre is already in the translation
```

A shape that appears in only one element is still a mesh with one instance. What isn't instanced costs 64 bytes more, and it keeps one path through the viewer.

### Mesh

```
u32  magic 'IFCM'
u16  version 1
u16  flags: bit 0 32-bit indices; 1 float positions; 2 normals; 3 edges
u32  vertex_count
u32  index_count
u16  part_count
u16  reserved
f32  min[3], max[3]       the mesh's box, for dequantizing
part_count times:
  u32 first_index, u32 index_count, u32 material
positions:
  quantized: u16[3] per vertex, mapped linearly onto the box
  float (flag 1): f32[3] per vertex
normals (flag 2): two i8 per vertex, octahedral
indices: u16 or u32 (flag 0), three per triangle
edges (flag 3): u32 count, then pairs of vertex indices
```

- A triangle's colour is its part's material, unless that's 0xFFFFFFFF, then its instance's, and then the default (material 0). Colour maps make parts; a style on the shape or on the mapped item that places it makes the instance's.
- Each mesh is centred on its box, and the centre goes into its instances' translations, so a mesh written in a projected grid still has small coordinates.
- Positions are quantized to 16 bits on the mesh's box, unless that would be coarser than 0.5 mm, in which case they're float. Below about 30 m across, a mesh is quantized; long road meshes are float. The viewer dequantizes in the vertex shader, from the box.
- Normals are only stored for curved shapes, where the faces around a vertex aren't all in one plane. Flat shapes are shaded flat in the fragment shader, from screen-space derivatives, which most of a BIM model is.
- Parts are what a colour map or several styled items make: each part is a run of triangles in one material.
- Edges are the feature edges, where faces meet at more than 30 degrees, and boundary edges, for the outline a BIM viewer draws.

Against IFC text, a triangle mesh is about 6 bytes a vertex and 6 a triangle before zstd, against about 25 and 15.

## Building one

```sh
ifcfilter cache MODEL.ifc [--out MODEL.ifcc]
```

1. Read the model, as IFCFilter does now.
2. Tessellate each element's body:
   - Triangulated and polygonal face sets, and faceted breps, as they are: polygons are cut into triangles as compact writing does.
   - Mapped items become instances, with the mapping's transform composed with the element's placement. Shapes that are the same moved and turned are found as instancing does now, so models not run through `split` instance too.
   - Anything else (extrusions, sweeps) needs a geometry engine. See open questions. Until then it's an instance of one shared unit box, scaled to the item's extent.
   - A boolean is drawn as its first operand, without what's taken away.
3. Assign elements to tiles, then write meshes, tiles, elements and properties.
4. Write `meta` last, so a cache cut short doesn't look finished.

`split` could write a cache for each output alongside it, with `--cache`.

## Federation

A federation is a small JSON file listing caches:

```json
{ "name": "Zone A", "caches": [ { "path": "FS/MODEL.federate.ifcc", "visible": true }, { "path": "HY/MODEL.federate.ifcc", "visible": true } ] }
```

The backend opens each cache and gives it a range of picking IDs: a cache's element `n` is `base + n`, with `base` the sum of the element counts before it. Picking IDs are 32-bit, so a federation holds up to about 4 billion elements. The viewer renders IDs to an `R32UI` target and reads the pixel under the mouse.

## Backend API

What the backend, in Tauri or as a server, gives the frontend. Geometry is binary, through Tauri's custom protocol or HTTP; everything else is JSON.

| call | returns |
|---|---|
| `open(federation)` | each cache's `meta` and ID base |
| `tiles(cache)` | the tile index, binary |
| `tile(cache, id)` | the tile's instance blob, decompressed |
| `meshes(cache, ids)` | those meshes' blobs, decompressed, one after another |
| `children(cache, element)` | the tree, a level at a time |
| `element(cache, id)` | its attributes and property sets |
| `query(filters)` | matching picking IDs, as a binary list |
| `search(text)` | matching elements, by name, tag and value |

The frontend loads the tiles in view, largest `size` on screen first, and asks for the meshes it doesn't have yet. It keeps meshes it's seen for as long as memory allows, since tiles share them. Draw calls are one `BatchedMesh` or `InstancedMesh` per mesh per tile; far-away tiles can be drawn as their elements' boxes until their geometry arrives.

## Open questions

1. **Geometry engine for what isn't a mesh.** web-ifc's C++ core is light and fast; IfcOpenShell's iterator, with OpenCascade, is more exact and heavier. IFCFilter's outputs are mostly meshes already, so the cache builder could start with meshes only and draw the rest as boxes.
2. **Tile thresholds**: 2,000 instances and 200,000 triangles are guesses, to be tuned on the large models (CC, LA).
3. **Quantization**: whether 0.5 mm is the right point to switch to float, given IFCFilter rounds to 0.1 mm.
4. **The hidden and envelope outputs**: separate caches the viewer layers, or one cache from all three, with the `flags` saying which.
5. **Simplified meshes**: grid snapping is quick and takes any triangles, but quadric decimation keeps shapes better for the triangles; worth comparing on the large models.
6. **Manifest links**: whether the cache keeps each element's original GUIDs from the manifest, so the viewer can jump from a merged asset to the original model's elements.

## Prototype

`ifcfilter cache MODEL.ifc [--out MODEL.ifcc]` writes all of the above but what's marked reserved, and the viewer and backend don't exist yet. On this laptop:

| Model | IFC | Cache | Time | Meshes | Instances | Triangles | Boxes |
|---|---|---|---|---|---|---|---|
| FS, the federate output | 8.0 MB | 3.7 MB | 1.1 s | 573 | 2,693 | 453,708 | 374 extrusions |
| LT, the raw model, faceted breps | 28.1 MB | 1.4 MB | 1.3 s | 874 | 4,470 | 188,800 | none |

Geometry is about 1 MB of each: meshes with zstd 1.0 MB, instances 0.04 MB. In FS the rest is properties: Revit gives most elements sets of their own, so there are 4,797 sets and 40,107 values on 718 elements.
