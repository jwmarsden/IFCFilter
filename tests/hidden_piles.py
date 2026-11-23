"""The exact Uniclass pile product goes to hidden, regardless of family name."""
import csv,pathlib,subprocess,sys,tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
with tempfile.TemporaryDirectory() as temp:
 root=pathlib.Path(temp)
 extra="""#101=IFCPROPERTYSINGLEVALUE('Uniclass_PrCode',$,IFCTEXT('Pr_20_85_62_18'),$);
#102=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr1',$,'DIT_Asset',$,(#101));
#103=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr2',$,$,$,(#15),#102);
"""
 model=root/'model.ifc';model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;',extra+'ENDSEC;\nEND-ISO-10303-21;'))
 rule=root/'rules.txt';rule.write_text('hidden-if DIT_Asset.Uniclass_PrCode=Pr_20_85_62_18\n')
 workbook=Workbook();workbook.active.append(('IFC_Filter','SiteID'));workbook.save(root/'site-id-attributes.xlsx')
 (root/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
 out=root/'out'
 subprocess.run([str(exe),'split',str(model),'--out',str(out),'--data-dir',str(root),'--rules',str(rule),'--no-zip','--no-merge','--no-instance','--no-compact'],check=True,stdout=subprocess.DEVNULL)
 with next((out/'manifest').glob('*.csv')).open(newline='') as stream:rows=list(csv.DictReader(stream))
 result=[r for r in rows if r['guid']=='0Jm4wmcl10lPX6wYa53Xrf']
 assert len(result)==1 and result[0]['output']=='hidden' and result[0]['reason']=='hidden rule',result
print('The Uniclass pile product is placed in the hidden output')
