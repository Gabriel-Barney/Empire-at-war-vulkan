#!/usr/bin/env python3
"""megtool.py - reference reader for Petroglyph/Alamo MEG archives.

Supports:
  v1  (EaW / FoC retail):  u32 numNames, u32 numFiles, ...
  v2  (EaW/FoC-era tools):  u32 0xFFFFFFFF, u32 0x3F7D70A4, u32 dataStart, u32 numNames, u32 numFiles
  v3  (Grey Goo / 8-bit Armies / Remaster era): u32 0xFFFFFFFF or 0x8FFFFFFF (encrypted),
       u32 0x3F7D70A4, u32 dataStart, u32 numNames, u32 numFiles, u32 nameTableSize

Encrypted v3 archives are detected and listed only if the header region is not
encrypted (it is, for 0x8FFFFFFF) -- this tool reports them and refuses to extract.

Usage:
  megtool.py list <file.meg> [--crc]
  megtool.py extract <file.meg> <outdir> [--filter GLOB]...
  megtool.py create <out.meg> <ARCHIVE\\NAME=srcfile>...
  megtool.py catalog <out.json> <label=file.meg>...   (merges into out.json if it exists)
"""
import fnmatch
import json
import os
import struct
import sys
import zlib

MAGIC_V2V3 = 0x3F7D70A4
ID_PLAIN = 0xFFFFFFFF
ID_ENCRYPTED = 0x8FFFFFFF


def meg_crc(name: str) -> int:
    """Engine CRC: standard CRC-32 (IEEE, reflected 0xEDB88320, init/xorout 0xFFFFFFFF)
    of the upper-cased ASCII name with backslash separators."""
    return zlib.crc32(name.upper().replace('/', '\\').encode('latin-1')) & 0xFFFFFFFF


class MegEntry:
    __slots__ = ('name', 'crc', 'index', 'size', 'offset', 'name_index', 'flags')

    def to_dict(self):
        return {'name': self.name, 'size': self.size, 'offset': self.offset,
                'crc': '%08X' % self.crc}


class MegArchive:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = f.read()
        self.version = None
        self.encrypted = False
        self.entries = []
        self._parse()

    def _u32(self, off):
        return struct.unpack_from('<I', self.data, off)[0]

    def _u16(self, off):
        return struct.unpack_from('<H', self.data, off)[0]

    def _read_names(self, off, count):
        names = []
        for _ in range(count):
            ln = self._u16(off)
            off += 2
            names.append(self.data[off:off + ln].decode('latin-1'))
            off += ln
        return names, off

    def _parse(self):
        d = self.data
        if len(d) < 8:
            raise ValueError('too small')
        a, b = self._u32(0), self._u32(4)
        if a in (ID_PLAIN, ID_ENCRYPTED) and b == MAGIC_V2V3:
            data_start = self._u32(8)
            n_names, n_files = self._u32(12), self._u32(16)
            # v3 has a name-table size field; v2 doesn't. Distinguish: in v3,
            # u32@20 + 24 + n_files*20 == data_start.
            cand = self._u32(20)
            if a == ID_ENCRYPTED:
                self.version, self.encrypted = 3, True
                return  # header tables are AES-encrypted; can't list without key
            if 24 + cand + n_files * 20 == data_start:
                self.version = 3
                names, off = self._read_names(24, n_names)
                for i in range(n_files):
                    flags, crc, idx, size, start, ni = struct.unpack_from('<HIIIIH', d, off)
                    off += 20
                    self._add(names, crc, idx, size, start, ni, flags)
            else:
                self.version = 2
                names, off = self._read_names(20, n_names)
                for i in range(n_files):
                    crc, idx, size, start, ni = struct.unpack_from('<5I', d, off)
                    off += 20
                    self._add(names, crc, idx, size, start, ni, 0)
        else:
            self.version = 1
            n_names, n_files = a, b
            names, off = self._read_names(8, n_names)
            for i in range(n_files):
                crc, idx, size, start, ni = struct.unpack_from('<5I', d, off)
                off += 20
                self._add(names, crc, idx, size, start, ni, 0)

    def _add(self, names, crc, idx, size, start, ni, flags):
        e = MegEntry()
        e.name = names[ni] if ni < len(names) else '<bad name index %d>' % ni
        e.crc, e.index, e.size, e.offset, e.name_index, e.flags = crc, idx, size, start, ni, flags
        self.entries.append(e)

    def read(self, e):
        if e.flags & 1:
            raise ValueError('entry is encrypted')
        if e.offset + e.size > len(self.data):
            raise ValueError('entry out of bounds')
        return self.data[e.offset:e.offset + e.size]

    def find(self, name):
        """Lookup the way the engine does: CRC32 of uppercased name, binary search on
        the CRC-sorted record table, then confirm by name."""
        c = meg_crc(name)
        lo, hi = 0, len(self.entries)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.entries[mid].crc < c:
                lo = mid + 1
            else:
                hi = mid
        while lo < len(self.entries) and self.entries[lo].crc == c:
            if self.entries[lo].name.upper() == name.upper().replace('/', '\\'):
                return self.entries[lo]
            lo += 1
        return None

    def verify(self):
        """Return list of problems (crc mismatch, unsorted, OOB)."""
        probs = []
        prev = -1
        for e in self.entries:
            if meg_crc(e.name) != e.crc:
                probs.append('crc mismatch: %s' % e.name)
            if e.crc < prev:
                probs.append('unsorted at %s' % e.name)
            prev = e.crc
            if e.offset + e.size > len(self.data):
                probs.append('oob: %s' % e.name)
        return probs


def safe_out_path(outdir, name):
    parts = [p for p in name.replace('\\', '/').split('/') if p not in ('', '.', '..')]
    return os.path.join(outdir, *parts)


def cmd_list(argv):
    m = MegArchive(argv[0])
    show_crc = '--crc' in argv
    for e in m.entries:
        if show_crc:
            print('%08X %10d %10d %s' % (e.crc, e.offset, e.size, e.name))
        else:
            print('%10d %10d %s' % (e.offset, e.size, e.name))


def cmd_extract(argv):
    path, outdir = argv[0], argv[1]
    filters = []
    i = 2
    while i < len(argv):
        if argv[i] == '--filter':
            filters.append(argv[i + 1].upper().replace('/', '\\'))
            i += 2
        else:
            i += 1
    m = MegArchive(path)
    n = 0
    for e in m.entries:
        if filters and not any(fnmatch.fnmatchcase(e.name.upper(), f) for f in filters):
            continue
        p = safe_out_path(outdir, e.name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(m.read(e))
        n += 1
    print('extracted %d files' % n, file=sys.stderr)


def cmd_catalog(argv):
    out = argv[0]
    cat = {}
    if os.path.exists(out):  # merge into existing catalog (re-cataloged labels are replaced)
        with open(out) as f:
            cat = json.load(f)
    for spec in argv[1:]:
        label, path = spec.split('=', 1)
        m = MegArchive(path)
        cat[label] = {
            'file_size': len(m.data), 'version': m.version, 'encrypted': m.encrypted,
            'problems': m.verify(),
            'entries': [e.to_dict() for e in m.entries],
        }
        print('%s: v%d %d entries' % (label, m.version, len(m.entries)), file=sys.stderr)
    with open(out, 'w') as f:
        json.dump(cat, f, indent=1)


def write_v1(out_path, files):
    """Write a retail-style v1 archive. files: list of (archive_name, bytes).
    Names are normalised (upper-case, backslashes); records CRC-sorted; data contiguous
    in name-table order, matching the retail invariants."""
    files = [(n.upper().replace('/', '\\'), d) for n, d in files]
    names = b''.join(struct.pack('<H', len(n)) + n.encode('latin-1') for n, _ in files)
    data_start = 8 + len(names) + 20 * len(files)
    offs, cur = [], data_start
    for _, d in files:
        offs.append(cur)
        cur += len(d)
    order = sorted(range(len(files)), key=lambda i: meg_crc(files[i][0]))
    out = bytearray(struct.pack('<II', len(files), len(files)))
    out += names
    for r, i in enumerate(order):
        out += struct.pack('<5I', meg_crc(files[i][0]), r, len(files[i][1]), offs[i], i)
    for _, d in files:
        out += d
    with open(out_path, 'wb') as f:
        f.write(out)


def cmd_create(argv):
    files = []
    for spec in argv[1:]:
        name, src = spec.split('=', 1)
        with open(src, 'rb') as f:
            files.append((name, f.read()))
    write_v1(argv[0], files)
    print('wrote %s (%d files)' % (argv[0], len(files)), file=sys.stderr)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    {'list': cmd_list, 'extract': cmd_extract, 'catalog': cmd_catalog, 'create': cmd_create}[cmd](sys.argv[2:])
    return 0


if __name__ == '__main__':
    sys.exit(main())
