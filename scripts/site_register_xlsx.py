"""Read/write the site register, preserving values longer than Excel's cell limit."""
from collections import defaultdict
from openpyxl import Workbook, load_workbook
from openpyxl.cell import WriteOnlyCell

PREFIX = 'OVERFLOW:'
CHUNK = 30000


def write_register(path, rows):
    workbook = Workbook(write_only=True)
    sheet = workbook.create_sheet('Site register')
    overflow = workbook.create_sheet('Overflow')
    sheet.freeze_panes = 'B2'
    overflow.append(['Reference', 'Part', 'Parts', 'Text'])
    for row_number, row in enumerate(rows, 1):
        cells = list(row)
        for col, value in enumerate(cells, 1):
            if isinstance(value, str) and len(value) > 32767:
                key = f'{PREFIX}{row_number}:{col}'
                parts = [value[i:i + CHUNK] for i in range(0, len(value), CHUNK)]
                for index, part in enumerate(parts, 1):
                    overflow.append([key, index, len(parts), part])
                cells[col - 1] = key
            elif value == '':
                cells[col - 1] = None
            elif isinstance(value, str) and value.startswith('='):
                cell = WriteOnlyCell(sheet, value=value)
                cell.data_type = 's'
                cells[col - 1] = cell
        sheet.append(cells)
    workbook.save(path)


def read_register(path):
    workbook = load_workbook(path, read_only=True, data_only=True)
    try:
        parts = defaultdict(list)
        if 'Overflow' in workbook.sheetnames:
            for key, index, total, value in workbook['Overflow'].iter_rows(min_row=2, max_col=4, values_only=True):
                if key:
                    parts[str(key)].append((int(index), int(total), value or ''))
        restored = {}
        for key, chunks in parts.items():
            chunks.sort()
            if len(chunks) != chunks[0][1] or [x[0] for x in chunks] != list(range(1, len(chunks) + 1)) or any(x[1] != len(chunks) for x in chunks):
                raise ValueError(f'{path}: incomplete overflow value {key}')
            restored[key] = ''.join(x[2] for x in chunks)
        rows = workbook.worksheets[0].iter_rows(values_only=True)
        headers = next(rows)
        for row in rows:
            values = []
            for value in row[:len(headers)]:
                if isinstance(value, str) and value.startswith(PREFIX):
                    if value not in restored:
                        raise ValueError(f'{path}: missing overflow value {value}')
                    value = restored[value]
                values.append(value if value is not None else '')
            yield dict(zip(headers, values))
    finally:
        workbook.close()
