"""except DISC RULE: the rule is for every discipline but those, by the folder a model is in."""
import csv,pathlib,subprocess,sys,tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
END='ENDSEC;\nEND-ISO-10303-21;'
with tempfile.TemporaryDirectory() as temp:
 root=pathlib.Path(temp)
 extra="""#101=IFCPROPERTYSINGLEVALUE('Uniclass_PrCode',$,IFCTEXT('Pr_20_93_71_18'),$);
#102=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xr1',$,'DIT_Asset',$,(#101));
#103=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xr2',$,$,$,(#15),#102);
"""
 text=fixture.read_text().replace(END,extra+END)
 models=[]
 for discipline in ('CC','XY'):
  (root/discipline).mkdir(); model=root/discipline/f'P-X-ZZZ-{discipline}-M3D-000001.ifc'
  # Each its own GUIDs, so neither is renamed as shared with the other
  model.write_text(text.replace('0Jm4wmcl10lPX6wYa53X','0Jm4wmcl10lPX6wYa5'+discipline+'X')); models.append(model)
 rule=root/'rules.txt';rule.write_text('except cc,AB skip-if DIT_Asset.Uniclass_PrCode=Pr_20_93_71_18\n')
 workbook=Workbook();workbook.active.append(('IFC_Filter','SiteID'));workbook.save(root/'site-id-attributes.xlsx')
 (root/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
 out=root/'out'
 subprocess.run([str(exe),'split',*map(str,models),'--out',str(out),'--data-dir',str(root),'--rules',str(rule),'--no-zip','--no-merge','--no-instance','--no-compact'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 def walls(discipline):
  with (out/'manifest'/f'P-X-ZZZ-{discipline}-M3D-000001.manifest.csv').open(newline='') as stream:
   return [r['output'] for r in csv.DictReader(stream) if r['class']=='IfcWall']
 assert walls('CC')==['federate'],walls('CC')
 assert walls('XY')==[],walls('XY')
 # A mistake in an except rule stops the run, as any other would
 rule.write_text('except CC skip-when X.Y=Z\n')
 run=subprocess.run([str(exe),'split',str(models[1]),'--out',str(root/'bad'),'--data-dir',str(root),'--rules',str(rule),'--no-zip'],capture_output=True,text=True)
 assert run.returncode!=0 and "except can't be used with skip-when" in run.stdout+run.stderr,run.stdout+run.stderr
print('except DISC RULE leaves those disciplines out of the rule, and only those')
