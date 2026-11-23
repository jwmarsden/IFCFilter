"""A part drawn in a surface style an envelope-style rule names goes to envelopes, though it's under the transparency threshold."""
import csv,pathlib,subprocess,sys,tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=pathlib.Path(__file__).parent/'fixtures'/'opening.ifc'
END='ENDSEC;\nEND-ISO-10303-21;'
with tempfile.TemporaryDirectory() as temp:
 root=pathlib.Path(temp)
 # The wall gets a second, half see-through solid in front of it, in a style named Clearance Zones
 extra="""#30=IFCCARTESIANPOINT((0.,3.,0.));
#31=IFCAXIS2PLACEMENT3D(#30,$,$);
#32=IFCEXTRUDEDAREASOLID(#11,#31,#5,1.);
#33=IFCCOLOURRGB($,0.5,0.5,0.5);
#34=IFCSURFACESTYLERENDERING(#33,0.5,$,$,$,$,$,$,.NOTDEFINED.);
#35=IFCSURFACESTYLE('Clearance Zones',.BOTH.,(#34));
#36=IFCSTYLEDITEM(#32,(#35),$);
"""
 text=fixture.read_text().replace("#13=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#12));","#13=IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',(#12,#32));")
 model=root/'model.ifc';model.write_text(text.replace(END,extra+END))
 workbook=Workbook();workbook.active.append(('IFC_Filter','SiteID'));workbook.save(root/'site-id-attributes.xlsx')
 (root/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
 def outputs(rules):
  rule=root/'rules.txt';rule.write_text(rules)
  out=root/('out'+str(len(rules)))
  subprocess.run([str(exe),'split',str(model),'--out',str(out),'--data-dir',str(root),'--rules',str(rule),'--no-zip','--no-merge','--no-instance','--no-compact'],check=True,stdout=subprocess.DEVNULL)
  with next((out/'manifest').glob('*.csv')).open(newline='') as stream:rows=list(csv.DictReader(stream))
  return sorted(r['output'] for r in rows if r['guid']=='0Jm4wmcl10lPX6wYa53Xrf')
 # Half see-through is under the 0.7 threshold, so without the rule all of it is in the federation file
 assert outputs('# none\n')==['federate'],outputs('# none\n')
 # The rule's case doesn't matter, and the wall itself stays
 assert outputs('envelope-style clearance zone\n')==['envelopes','federate'],outputs('envelope-style clearance zone\n')
 # A physical family rule keeps it, as the family is really there
 assert outputs('envelope-style clearance zone\nphysical Wall\n')==['federate'],outputs('envelope-style clearance zone\nphysical Wall\n')
print('A part in a surface style an envelope-style rule names goes to envelopes')
