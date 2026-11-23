"""Regression test for SiteID correction lookup; pass the path to ifcfilter.exe."""
import csv
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
from openpyxl import Workbook

exe = pathlib.Path(sys.argv[1]).resolve()
fixture = pathlib.Path(__file__).parent / 'fixtures' / 'opening.ifc'
with tempfile.TemporaryDirectory() as temp:
    root = pathlib.Path(temp)
    text = fixture.read_text()
    extra = """#101=IFCPROPERTYSINGLEVALUE('SiteID',$,IFCLABEL('PU000123'),$);
#102=IFCPROPERTYSINGLEVALUE('DesignTag',$,IFCLABEL('OLD'),$);
#103=IFCPROPERTYSINGLEVALUE('Uniclass_EfCode',$,IFCLABEL('EF_15'),$);
#104=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xri',$,'Asset',$,(#101,#102,#103));
#105=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrj',$,$,$,(#15),#104);
#106=IFCPROPERTYSINGLEVALUE('Identifier',$,IFCLABEL('ORIGINAL ID'),$);
#107=IFCPROPERTYSINGLEVALUE('Description',$,IFCLABEL('ORIGINAL DESCRIPTION'),$);
#108=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrk',$,'IFCFilter',$,(#106,#107,#125,#126));
#109=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrl',$,$,$,(#15),#108);
#125=IFCPROPERTYSINGLEVALUE('Alias',$,IFCLABEL('SOURCE ALIAS'),$);
#126=IFCPROPERTYSINGLEVALUE('Operational_ID',$,IFCLABEL('SOURCE OPERATIONAL'),$);
#111=IFCPROPERTYSINGLEVALUE('DisciplineCode',$,IFCLABEL('BR'),$);
#112=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrm',$,'Project',$,(#111));
#113=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrn',$,$,$,(#15),#112);
#114=IFCRELAGGREGATES('0Jm4wmcl10lPX6wYa53Xro',$,$,$,#15,(#115));
#115=IFCWALL('0Jm4wmcl10lPX6wYa53Xrp',$,'Child',$,$,#4,#14,$,$);
#116=IFCPROPERTYSINGLEVALUE('PrimaryParent',$,IFCTEXT('PRIMARY-1'),$);
#117=IFCPROPERTYSINGLEVALUE('ElectricalParent',$,IFCTEXT('ELECTRICAL-1'),$);
#118=IFCPROPERTYSINGLEVALUE('CommsParent',$,IFCTEXT('COMMS-1'),$);
#119=IFCPROPERTYSINGLEVALUE('SupplyIsolationID',$,IFCTEXT('ISOLATION-1'),$);
#120=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrq',$,'AssetData',$,(#116,#117,#118,#119));
#121=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrr',$,$,$,(#15),#120);
"""
    model = root / 'test.ifc'
    model.write_text(text.replace('ENDSEC;\nEND-ISO-10303-21;', extra+'ENDSEC;\nEND-ISO-10303-21;'))
    rules = root / 'rules.txt'
    rules.write_text('site-set Asset\nsite-id SiteID\ndesign-set Asset\ndesign-tag DesignTag\nproject-set Project\nhierarchy-set AssetData\nhierarchy-source SafetyParent SupplyIsolationID\n')
    lookup = root / 'lookup.csv'
    lookup.write_text('SiteID,DesignTag,Asset Type,Location,Zone,Co,En,SL,EF,Ss,Pr,Identifier,Description\nPU000123,NEW,PUMP,LOC,A,,,,EF_20,,,CORRECTED ID,CORRECTED DESCRIPTION\n')
    schema_book = Workbook()
    schema_sheet = schema_book.active
    for name in ('SiteID', 'Design_ID', 'Asset_Type', 'Location', 'Zone', 'Identifier', 'Description', 'Owner', 'Maintainer', 'Location_Description', 'Alias', 'Operational_ID'):
        schema_sheet.append(('IFC_Filter', name))
    for name in ('EF_Code', 'EF_Description', 'Co_Code', 'Co_Description', 'En_Code', 'En_Description', 'SL_Code', 'SL_Description'):
        schema_sheet.append(('Classification', name))
    schema_sheet.append(('Project', 'DisciplineCode'))
    for name in ('Location', 'PrimaryParent', 'ElectricalParent', 'CommsParent', 'SafetyParent'):
        schema_sheet.append(('Hierarchy', name))
    schema = root / 'schema.xlsx'
    schema_book.save(schema)
    reference = root / 'location-referencing.csv'
    reference.write_text('XYZ_LocationHierarchyCode,LocationHierarchyDescription,Uniclass_CoCode,Uniclass_CoTitle,Uniclass_EnCode,Uniclass_EnTitle,Uniclass_SLCode,Uniclass_SLTitle\nLOC,Example location,Co_20,Complex,En_20,Entity,SL_20,Space\n')
    out = root / 'output'
    cmd = [str(exe), 'split', str(model), '--out', str(out), '--rules', str(rules), '--site-lookup', str(lookup), '--property-schema', str(schema), '--no-zip', '--no-merge', '--no-instance', '--no-compact', '--simplify-mm', '0']
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    result = next((out / 'federate').rglob('*.ifc'))
    cache = root / 'test.ifcc'
    subprocess.run([str(exe), 'cache', str(result), '--out', str(cache), '--rules', str(rules)], check=True, stdout=subprocess.DEVNULL)
    with sqlite3.connect(cache) as db:
        rows = db.execute('''SELECT sn.text, pn.text, t.text FROM elements e
          JOIN element_psets ep ON ep.element=e.id JOIN psets p ON p.id=ep.pset
          JOIN strings sn ON sn.id=p.name JOIN pset_values pv ON pv.pset=p.id
          JOIN strings pn ON pn.id=pv.name JOIN "values" v ON v.id=pv.value
          LEFT JOIN strings t ON t.id=v.text WHERE e.guid='0Jm4wmcl10lPX6wYa53Xrf' ''').fetchall()
    resolved = db.execute("SELECT e.guid, f.zone, f.discipline FROM elements e JOIN element_filters f ON f.element=e.id WHERE e.guid IN (?,?)", ('0Jm4wmcl10lPX6wYa53Xrf', '0Jm4wmcl10lPX6wYa53Xrp')).fetchall()
    db.close()
    assert dict((guid, (zone, discipline)) for guid,zone,discipline in resolved) == {'0Jm4wmcl10lPX6wYa53Xrf': ('A','BR'), '0Jm4wmcl10lPX6wYa53Xrp': ('A','BR')}, resolved
    actual = {(s,n):v for s,n,v in rows}
    assert actual.get(('IFC_Filter','Alias')) == 'SOURCE ALIAS'
    assert actual.get(('IFC_Filter','Operational_ID')) == 'SOURCE OPERATIONAL'
    for key, value in {('IFC_Filter','SiteID'):'PU000123',('IFC_Filter','Design_ID'):'NEW',('IFC_Filter','Asset_Type'):'PUMP',('IFC_Filter','Location'):'LOC',('IFC_Filter','Location_Description'):'Example location',('IFC_Filter','Identifier'):'CORRECTED ID',('IFC_Filter','Description'):'CORRECTED DESCRIPTION',('Classification','Co_Code'):'Co_20',('Classification','Co_Description'):'Complex',('Classification','En_Code'):'En_20',('Classification','SL_Code'):'SL_20',('Classification','EF_Code'):'EF_20',('Classification','EF_Description'):'Structural elements'}.items():
        assert actual.get(key) == value, (key, actual.get(key))
    for name, value in {'Location': 'LOC', 'PrimaryParent': 'PRIMARY-1', 'ElectricalParent': 'ELECTRICAL-1', 'CommsParent': 'COMMS-1', 'SafetyParent': 'ISOLATION-1'}.items():
        assert actual.get(('Hierarchy', name)) == value, (name, actual.get(('Hierarchy', name)))
    # Prefer the workbook next to the property schema, even when a stale CSV also exists.
    location_book = Workbook()
    location_sheet = location_book.active
    location_sheet.append(['XYZ_LocationHierarchyCode', 'LocationHierarchyDescription', 'Uniclass_CoCode'])
    location_sheet.append(['LOC', 'Workbook location', 'Co_20'])
    location_book.save(root / 'location-referencing.xlsx')
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    assert "IFCPROPERTYSINGLEVALUE('Location_Description',$,IFCLABEL('Workbook location')" in result.read_text()
    (root / 'location-referencing.xlsx').unlink()
    # Excel's inline strings and an edited correction must take the same path as CSV.
    workbook = Workbook()
    sheet = workbook.active
    sheet.append(['SiteID','DesignTag','Asset Type','Location','Zone','Co','En','SL','EF','Ss','Pr','Identifier','Description','Owner','Maintainer','Alias','Operational_ID'])
    sheet.append(['PU000123','FROM EXCEL','PUMP','LOC','A',None,None,None,'EF_20',None,None,'EXCEL ID','EXCEL DESCRIPTION','OWNER TEST','MAINTAINER TEST','LOOKUP ALIAS','LOOKUP OPERATIONAL'])
    excel = root / 'lookup.xlsx'
    workbook.save(excel)
    excel_cmd = cmd.copy()
    excel_cmd[excel_cmd.index(str(lookup))] = str(excel)
    subprocess.run(excel_cmd, check=True, stdout=subprocess.DEVNULL)
    assert "IFCPROPERTYSINGLEVALUE('Design_ID',$,IFCLABEL('FROM EXCEL')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Identifier',$,IFCLABEL('EXCEL ID')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Description',$,IFCLABEL('EXCEL DESCRIPTION')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Owner',$,IFCLABEL('OWNER TEST')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Maintainer',$,IFCLABEL('MAINTAINER TEST')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Alias',$,IFCLABEL('LOOKUP ALIAS')" in result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Operational_ID',$,IFCLABEL('LOOKUP OPERATIONAL')" in result.read_text()
    # A suffix is not a match for the shorter location code.
    lookup.write_text('SiteID,DesignTag,Asset Type,Location,Zone,Co,En,SL,EF,Ss,Pr\nPU000123,,,LOC-SUFFIX,,,,,,,\n')
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    assert "IFCPROPERTYSINGLEVALUE('Location_Description',$,IFCLABEL(''),$)" in result.read_text()
    lookup.write_text('SiteID,DesignTag,Asset Type,Location,Zone,Co,En,SL,EF,Ss,Pr\nPU000123,,,,,,,,,,\n')
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    text = result.read_text()
    assert "IFCPROPERTYSINGLEVALUE('Design_ID',$,IFCLABEL('OLD')" in text
    assert "IFCPROPERTYSINGLEVALUE('EF_Code',$,IFCLABEL('EF_15')" in text
    assert "IFCPROPERTYSINGLEVALUE('Identifier',$,IFCLABEL('ORIGINAL ID')" in text
    assert "IFCPROPERTYSINGLEVALUE('Description',$,IFCLABEL('ORIGINAL DESCRIPTION')" in text
    # Multiple different source values must not be silently chosen for a merged hierarchy field.
    conflict = """#122=IFCPROPERTYSINGLEVALUE('PrimaryParent',$,IFCTEXT('OTHER-PARENT'),$);
#123=IFCPROPERTYSET('0Jm4wmcl10lPX6wYa53Xrs',$,'AssetData',$,(#122));
#124=IFCRELDEFINESBYPROPERTIES('0Jm4wmcl10lPX6wYa53Xrt',$,$,$,(#15),#123);
"""
    model.write_text(model.read_text().replace('ENDSEC;\nEND-ISO-10303-21;', conflict+'ENDSEC;\nEND-ISO-10303-21;'))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    assert "IFCPROPERTYSINGLEVALUE('PrimaryParent',$,IFCLABEL(''),$)" in result.read_text()
print('Site lookup overrides and blank preservation passed')



