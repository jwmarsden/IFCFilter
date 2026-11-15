"""Build an editable, one-row-per-site register from existing IFCViewer caches.

Usage: python scripts/site-register.py C:/Apps/IFCViewer data/site-register.csv
Conflicting values are blank; GUIDs and files are listed to show sites spanning models.
This is a report generator, not yet an override mechanism.
"""
import csv
import pathlib
import sqlite3
import sys
from collections import defaultdict


def build(root, output):
    if output.exists():
        raise SystemExit(f'{output} already exists; refusing to overwrite your edits')
    sites = {}
    fields = set()
    for file in sorted(root.rglob('*.ifcc')):
        with sqlite3.connect(f'file:{file.as_posix()}?mode=ro', uri=True) as db:
            site_elements = db.execute('''
                SELECT e.id, e.guid, value_text.text
                FROM elements e
                JOIN element_psets ep ON ep.element=e.id
                JOIN psets p ON p.id=ep.pset JOIN strings ps ON ps.id=p.name AND ps.text='IFCFilter'
                JOIN pset_values pv ON pv.pset=p.id JOIN strings pn ON pn.id=pv.name AND pn.text='SiteID'
                JOIN "values" v ON v.id=pv.value JOIN strings value_text ON value_text.id=v.text
                WHERE value_text.text<>''
            ''').fetchall()
            lookup = {}
            for element, guid, site in site_elements:
                if '; ' in site:
                    continue  # a combined value cannot be assigned safely to one site
                entry = sites.setdefault(site, {'guids': set(), 'files': set(), 'sources': set(), 'values': defaultdict(set)})
                if guid:
                    entry['guids'].add(guid)
                entry['files'].add(str(file.relative_to(root)))
                lookup[element] = entry
            if not lookup:
                continue
            query = '''
                SELECT ep.element, ps.text, pn.text, vt.text, v.number
                FROM element_psets ep JOIN psets p ON p.id=ep.pset
                JOIN strings ps ON ps.id=p.name JOIN pset_values pv ON pv.pset=p.id
                JOIN strings pn ON pn.id=pv.name JOIN "values" v ON v.id=pv.value
                LEFT JOIN strings vt ON vt.id=v.text WHERE ep.element=?
            '''
            for element, entry in lookup.items():
                for _, pset, name, text, number in db.execute(query, (element,)):
                    field = pset + '.' + name
                    fields.add(field)
                    value = text if text is not None else (str(number) if number is not None else '')
                    if value:
                        entry['values'][field].add(value)
                        if field == 'IFCFilter.SourceFile':
                            entry['sources'].update(value.split('; '))
    for code in ('Co', 'En', 'SL', 'EF', 'Ss', 'Pr'):
        fields.update((f'Classification.{code} Value', f'Classification.{code} Description'))
    fields = sorted(fields)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x', newline='', encoding='utf-8-sig') as f:
        writer = csv.writer(f)
        writer.writerow(['SiteID', 'Output GUID count', 'Output GUIDs', 'Source file count', 'Source files', 'Cache file count', 'Cache files', *fields])
        for site, data in sorted(sites.items()):
            values = [next(iter(v)) if len(v) == 1 else '' for v in (data['values'].get(name, set()) for name in fields)]
            writer.writerow([site, len(data['guids']), '; '.join(sorted(data['guids'])), len(data['sources']), '; '.join(sorted(data['sources'])), len(data['files']), '; '.join(sorted(data['files'])), *values])
    multi_file = sum(len(data['files']) > 1 for data in sites.values())
    multi_source = sum(len(data['sources']) > 1 for data in sites.values())
    multi_guid = sum(len(data['guids']) > 1 for data in sites.values())
    print(f'{len(sites)} sites; {multi_source} span multiple source files; {multi_file} span multiple caches; {multi_guid} have multiple output GUIDs')
    print(output)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))

