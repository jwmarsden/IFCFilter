"""Joined across models: an element whose site a correction gives it goes into the other model's element of that site,
and its ports are nested on that element instead of being lost."""
import pathlib,subprocess,sys,tempfile
import ifcopenshell
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=(pathlib.Path(__file__).parent/'fixtures'/'opening.ifc').read_text()
END='ENDSEC;\nEND-ISO-10303-21;'
SITE='ABC120107'
# A site, building and level, each model's own, the wall and pipe on the level
STRUCTURE="""#50=IFCSITE('0Jm4wmcl10lPX6wYa53Xs1',$,'Site',$,$,#4,$,$,.ELEMENT.,$,$,$,$,$);
#51=IFCBUILDING('0Jm4wmcl10lPX6wYa53Xs2',$,'Building',$,$,#4,$,$,.ELEMENT.,$,$,$);
#52=IFCBUILDINGSTOREY('0Jm4wmcl10lPX6wYa53Xs3',$,'Level 1',$,$,#4,$,$,.ELEMENT.,0.);
#53=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xs4',$,$,$,#1,(#50));
#54=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xs5',$,$,$,#50,(#51));
#55=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xs6',$,$,$,#51,(#52));
"""
fixture=fixture.replace(END,STRUCTURE+END)
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp); data=root/'data'; data.mkdir()
 # The model whose element has the site of its own
 (root/'BR').mkdir(); keeper=root/'BR'/'P-X-ZZZ-BR-M3D-000001.ifc'
 keeper.write_text(fixture.replace(END,f"""#40=IFCPROPERTYSINGLEVALUE('SiteID',$,IFCLABEL('{SITE}'),$);
#41=IFCPROPERTYSET('1Jm4wmcl10lPX6wYa53Xra',$,'Asset',$,(#40));
#42=IFCRELDEFINESBYPROPERTIES('1Jm4wmcl10lPX6wYa53Xrb',$,$,$,(#15),#41);
#56=IFCRELCONTAINEDINSPATIALSTRUCTURE('0Jm4wmcl10lPX6wYa53Xs7',$,$,$,(#15),#52);
"""+END))
 # and the one whose pipe, with two ports, a correction puts in it
 (root/'DU').mkdir(); other=root/'DU'/'P-X-ZZZ-DU-M3D-000001.ifc'
 other.write_text(fixture.replace("0Jm4wmcl10lPX6wYa53X","2Jm4wmcl10lPX6wYa53X").replace(END,"""#30=IFCCARTESIANPOINT((10.,0.,0.));
#31=IFCAXIS2PLACEMENT3D(#30,$,$);
#32=IFCLOCALPLACEMENT($,#31);
#33=IFCPIPESEGMENT('3Jm4wmcl10lPX6wYa53Xra',$,'Pipe',$,$,#32,#14,$,.RIGIDSEGMENT.);
#34=IFCLOCALPLACEMENT(#32,#3);
#35=IFCDISTRIBUTIONPORT('3Jm4wmcl10lPX6wYa53Xrb',$,'Port_0','Flow',$,#34,$,.SOURCE.,.PIPE.,.DRAINAGE.);
#36=IFCDISTRIBUTIONPORT('3Jm4wmcl10lPX6wYa53Xrc',$,'Port_1','Flow',$,#34,$,.SINK.,.PIPE.,.DRAINAGE.);
#37=IFCRELNESTS('3Jm4wmcl10lPX6wYa53Xrd',$,'NestedPorts','Flow',#33,(#35,#36));
#56=IFCRELCONTAINEDINSPATIALSTRUCTURE('3Jm4wmcl10lPX6wYa53Xs7',$,$,$,(#15,#33),#52);
"""+END))
 sites=data/'site-id-corrections'/'DU'; sites.mkdir(parents=True)
 (sites/other.with_suffix('.csv').name).write_text('GlobalId,Site ID,Model,Set by,Set at,Status\n'
  f'3Jm4wmcl10lPX6wYa53Xra,{SITE},{other.name},someone,2026-11-19T10:00:00,Approved\n')
 rules=root/'rules.txt'; rules.write_text('site-set Asset\nsite-id SiteID\n')
 schema=Workbook(); s=schema.active
 for name in ('SiteID','Asset_Type'): s.append(['IFC_Filter',name])
 schema.save(data/'site-id-attributes.xlsx')
 (data/'location-referencing.csv').write_text('XYZ_LocationHierarchyCode,LocationHierarchyDescription\n')
 register=root/'combine.csv'
 register.write_text('combined,model,discipline,zone,mb,elements,contents,name\n'
  f'GROUP,{keeper.stem},BR,ZZZ,,,,\nGROUP,{other.stem},DU,ZZZ,,,,\n')
 out=root/'out'
 run=subprocess.run([str(exe),'split',str(keeper),str(other),'--out',str(out),'--rules',str(rules),'--combine',str(register),
  '--data-dir',str(data),'--property-schema',str(data/'site-id-attributes.xlsx'),'--no-zip','--no-instance','--no-compact','--simplify-mm','0','--merge-gap','0'],
  text=True,capture_output=True)
 assert run.returncode==0,run.stdout+run.stderr
 assert '1 elements joined to 1 sites' in run.stdout,run.stdout+run.stderr
 assert 'their ports nested on them' in run.stdout,run.stdout
 f=ifcopenshell.open(str(next((out/'federate').rglob('GROUP.federate.ifc'))))
 assert not f.by_type('IfcPipeSegment'),'the pipe should have joined the wall'
 ports=f.by_type('IfcDistributionPort')
 assert len(ports)==2,ports
 for port in ports:
  hosts=[r.RelatingObject for r in port.Nests]
  assert [h.GlobalId for h in hosts]==['0Jm4wmcl10lPX6wYa53Xrf'],(port.Name,hosts)
 assert {p.FlowDirection for p in ports}=={'SOURCE','SINK'}
print('Joined ports: a joined element\'s ports are nested on the element it joins, with their own data')
