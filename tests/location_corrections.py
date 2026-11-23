"""Location corrections: the last approved row for an element sets its IFCFilter Location, ahead of the model's."""
import pathlib,subprocess,sys,tempfile
import ifcopenshell, ifcopenshell.util.element
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
HEADER='GlobalId,Location,Description,Model,Set by,Set at,Status\n'
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp); data=root/'data'; data.mkdir(); inp=root/'BR';inp.mkdir()
 extra="""#30=IFCWALL('0Jm4wmcl10lPX6wYa53Xrp',$,'Moved',$,$,#4,#14,$,$);
#31=IFCWALL('0Jm4wmcl10lPX6wYa53Xrq',$,'Taken back',$,$,#4,#14,$,$);
#32=IFCWALL('0Jm4wmcl10lPX6wYa53Xrr',$,'Proposed only',$,$,#4,#14,$,$);
#40=IFCPROPERTYSINGLEVALUE('LocationCode',$,IFCLABEL('OLD'),$);
#41=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrs',$,'Data',$,(#40));
#42=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrt',$,$,$,(#30,#31,#32),#41);
"""
 model=inp/'test.ifc'
 model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;',extra+'ENDSEC;\nEND-ISO-10303-21;'))
 rules=root/'rules.txt';rules.write_text('location-property LocationCode\n')
 schema=Workbook();s=schema.active
 for name in ('SiteID','Location','Location_Description'):s.append(['IFC_Filter',name])
 schema.save(data/'site-id-attributes.xlsx')
 reference=data/'location-referencing.csv'
 reference.write_text('XYZ_LocationHierarchyCode,LocationHierarchyDescription\nOLD,Old place\nA-B001,Compound;Building\nA-B001-S101,Compound;Building;Room\n')
 folder=data/'location-corrections'/'BR';folder.mkdir(parents=True)
 corrections=folder/'test.csv'
 corrections.write_text(HEADER+
  '0Jm4wmcl10lPX6wYa53Xrp,A-B001,,test.ifc,someone,2026-11-17T10:00:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrq,A-B001,,test.ifc,someone,2026-11-17T10:00:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrp,A-B001-S101,"Compound;Building;Room",test.ifc,someone,2026-11-17T10:05:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrq,,,test.ifc,someone,2026-11-17T10:06:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrr,A-B001,,test.ifc,someone,2026-11-17T10:07:00,Proposed\n'
  '0Jm4wmcl10lPX6wYa53Xrp,A-B001,,other.ifc,someone,2026-11-17T10:08:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xzz,A-B001,,test.ifc,someone,2026-11-17T10:09:00,Approved\n')
 out=root/'out'
 cmd=[str(exe),'split',str(model),'--out',str(out),'--rules',str(rules),'--data-dir',str(data),'--property-schema',str(data/'site-id-attributes.xlsx'),'--location-reference',str(reference),'--no-zip','--no-instance','--no-compact','--simplify-mm','0','--merge-gap','0']
 def run():return subprocess.run(cmd,text=True,capture_output=True,check=True)
 first=run()
 assert '1 location corrections applied' in first.stdout,first.stdout
 assert '1 rows for other models left out' in first.stdout,first.stdout
 assert 'location correction GlobalId 0Jm4wmcl10lPX6wYa53Xzz is not in test.ifc' in first.stderr,first.stderr
 path=next((out/'federate').rglob('*.ifc'))
 def locations():
  f=ifcopenshell.open(str(path))
  return {e.GlobalId:ifcopenshell.util.element.get_psets(e).get('IFC_Filter',{}) for e in f.by_type('IfcWall')}
 got=locations()
 assert got['0Jm4wmcl10lPX6wYa53Xrp']['Location']=='A-B001-S101',got
 assert got['0Jm4wmcl10lPX6wYa53Xrp']['Location_Description']=='Compound;Building;Room',got
 assert got['0Jm4wmcl10lPX6wYa53Xrq']['Location']=='OLD',got  # its correction was taken away
 assert got['0Jm4wmcl10lPX6wYa53Xrr']['Location']=='OLD',got  # not approved
 assert '0 of 1 models to do' in run().stdout
 # A new row is a change to the model's corrections, so it's done again
 with corrections.open('a') as f: f.write('0Jm4wmcl10lPX6wYa53Xrr,A-B001,,test.ifc,someone,2026-11-17T11:00:00,Approved\n')
 assert '1 of 1 models to do' in run().stdout
 assert locations()['0Jm4wmcl10lPX6wYa53Xrr']['Location']=='A-B001'
 # Site IDs from the viewer's file beside the workbook: the last approved row wins, and an empty one takes it away
 sites=data/'site-id-corrections'/'BR';sites.mkdir(parents=True)
 (sites/'test.csv').write_text('GlobalId,Site ID,Model,Set by,Set at,Status\n'
  '0Jm4wmcl10lPX6wYa53Xrq,ABC120107,test.ifc,someone,2026-11-18T10:00:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrr,ABC120107,test.ifc,someone,2026-11-18T10:00:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrr,,test.ifc,someone,2026-11-18T10:01:00,Approved\n'
  '0Jm4wmcl10lPX6wYa53Xrp,NOTASITE,test.ifc,someone,2026-11-18T10:02:00,Approved\n')
 third=run()
 assert '1 of 1 models to do' in third.stdout,third.stdout
 assert '1 site ID corrections applied' in third.stdout,third.stdout
 assert 'invalid Site ID NOTASITE' in third.stderr,third.stderr
 got=locations()
 assert got['0Jm4wmcl10lPX6wYa53Xrq'].get('SiteID')=='ABC120107',got
 assert not got['0Jm4wmcl10lPX6wYa53Xrr'].get('SiteID'),got
print('Location corrections: last approved row wins, removal, review gating, other models and incremental rebuild, and site ID rows passed')
