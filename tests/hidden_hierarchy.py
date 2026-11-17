"""Property rules hide bracketed sets and every part of a selected assembly."""
import csv
import pathlib
import subprocess
import sys
import tempfile
from openpyxl import Workbook

exe = pathlib.Path(sys.argv[1]).resolve()
fixture = pathlib.Path(__file__).parent / 'fixtures' / 'opening.ifc'
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    extra = """#101=IFCPROPERTYSINGLEVALUE('User Label',$,IFCTEXT('Rootball'),$);
#102=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr1',$,'[Generic].[Graphic3d]',$,(#101));
#103=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr2',$,$,$,(#15),#102);
#104=IFCELEMENTASSEMBLY('0Jm4wmcl10lPX6wYa53Xr3',$,'Footing',$,$,$,$,$,.FACTORY.,.USERDEFINED.);
#105=IFCPROPERTYSINGLEVALUE('Element_Description',$,IFCTEXT('Sign footing'),$);
#106=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr4',$,'Corridor Data',$,(#105));
#107=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr5',$,$,$,(#104),#106);
#108=IFCEXTRUDEDAREASOLID(#16,#3,#5,0.5);
#109=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#108));
#110=IFCPRODUCTDEFINITIONSHAPE($,$,(#109));
#111=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr6',$,'Footing part',$,$,#4,#110,$,$);
#112=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xr7',$,$,$,#104,(#111));
#113=IFCEXTRUDEDAREASOLID(#16,#3,#5,0.6);
#114=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#113));
#115=IFCPRODUCTDEFINITIONSHAPE($,$,(#114));
#116=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr8',$,'Sign',$,$,#4,#115,$,$);
"""
    model = root / 'input.ifc'
    model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;', extra + 'ENDSEC;\nEND-ISO-10303-21;'))
    rules = root / 'rules.txt'
    rules.write_text('hidden-if [Generic].[Graphic3d].User Label=Rootball\nhidden-tree-if Corridor Data.Element_Description=Sign footing\n')
    workbook = Workbook()
    workbook.active.append(('Element', 'GlobalId'))
    workbook.active.append(('IFC_Filter', 'SiteID'))
    workbook.save(root / 'site-id-attributes.xlsx')
    (root / 'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
    output = root / 'out'
    subprocess.run([str(exe), 'split', str(model), '--out', str(output), '--rules', str(rules), '--data-dir', str(root), '--no-zip', '--no-merge', '--no-instance', '--no-compact'], check=True, stdout=subprocess.DEVNULL)
    with next((output / 'manifest').glob('*.csv')).open(newline='') as f:
        rows = list(csv.DictReader(f))
    def outputs(guid): return {r['output'] for r in rows if r['guid'] == guid}
    assert outputs('0Jm4wmcl10lPX6wYa53Xrf') == {'hidden'}
    assert outputs('0Jm4wmcl10lPX6wYa53Xr6') == {'hidden'}
    assert outputs('0Jm4wmcl10lPX6wYa53Xr8') == {'federate'}
print('Root balls and footing descendants are hidden; unrelated geometry remains federated')
