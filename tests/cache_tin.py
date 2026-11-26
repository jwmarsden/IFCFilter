"""A survey's TIN, IfcTriangulatedIrregularNetwork, is cached as its triangles, not drawn as its box."""
import pathlib, subprocess, sys, tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
MODEL = """ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [ReferenceView]'),'2;1');
FILE_NAME('tin.ifc','2026-01-01T00:00:00',(),(),'IFCFilter test','IFCFilter test','');
FILE_SCHEMA(('IFC4X3_ADD2'));
ENDSEC;
DATA;
#1=IFCPROJECT('0Jm4wmcl10lPX6wYa53Xre',$,'Test',$,$,$,$,(#6),#7);
#2=IFCCARTESIANPOINT((0.,0.,0.));
#3=IFCAXIS2PLACEMENT3D(#2,$,$);
#4=IFCLOCALPLACEMENT($,#3);
#6=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-5,#3,$);
#7=IFCUNITASSIGNMENT((#8));
#8=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#9=IFCSITE('0Jm4wmcl10lPX6wYa53Xs1',$,'Site',$,$,#4,$,$,.ELEMENT.,$,$,$,$,$);
#10=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xs4',$,$,$,#1,(#9));
#11=IFCCARTESIANPOINTLIST3D(((0.,0.,1.),(100.,0.,2.),(100.,100.,3.),(0.,100.,1.),(50.,50.,10.)),$);
#12=IFCTRIANGULATEDIRREGULARNETWORK(#11,$,.F.,((1,2,5),(2,3,5),(3,4,5),(4,1,5)),$,(0,0,0,0));
#13=IFCSHAPEREPRESENTATION(#6,'Body','Tessellation',(#12));
#14=IFCPRODUCTDEFINITIONSHAPE($,$,(#13));
#15=IFCGEOGRAPHICELEMENT('0Jm4wmcl10lPX6wYa53Xrf',$,'Surface',$,$,#4,#14,$,.TERRAIN.);
#16=IFCRELCONTAINEDINSPATIALSTRUCTURE('0Jm4wmcl10lPX6wYa53Xs7',$,$,$,(#15),#9);
ENDSEC;
END-ISO-10303-21;
"""
with tempfile.TemporaryDirectory() as temp:
    root = pathlib.Path(temp)
    model = root / "tin.ifc"
    model.write_text(MODEL)
    run = subprocess.run([str(exe), "cache", str(model), "--out", str(root / "tin.ifcc")], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "4 triangles" in run.stdout, run.stdout
    assert "drawn as their box" not in run.stdout, run.stdout
print("A TIN is cached as its triangles")
