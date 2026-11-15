"""Exercise the workbook-defined property schema in both IFC and viewer cache."""
import pathlib, sqlite3, subprocess, sys, tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve(); fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
with tempfile.TemporaryDirectory() as directory:
 root=pathlib.Path(directory)
 extra="""#101=IFCPROPERTYSINGLEVALUE('SiteID',$,IFCLABEL('PU000123'),$);
#102=IFCPROPERTYSINGLEVALUE('Uniclass_EfCode',$,IFCLABEL('EF_15'),$);
#103=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xri',$,'Asset',$,(#101,#102));
#104=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrj',$,$,$,(#15),#103);
#105=IFCPROPERTYSINGLEVALUE('Important Value',$,IFCLABEL('Source value'),$);
#106=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrk',$,'Custom Set',$,(#105));
#107=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrl',$,$,$,(#15),#106);
#108=IFCPROPERTYSINGLEVALUE('Custom Field',$,IFCLABEL('Extra value'),$);
#109=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrm',$,'IFCFilter',$,(#108));
#110=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrn',$,$,$,(#15),#109);
#111=IFCMATERIAL('Concrete',$,$);
#112=IFCRELASSOCIATESMATERIAL('0Jm4wmcl10lPX6wYa53Xro',$,$,$,(#15),#111);
"""
 model=root/'input.ifc';model.write_text(fixture.read_text().replace('ENDSEC;\nEND-ISO-10303-21;',extra+'ENDSEC;\nEND-ISO-10303-21;'))
 rules=root/'rules.txt';rules.write_text('site-set Asset\nsite-id SiteID\n')
 workbook=Workbook();sheet=workbook.active
 for row in [('Element','GlobalId'),('IFC_Filter','SiteID'),('IFC_Filter','Custom_Field'),('Classification','EF_Code'),('Classification','EF_Description'),('Custom_Set','Important_Value')]:sheet.append(row)
 schema=root/'schema.xlsx';workbook.save(schema)
 reference = root / 'location-referencing.csv'
 reference.write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example location\n')
 output=root/'out';cmd=[str(exe),'split',str(model),'--out',str(output),'--rules',str(rules),'--property-schema',str(schema),'--no-zip','--no-merge','--no-instance','--no-compact','--simplify-mm','0']
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
 model_out=next((output/'federate').rglob('*.ifc'));cache=root/'test.ifcc'
 subprocess.run([str(exe),'cache',str(model_out),'--out',str(cache)],check=True,stdout=subprocess.DEVNULL)
 db=sqlite3.connect(cache)
 rows=db.execute('''SELECT sn.text,pn.text,vt.text FROM elements e JOIN element_psets ep ON ep.element=e.id JOIN psets p ON p.id=ep.pset JOIN strings sn ON sn.id=p.name JOIN pset_values pv ON pv.pset=p.id JOIN strings pn ON pn.id=pv.name JOIN "values" v ON v.id=pv.value LEFT JOIN strings vt ON vt.id=v.text WHERE e.guid=?''',('0Jm4wmcl10lPX6wYa53Xrf',)).fetchall();db.close()
 actual={(s,n):v for s,n,v in rows}
 expected={('IFC_Filter','SiteID'):'PU000123',('IFC_Filter','Custom_Field'):'Extra value',('Classification','EF_Code'):'EF_15',('Classification','EF_Description'):'Earthworks and remediation',('Custom_Set','Important_Value'):'Source value'}
 assert actual==expected,(actual,expected)
 assert "'Asset'" not in model_out.read_text(), 'unlisted property set should be removed from IFC'
 first=model_out.read_bytes()
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
 assert model_out.read_bytes()==first, 'schema pass must be idempotent'
 sheet.append(('Custom_Set','New_Field'))
 workbook.save(schema)
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
 subprocess.run([str(exe),'cache',str(model_out),'--out',str(cache)],check=True,stdout=subprocess.DEVNULL)
 db=sqlite3.connect(cache)
 names=db.execute('''SELECT pn.text FROM elements e JOIN element_psets ep ON ep.element=e.id JOIN psets p ON p.id=ep.pset JOIN strings sn ON sn.id=p.name JOIN pset_values pv ON pv.pset=p.id JOIN strings pn ON pn.id=pv.name WHERE e.guid=? AND sn.text=?''',('0Jm4wmcl10lPX6wYa53Xrf','Custom_Set')).fetchall();db.close()
 assert {n for (n,) in names}=={'Important_Value','New_Field'},names
print('Workbook schema controls properties and preserves source values')
