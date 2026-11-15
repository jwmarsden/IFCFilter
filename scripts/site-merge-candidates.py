"""Make a reviewable list of cross-file site candidates from caches and source manifests.

Usage: python scripts/site-merge-candidates.py SITE_REGISTER FEDERATION_DIR OUTPUT.xlsx
Output has SiteID, SourceFile and source GlobalId. Add rows for untagged geometry.
This prepares a candidate list; it does not modify or merge IFC geometry.
"""
import csv
import pathlib
import re
import sys
from collections import defaultdict
from openpyxl import Workbook
from openpyxl.cell import WriteOnlyCell
from openpyxl.styles import Font, PatternFill

csv.field_size_limit(2**31-1)

def valid_site(site):
    return bool(re.fullmatch(r'(?:[A-Za-z]+|RC[123])[0-9]{6}', site)) and not site.endswith('000000')

def run(register, federation, output):
    if output.exists():
        raise SystemExit(f'{output} exists; refusing to overwrite edits')
    guid_sites = defaultdict(set)
    by_manifest = defaultdict(lambda: defaultdict(set))
    with register.open(encoding='utf-8-sig', newline='') as f:
        for row in csv.DictReader(f):
            if not valid_site(row['SiteID']) or int(row['Source file count']) < 2:
                continue
            for guid in row['Output GUIDs'].split('; '):
                if guid:
                    guid_sites[guid].add(row['SiteID'])
                    for cache in row['Cache files'].split('; '):
                        if cache:
                            manifest = pathlib.Path(cache).with_suffix('.manifest.csv')
                            by_manifest[manifest][guid].add(row['SiteID'])
    rows=set(); matched=set()
    for relative, candidates in sorted(by_manifest.items()):
        manifest = federation / relative
        if not manifest.exists():
            continue
        with manifest.open(encoding='utf-8-sig', newline='') as f:
            reader=csv.DictReader(f)
            if not reader.fieldnames or 'output_guid' not in reader.fieldnames:
                continue
            for row in reader:
                for site in candidates.get(row['output_guid'], ()):
                    if row['source_file'] and row['source_guid']:
                        rows.add((site,row['source_file'],row['source_guid']))
                        matched.add(row['output_guid'])
    by_source=defaultdict(set)
    for site,source,guid in rows:
        by_source[source,guid].add(site)
    conflicts=[key for key,sites in by_source.items() if len(sites)>1]
    conflicts=set(conflicts)
    output.parent.mkdir(parents=True,exist_ok=True)
    if output.suffix.lower() != '.xlsx':
        raise SystemExit('Output must be an .xlsx workbook')
    workbook=Workbook(write_only=True)
    sheet=workbook.create_sheet('Merge Candidates')
    sheet.freeze_panes='B2'
    for col,width in {'A':18,'B':45,'C':26,'D':18}.items(): sheet.column_dimensions[col].width=width
    header=[]
    for name in ['SiteID','SourceFile','GlobalId','Status']:
        cell=WriteOnlyCell(sheet,value=name)
        cell.font=Font(bold=True,color='FFFFFF')
        cell.fill=PatternFill('solid',fgColor='315E7D')
        header.append(cell)
    sheet.append(header)
    for site,source,guid in sorted(rows):
        sheet.append([site,source,guid,'CONFLICT' if (source,guid) in conflicts else 'candidate'])
    sheet.auto_filter.ref=f'A1:D{len(rows)+1}'
    workbook.save(output)
    print(f'{len(rows)} source elements for {len({r[0] for r in rows})} sites; {len(guid_sites)-len(matched)} output GUIDs missing from manifests')
    print(f'{len(conflicts)} source elements have conflicting site assignments and require review')
    print(output)

if __name__=='__main__':
    if len(sys.argv)!=4: raise SystemExit(__doc__)
    run(*(pathlib.Path(x) for x in sys.argv[1:]))



