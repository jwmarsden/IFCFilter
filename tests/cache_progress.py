"""Exercise the cache progress page with one failed and one successful model."""
import os,pathlib,subprocess,sys,tempfile

if os.name != 'nt':
    print('Windows batch integration test skipped on this platform')
    raise SystemExit(0)
repo=pathlib.Path(__file__).resolve().parents[1]
exe=pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
    root=pathlib.Path(temp)
    for part in ['source/RW','source/SW','input/RW','input/SW','output','progress','data']:
        (root/part).mkdir(parents=True)
    (root/'input/RW/good.ifc').write_bytes((repo/'tests/fixtures/opening.ifc').read_bytes())
    (root/'input/RW/bad.ifc').write_text('not an IFC')
    (root/'input/SW/excluded.ifc').write_text('must not be processed')
    # A cache left from a model now combined with others, and one in a discipline the run doesn't do
    for part in ['output/RW','output/SW']: (root/part).mkdir(parents=True)
    (root/'output/RW/gone.federate.ifcc').write_text('old')
    (root/'output/SW/gone.federate.ifcc').write_text('old')
    rules=root/'rules.txt';rules.write_text('')
    script=repo/'scripts/regenerate-cache-progress.ps1'
    cmd=['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(script),'-Exe',str(exe),'-Rules',str(rules),'-Source',str(root/'source'),'-InputDir',str(root/'input'),'-Output',str(root/'output'),'-DataDir',str(root/'data'),'-ProgressDir',str(root/'progress')]
    result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    assert result.returncode==1,result.stdout
    assert 'Processed 2 of 2 models; 1 failed.' in result.stdout,result.stdout
    page=(root/'progress/progress-cache.html').read_text(encoding='utf8')
    assert 'finished with failures' in page and 'good.ifc' in page and 'bad.ifc' in page and 'Failed (exit 1)' in page
    assert 'excluded.ifc' not in page and 'http-equiv="refresh"' not in page
    assert (root/'output/RW/good.ifcc').is_file()
    assert not (root/'output/RW/gone.federate.ifcc').exists(),'a cache with no federation file should go'
    assert (root/'output/SW/gone.federate.ifcc').exists(),'a discipline the run does not do is left alone'
    logs=list((root/'progress/cache-logs').glob('*.log'));assert len(logs)==1
    assert 'no DATA section' in logs[0].read_text(encoding='utf8')
    result=subprocess.run(cmd+['-Selection','SW'],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    assert result.returncode!=0 and 'excluded discipline' in result.stdout,result.stdout
print('Cache progress reports success and failure, continues processing and excludes SW')
