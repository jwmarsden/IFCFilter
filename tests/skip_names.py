"""Exact-name exclusions remove construction meshes, preserving shared geometry and other elements."""
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
    extra = """#101=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr1',$,'OBSERVER',$,'3D Tri-mesh',#4,#14,$,$);
#102=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr2',$,'edge',$,'3D Tri-mesh',#4,#14,$,$);
#103=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr3',$,'Edge beam',$,'3D Tri-mesh',#4,#14,$,$);
#104=IFCBUILDINGELEMENTPROXY('0Jm4wmcl10lPX6wYa53Xr4',$,'Diaphragm wall',$,'3D Tri-mesh',#4,#14,$,$);
#105=IFCPROPERTYSINGLEVALUE('Uniclass_PrCode',$,IFCTEXT('Pr_20_93_71_18'),$);
#106=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr5',$,'DIT_Asset',$,(#105));
#107=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr6',$,$,$,(#104),#106);
"""
    model = root / 'input.ifc'
    model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;', extra + 'ENDSEC;\nEND-ISO-10303-21;'))
    rules = root / 'rules.txt'; rules.write_text('skip-name OBSERVER\nskip-name Edge\nskip-if DIT_Asset.Uniclass_PrCode=Pr_20_93_71_18\n')
    workbook = Workbook(); workbook.active.append(('IFC_Filter', 'SiteID')); workbook.save(root / 'site-id-attributes.xlsx')
    (root / 'location-referencing.csv').write_text('DIT_LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
    output = root / 'out'
    subprocess.run([str(exe), 'split', str(model), '--out', str(output), '--rules', str(rules), '--data-dir', str(root), '--no-zip', '--no-merge', '--no-instance', '--no-compact'], check=True, stdout=subprocess.DEVNULL)
    output_files = list(output.rglob('*.ifc'))
    contents = '\n'.join(p.read_text() for p in output_files)
    assert '0Jm4wmcl10lPX6wYa53Xr1' not in contents
    assert '0Jm4wmcl10lPX6wYa53Xr2' not in contents
    assert '0Jm4wmcl10lPX6wYa53Xr4' not in contents
    assert '0Jm4wmcl10lPX6wYa53Xr3' in contents
    with next((output / 'manifest').glob('*.csv')).open(newline='') as f: rows=list(csv.DictReader(f))
    assert any(r['guid']=='0Jm4wmcl10lPX6wYa53Xr3' and r['output']=='federate' and int(r['items']) > 0 for r in rows)
print('Exact names skipped while similar names and shared geometry remain')
