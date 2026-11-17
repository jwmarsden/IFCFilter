"""Exercise per-model approved element corrections before merging and tracing."""
import csv,pathlib,subprocess,sys,tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp); data=root/'data'; data.mkdir(); inp=root/'BR';inp.mkdir()
 extra="""#30=IFCWALL('0Jm4wmcl10lPX6wYa53Xrp',$,'Other',$,$,#4,#14,$,$);
#31=IFCWALL('0Jm4wmcl10lPX6wYa53Xrq',$,'Pending',$,$,#4,#14,$,$);
#40=IFCPROPERTYSINGLEVALUE('Asset_SiteID',$,IFCLABEL('N/A'),$);
#41=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrr',$,'AssetData',$,(#40));
#42=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrs',$,$,$,(#15,#30,#31),#41);
"""
 model=inp/'test.ifc'
 model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;',extra+'ENDSEC;\nEND-ISO-10303-21;'))
 rules=root/'rules.txt';rules.write_text('site-set AssetData\nsite-id Asset_SiteID\nproject-set Project\n')
 schema=Workbook();s=schema.active
 for name in ('SiteID','Asset_Type','Identifier'):s.append(['IFC_Filter',name])
 schema.save(data/'site-id-attributes.xlsx')
 (data/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
 lookup=root/'lookup.csv'
 lookup.write_text('SiteID,DesignTag,Asset Type,Location,Zone,Co,En,SL,EF,Ss,Pr,Identifier\nBRG272012,,,,,,,,,,,Corrected asset\n')
 folder=data/'site-id-corrections'/'BR';folder.mkdir(parents=True)
 book=Workbook();sheet=book.active
 sheet.append(['Asset Type','Site ID','Review status','Source GlobalId','Source model'])
 sheet.append(['BRG','BRG272012','Approved','0Jm4wmcl10lPX6wYa53Xrf','test.ifc'])
 sheet.append(['BRG','BRG272012','Approved','0Jm4wmcl10lPX6wYa53Xrp','test.ifc'])
 sheet.append(['BAD','BAD000000','Needs review','0Jm4wmcl10lPX6wYa53Xrq','test.ifc'])
 sheet.append(['BRG','BRG272019','Approved','0Jm4wmcl10lPX6wYa53Xzz','test.ifc'])
 workbook=folder/'test.xlsx';book.save(workbook)
 out=root/'out'
 cmd=[str(exe),'split',str(model),'--out',str(out),'--rules',str(rules),'--data-dir',str(data),'--site-lookup',str(lookup),'--no-zip','--no-instance','--no-compact','--simplify-mm','0','--merge-gap','0']
 def run():return subprocess.run(cmd,text=True,capture_output=True,check=True)
 first=run()
 assert '2 approved element corrections applied' in first.stdout,first.stdout
 assert 'is not in test.ifc' in first.stderr,first.stderr
 path=next((out/'federate').rglob('*.ifc'))
 result=path.read_text()
 assert "IFCPROPERTYSINGLEVALUE('SiteID',$,IFCLABEL('BRG272012')" in result
 assert "IFCPROPERTYSINGLEVALUE('Asset_Type',$,IFCLABEL('BRG')" in result
 assert "IFCPROPERTYSINGLEVALUE('Identifier',$,IFCLABEL('Corrected asset')" in result
 manifest=path.with_suffix('.manifest.csv')
 with manifest.open(newline='') as f: links={r['source_guid']:r['output_guid'] for r in csv.DictReader(f)}
 assert links['0Jm4wmcl10lPX6wYa53Xrf']==links['0Jm4wmcl10lPX6wYa53Xrp'],links
 assert links['0Jm4wmcl10lPX6wYa53Xrq']!=links['0Jm4wmcl10lPX6wYa53Xrf']
 assert '0 of 1 models to do' in run().stdout
 sheet.cell(2,2,'BRG272013');sheet.cell(3,2,'BRG272013');book.save(workbook)
 assert '1 of 1 models to do' in run().stdout
 assert "IFCPROPERTYSINGLEVALUE('SiteID',$,IFCLABEL('BRG272013')" in path.read_text()
 # A type-only correction changes the property without inventing or grouping by a SiteID.
 sheet.cell(4,1,'LJB');sheet.cell(4,2).value=None;sheet.cell(4,3,'Approved');book.save(workbook)
 run()
 assert "IFCPROPERTYSINGLEVALUE('Asset_Type',$,IFCLABEL('LJB')" in path.read_text()
 sheet.cell(2,1,'WRONG');book.save(workbook)
 failed=subprocess.run(cmd,text=True,capture_output=True)
 assert failed.returncode!=0 and 'Asset Type does not match Site ID' in failed.stderr,failed.stderr
print('Approved per-model corrections, merging, lookup, review gating and incremental rebuild passed')

