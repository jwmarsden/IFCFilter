"""Lane placeholders survive filtering and reach the cache without extra IFC property sets."""
import pathlib, sqlite3, subprocess, sys, tempfile
from openpyxl import Workbook

exe = pathlib.Path(sys.argv[1]).resolve()
fixture = pathlib.Path(__file__).parent / 'fixtures' / 'opening.ifc'
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    extra = """#101=IFCEXTRUDEDAREASOLID(#16,#3,#5,0.5);
#102=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#101));
#103=IFCPRODUCTDEFINITIONSHAPE($,$,(#102));
#104=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr1',$,'Mesh',$,'Mesh',#4,#103,$,$);
#105=IFCPROPERTYSINGLEVALUE('Feature_Definition',$,IFCLABEL('Lane-T1L'),$);
#106=IFCPROPERTYSINGLEVALUE('Feature_Name',$,IFCLABEL('LANE-T1L'),$);
#107=IFCPROPERTYSINGLEVALUE('Alignment',$,IFCLABEL('MC02'),$);
#108=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr2',$,'Corridor Data',$,(#105,#106,#107));
#109=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr3',$,$,$,(#104),#108);
#110=IFCPROPERTYSINGLEVALUE('Asset_Identifier_Label',$,IFCLABEL('PLACEHOLDER'),$);
#111=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr4',$,'Corridor_Mesh_ID',$,(#110));
#112=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr5',$,$,$,(#104),#111);
"""
    (root/'RW').mkdir()
    model = root/'RW'/'input.ifc'
    model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;',extra+'ENDSEC;\nEND-ISO-10303-21;'))
    rules = root/'rules.txt'; rules.write_text('hidden-if Corridor_Mesh_ID.Asset_Identifier_Label=PLACEHOLDER\n')
    book=Workbook(); book.active.append(('Element','GlobalId'));book.active.append(('IFC_Filter','SiteID'));book.save(root/'site-id-attributes.xlsx')
    (root/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
    out=root/'out'
    subprocess.run([str(exe),'split',str(model),'--out',str(out),'--rules',str(rules),'--data-dir',str(root),'--no-zip','--no-instance','--no-compact'],check=True,stdout=subprocess.DEVNULL)
    filtered=next((out/'federate').rglob('*.ifc'))
    lane_file=pathlib.Path(str(filtered)+'.lanes.tsv')
    assert lane_file.read_text().strip() == '0Jm4wmcl10lPX6wYa53Xr1\tLANE-T1L\tMC02'
    assert "'Corridor Data'" not in filtered.read_text(), 'source property set should not bypass the schema'
    cache=root/'result.ifcc'
    subprocess.run([str(exe),'cache',str(filtered),'--out',str(cache)],check=True,stdout=subprocess.DEVNULL)
    db=sqlite3.connect(cache)
    rows=db.execute('SELECT e.guid, l.name, l.alignment FROM drive_lanes l JOIN elements e ON e.id=l.element').fetchall()
    db.close()
    assert rows==[('0Jm4wmcl10lPX6wYa53Xr1','LANE-T1L','MC02')],rows
print('Lane metadata survives filtering and cache generation without adding IFC property sets')
