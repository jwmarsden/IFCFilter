"""Check that opening relationships and Boolean differences cut a 4 x 4 x 1 m solid.

Run with a geometry-enabled IFCFilter: python tests/cache_cuts.py path/to/ifcfilter
"""

import ctypes
import ctypes.util
import contextlib
import pathlib
import sqlite3
import struct
import subprocess
import sys
import tempfile


def volume(blob, zstd):
    source = ctypes.create_string_buffer(blob)
    size = zstd.ZSTD_getFrameContentSize(source, len(blob))
    if size >= 2**64 - 2:
        raise ValueError("mesh has no zstd frame size")
    target = ctypes.create_string_buffer(size)
    if zstd.ZSTD_decompress(target, size, source, len(blob)) != size:
        raise ValueError("mesh decompression failed")
    data = target.raw
    magic, version, flags, vertex_count, index_count, parts, _ = struct.unpack_from("<4sHHIIHH", data)
    if magic != b"IFCM" or version != 1 or flags & 4:
        raise ValueError("unexpected mesh format")
    low = struct.unpack_from("<3f", data, 20)
    high = struct.unpack_from("<3f", data, 32)
    offset = 44 + parts * 12
    points = []
    for _ in range(vertex_count):
        if flags & 2:
            point = struct.unpack_from("<3f", data, offset)
            offset += 12
        else:
            q = struct.unpack_from("<3H", data, offset)
            offset += 6
            point = tuple(low[k] + q[k] * (high[k] - low[k]) / 65535 for k in range(3))
        points.append(point)
    index_format = "<I" if flags & 1 else "<H"
    index_size = 4 if flags & 1 else 2
    indices = [struct.unpack_from(index_format, data, offset + i * index_size)[0] for i in range(index_count)]
    result = 0.0
    for i in range(0, len(indices), 3):
        a, b, c = (points[j] for j in indices[i : i + 3])
        result += (a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6
    return abs(result)


def main():
    exe = pathlib.Path(sys.argv[1]).resolve()
    local_dll = exe.parent / "zstd.dll"
    library = str(local_dll) if local_dll.exists() else ctypes.util.find_library("zstd")
    if not library:
        raise RuntimeError("zstd shared library not found")
    zstd = ctypes.CDLL(library)
    zstd.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    zstd.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
    zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    zstd.ZSTD_decompress.restype = ctypes.c_size_t
    with tempfile.TemporaryDirectory() as temp:
        for name in ("opening", "boolean"):
            cache = pathlib.Path(temp) / (name + ".ifcc")
            fixture = pathlib.Path(__file__).parent / "fixtures" / (name + ".ifc")
            subprocess.run([str(exe), "cache", str(fixture), "--out", str(cache)], check=True)
            with contextlib.closing(sqlite3.connect(cache)) as db:
                blobs = db.execute("SELECT blob FROM meshes").fetchall()
                flags = db.execute("SELECT flags FROM elements WHERE guid = ?", ("0Jm4wmcl10lPX6wYa53Xrf",)).fetchone()[0]
            assert len(blobs) == 1 and not flags & 16, (name, len(blobs), flags)
            measured = volume(blobs[0][0], zstd)
            assert abs(measured - 15.0) < 0.002, (name, measured)
            print(f"{name}: {measured:.6f} m3")

        # More than the default limit must keep the original mesh and report it approximate.
        fixture = pathlib.Path(__file__).parent / "fixtures" / "opening.ifc"
        text = fixture.read_text(encoding="ascii")
        original = "#24=IFCRELVOIDSELEMENT('0Jm4wmcl10lPX6wYa53Xrh',$,$,$,#15,#23);"
        relations = "\n".join(f"#{24 + i}=IFCRELVOIDSELEMENT('{i:022d}',$,$,$,#15,#23);" for i in range(33))
        assert original in text
        complex_fixture = pathlib.Path(temp) / "complex.ifc"
        complex_fixture.write_text(text.replace(original, relations), encoding="ascii")
        cache = pathlib.Path(temp) / "complex.ifcc"
        subprocess.run([str(exe), "cache", str(complex_fixture), "--out", str(cache)], check=True)
        with contextlib.closing(sqlite3.connect(cache)) as db:
            blob = db.execute("SELECT blob FROM meshes").fetchone()[0]
            flags = db.execute("SELECT flags FROM elements WHERE guid = ?", ("0Jm4wmcl10lPX6wYa53Xrf",)).fetchone()[0]
        measured = volume(blob, zstd)
        assert flags & 16 and abs(measured - 16.0) < 0.002, (flags, measured)
        print(f"complex fallback: {measured:.6f} m3, marked approximate")

        # A moderate number of openings can still be expensive on a merged body with many items.
        relations = "\n".join(f"#{24 + i}=IFCRELVOIDSELEMENT('{i:022d}',$,$,$,#15,#23);" for i in range(17))
        many_items = "#13=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(" + ",".join(["#12"] * 16) + "));"
        work_fixture = pathlib.Path(temp) / "complex-work.ifc"
        work_fixture.write_text(text.replace(original, relations).replace("#13=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#12));", many_items), encoding="ascii")
        cache = pathlib.Path(temp) / "complex-work.ifcc"
        subprocess.run([str(exe), "cache", str(work_fixture), "--out", str(cache)], check=True)
        with contextlib.closing(sqlite3.connect(cache)) as db:
            flags = db.execute("SELECT flags FROM elements WHERE guid = ?", ("0Jm4wmcl10lPX6wYa53Xrf",)).fetchone()[0]
        assert flags & 16, flags
        print("complex merged-body fallback: marked approximate")

        # A single tessellated opening can also be expensive even with only one relation.
        opening_set = "#17=IFCPOLYGONALFACESET(#26,.T.,(" + ",".join(["#27"] * 513) + "),$);"
        opening_defs = "\n#26=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)),$);\n#27=IFCINDEXEDPOLYGONALFACE((1,2,3));"
        mesh_fixture = pathlib.Path(temp) / "complex-opening.ifc"
        mesh_fixture.write_text(text.replace("#17=IFCEXTRUDEDAREASOLID(#16,#3,#5,1.2);", opening_set).replace(original, original + opening_defs), encoding="ascii")
        cache = pathlib.Path(temp) / "complex-opening.ifcc"
        subprocess.run([str(exe), "cache", str(mesh_fixture), "--out", str(cache)], check=True)
        with contextlib.closing(sqlite3.connect(cache)) as db:
            flags = db.execute("SELECT flags FROM elements WHERE guid = ?", ("0Jm4wmcl10lPX6wYa53Xrf",)).fetchone()[0]
        assert flags & 16, flags
        print("complex opening-mesh fallback: marked approximate")


if __name__ == "__main__":
    main()
