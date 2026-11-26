"""SKIP in combine.csv's combined column leaves a model out of a split run, in any case, while the rest are done."""
import pathlib,subprocess,sys,tempfile
from openpyxl import Workbook
exe=pathlib.Path(sys.argv[1]).resolve()
fixture=(pathlib.Path(__file__).parent/'fixtures'/'opening.ifc').read_text()
with tempfile.TemporaryDirectory() as temp:
 root=pathlib.Path(temp); (root/'XY').mkdir()
 models=[]
 for n in ('000001','000002','000003'):
  model=root/'XY'/f'P-X-ZZZ-XY-M3D-{n}.ifc'
  model.write_text(fixture.replace('0Jm4wmcl10lPX6wYa53X','0Jm4wmcl10lPX6w'+n+'X')); models.append(model)
 register=root/'combine.csv'
 register.write_text('combined,model,discipline,zone,mb,elements,contents,name\n'
  'SKIP,P-X-ZZZ-XY-M3D-000001,XY,ZZZ,,,,\n,P-X-ZZZ-XY-M3D-000002,XY,ZZZ,,,,\nskip,P-X-ZZZ-XY-M3D-000003,XY,ZZZ,,,,\n',encoding='utf-8-sig')
 workbook=Workbook();workbook.active.append(('IFC_Filter','SiteID'));workbook.save(root/'site-id-attributes.xlsx')
 (root/'location-referencing.csv').write_text('LocationHierarchyCode,LocationHierarchyDescription\nLOC,Example\n')
 (root/'rules.txt').write_text('# none\n')
 out=root/'out'
 run=subprocess.run([str(exe),'split',str(root/'XY'),'--out',str(out),'--data-dir',str(root),'--rules',str(root/'rules.txt'),'--combine',str(register),'--no-zip','--no-merge','--no-instance','--no-compact'],capture_output=True,text=True)
 assert run.returncode==0,run.stdout+run.stderr
 assert '2 models marked SKIP in combine.csv, so left out' in run.stdout,run.stdout
 done=sorted(p.name for p in (out/'federate'/'XY').glob('*.federate.ifc'))
 assert done==['P-X-ZZZ-XY-M3D-000002.federate.ifc'],done
 # and none of them goes to parts, as SKIP isn't a group to combine
 assert not (out/'parts').exists() or not any((out/'parts').rglob('*.ifc')),list((out/'parts').rglob('*'))
print('SKIP in combine.csv leaves those models out of the run, and only those')
