"""Verify lossless workbook handling for values longer than an Excel cell."""
import pathlib,sys,tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
from site_register_xlsx import read_register,write_register
with tempfile.TemporaryDirectory() as tmp:
 p=pathlib.Path(tmp)/'register.xlsx'
 original='; '.join(f'guid{i:06d}' for i in range(6000))
 rows=[['SiteID','Output GUIDs','Source file count'],['BR123789',original,2],['BR234890','abc',1]]
 write_register(p,iter(rows))
 output=list(read_register(p))
 assert output[0]['Output GUIDs']==original
 assert output[0]['Source file count']==2
 assert output[1]['Output GUIDs']=='abc'
print('Site register XLSX overflow round trip passed')

