"""Build a music-only WAD from an owner's legacy LCS Android OBB.

The OBB is already a DAWL archive, not a ZIP. Keep only MP3 entries absent
from the installed main WAD; never replace newer game assets with old ones.
The original encrypted payloads and CRCs are preserved, with XOR phase
adjusted when an entry moves to a different byte parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

KEY = b'\xaf\x66'
ENTRY = struct.Struct('<4I4Q')


def crypt(data, offset=0):
    return bytes(value ^ KEY[(offset + index) & 1] for index, value in enumerate(data))


def inventory(path):
    with path.open('rb') as source:
        header = crypt(source.read(2048))
        if header[:12] != b'DAWL\x01\0\0\0\x01\0\0\0':
            raise ValueError(f'Unsupported WAD header: {path}')
        count = struct.unpack('<I', crypt(source.read(4), 2048))[0]
        if not 0 < count < 100000:
            raise ValueError('Invalid directory count')
        directories = crypt(source.read(count * 8), 2052)
        pos = source.tell()
        count = struct.unpack('<I', crypt(source.read(4), pos))[0]
        if not 0 < count < 100000:
            raise ValueError('Invalid file count')
        pos = source.tell()
        records = list(ENTRY.iter_unpack(crypt(source.read(count * ENTRY.size), pos)))
        pos = source.tell()
        names_size = struct.unpack('<I', crypt(source.read(4), pos))[0]
        names = crypt(source.read(names_size), pos + 4)
        if any(record[4] + record[5] > path.stat().st_size for record in records):
            raise ValueError('WAD entry outside archive')
        return header, directories, records, names


def build(source_path, base_path, destination):
    header, directories, records, names = inventory(source_path)
    base_hashes = {record[0] for record in inventory(base_path)[2]}
    selected = []
    with source_path.open('rb') as source:
        for record in records:
            if record[0] in base_hashes:
                continue
            source.seek(record[4])
            sample = crypt(source.read(min(record[5], 1024)), record[4])
            if not (sample.startswith(b'ID3') or sample[:2] in (b'\xff\xfb', b'\xff\xf3', b'\xff\xf2')):
                continue
            if record[5] != record[6] or record[7]:
                raise ValueError('Compressed MP3 entry is unsupported')
            selected.append(record)
    if not selected:
        raise ValueError('No missing MP3 entries found')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise FileExistsError(destination)
    metadata_size = 2048 + 4 + len(directories) + 4 + len(selected) * ENTRY.size + 4 + len(names)
    data_start = (metadata_size + 2047) & ~2047
    output_records, manifest = [], []
    flip = bytes.maketrans(bytes(range(256)), bytes(value ^ 0xc9 for value in range(256)))
    with source_path.open('rb') as source, destination.open('xb+') as output:
        output.write(b'\0' * data_start)
        for record in selected:
            position = output.tell()
            source.seek(record[4])
            remaining = record[5]
            digest = hashlib.sha256()
            while remaining:
                block = source.read(min(remaining, 1 << 20))
                if not block:
                    raise ValueError('Truncated source payload')
                if (position ^ record[4]) & 1:
                    block = block.translate(flip)
                output.write(block)
                digest.update(block)
                remaining -= len(block)
            output_records.append((*record[:4], position, *record[5:]))
            manifest.append({'hash': f'{record[0]:08x}', 'bytes': record[5],
                             'source_offset': record[4], 'output_offset': position,
                             'encrypted_sha256': digest.hexdigest()})
        metadata = header + struct.pack('<I', len(directories) // 8) + directories
        metadata += struct.pack('<I', len(selected)) + b''.join(ENTRY.pack(*record) for record in output_records)
        metadata += struct.pack('<I', len(names)) + names
        output.seek(0)
        output.write(crypt(metadata.ljust(data_start, b'\0')))
    # Reparse and compare every payload byte, allowing only the XOR phase change.
    assert len(inventory(destination)[2]) == len(selected)
    with source_path.open('rb') as source, destination.open('rb') as output:
        for old, new in zip(selected, output_records):
            source.seek(old[4]); output.seek(new[4])
            remaining = old[5]
            while remaining:
                original = source.read(min(remaining, 1 << 20))
                relocated = output.read(len(original))
                if (old[4] ^ new[4]) & 1:
                    original = original.translate(flip)
                if original != relocated:
                    raise ValueError(f'Payload verification failed: {old[0]:08x}')
                remaining -= len(original)
    report = {'source': str(source_path.resolve()), 'base': str(base_path.resolve()),
              'output': str(destination.resolve()), 'entries': manifest}
    destination.with_suffix('.manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{destination}: {len(selected)} MP3 entries, {destination.stat().st_size:,} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    build(arguments.source, arguments.base, arguments.output)
