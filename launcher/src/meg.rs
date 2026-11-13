//! Reader for Petroglyph `.meg` archives (format 1 as shipped with EaW/FoC, plus the
//! unencrypted format 2/3 headers used by later 64-bit builds).

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct MegEntry {
    /// Upper-cased, backslash-separated path as stored in the archive.
    pub name: String,
    pub offset: u64,
    pub size: u32,
}

#[derive(Clone, Debug)]
pub struct MegArchive {
    pub path: PathBuf,
    pub entries: Vec<MegEntry>,
}

fn u16_at(b: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(o..o + 2)?.try_into().ok()?))
}
fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}

impl MegArchive {
    pub fn open(path: &Path) -> Result<Self, String> {
        let mut f = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let file_len = f.metadata().map(|m| m.len()).unwrap_or(0);
        let mut head = [0u8; 24];
        let got = f.read(&mut head).map_err(|e| e.to_string())?;
        if got < 8 {
            return Err(format!("{}: too small to be a MEG archive", path.display()));
        }
        let a = u32_at(&head, 0).unwrap();
        let b = u32_at(&head, 4).unwrap();

        // Format 2/3: flags (0xFFFFFFFF plain, 0x8FFFFFFF encrypted), id 0x3F7D70A4,
        // data start, name count, file count, [v3: name table size].
        let (names_at, n_names, n_files, table_end, v3) = if b == 0x3F7D_70A4 {
            if a == 0x8FFF_FFFF {
                return Err(format!("{}: encrypted MEG archives aren't supported", path.display()));
            }
            let data_start = u32_at(&head, 8).unwrap() as u64;
            let n_names = u32_at(&head, 12).unwrap();
            let n_files = u32_at(&head, 16).unwrap();
            let names_size = u32_at(&head, 20).unwrap() as u64;
            // v3 has the extra name-table-size field; v2 goes straight into the name table.
            let is_v3 = 24 + names_size + n_files as u64 * 20 == data_start;
            (if is_v3 { 24 } else { 20 }, n_names, n_files, data_start, is_v3)
        } else {
            (8u64, a, b, 0u64, false)
        };
        if n_names != n_files || n_files > 2_000_000 {
            return Err(format!("{}: not a recognised MEG archive", path.display()));
        }

        // Read the whole table region in one go.
        f.seek(SeekFrom::Start(names_at)).map_err(|e| e.to_string())?;
        let table = if table_end > names_at {
            let mut t = vec![0u8; (table_end - names_at) as usize];
            f.read_exact(&mut t).map_err(|e| e.to_string())?;
            t
        } else {
            // Format 1 doesn't say where the tables end; names are short, so read
            // generously and grow if needed.
            let mut t = Vec::new();
            let want = (n_files as u64 * (20 + 2 + 96)).min(file_len.saturating_sub(names_at));
            f.by_ref().take(want).read_to_end(&mut t).map_err(|e| e.to_string())?;
            t
        };

        let bad = || format!("{}: truncated MEG table", path.display());
        let mut names = Vec::with_capacity(n_names as usize);
        let mut o = 0usize;
        for _ in 0..n_names {
            let len = u16_at(&table, o).ok_or_else(bad)? as usize;
            o += 2;
            let raw = table.get(o..o + len).ok_or_else(bad)?;
            o += len;
            names.push(String::from_utf8_lossy(raw).to_ascii_uppercase().replace('/', "\\"));
        }
        let mut entries = Vec::with_capacity(n_files as usize);
        for _ in 0..n_files {
            let rec = table.get(o..o + 20).ok_or_else(bad)?;
            o += 20;
            let (size, start, name_idx) = if v3 {
                // u16 flags, u32 crc, u32 index, u32 size, u32 start, u16 name index
                (u32_at(rec, 10).unwrap(), u32_at(rec, 14).unwrap(), u16_at(rec, 18).unwrap() as usize)
            } else {
                // u32 crc, u32 index, u32 size, u32 start, u32 name index
                (u32_at(rec, 8).unwrap(), u32_at(rec, 12).unwrap(), u32_at(rec, 16).unwrap() as usize)
            };
            let name = names.get(name_idx).ok_or_else(bad)?.clone();
            if start as u64 + size as u64 <= file_len {
                entries.push(MegEntry { name, offset: start as u64, size });
            }
        }
        Ok(Self { path: path.to_path_buf(), entries })
    }

    pub fn read(&self, entry: &MegEntry) -> Result<Vec<u8>, String> {
        let mut f = File::open(&self.path).map_err(|e| e.to_string())?;
        f.seek(SeekFrom::Start(entry.offset)).map_err(|e| e.to_string())?;
        let mut buf = vec![0u8; entry.size as usize];
        f.read_exact(&mut buf).map_err(|e| e.to_string())?;
        Ok(buf)
    }
}
