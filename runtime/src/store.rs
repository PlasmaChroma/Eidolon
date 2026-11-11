//! Local-only package/flash store. No bytes from here are returned to Rack
//! serialization; metadata uses only lowercase hex content/session identifiers.
use crate::sha256;
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};
pub const MAX_PACKAGE: usize = 16 * 1024 * 1024;
const FLASH_SIZE: usize = 1 << 20;
const HEADER: usize = 8 + 4 + 64 + 32;
/// FM1RFL02 (a flash delta): only the 4 KiB sectors that differ from the package's own flash
/// image (the base the machine boots from), rebuilt on load. Embedded in Rack patches; older
/// local state files use it too. magic, flash size, firmware hash (hex), base
/// SHA-256, rebuilt-flash SHA-256, sector size, sector count, then (offset, sector) pairs.
const SECTOR: usize = 4096;
const HEADER2: usize = 8 + 4 + 64 + 32 + 32 + 4 + 4;
pub const MAX_DELTA: usize = HEADER2 + (FLASH_SIZE / SECTOR) * (4 + SECTOR);
static SERIAL: AtomicU64 = AtomicU64::new(0);
pub fn valid_id(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
pub fn session_id() -> String {
    sha256::hex(
        format!(
            "{}-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos(),
            SERIAL.fetch_add(1, Ordering::Relaxed)
        )
        .as_bytes(),
    )
}
fn read_bounded(path: &Path, max: usize) -> Result<Vec<u8>, String> {
    let f = fs::File::open(path).map_err(|e| format!("Cannot open local file: {e}"))?;
    if f.metadata().map_err(|e| e.to_string())?.len() > max as u64 {
        return Err("Local file exceeds size limit".into());
    }
    let mut data = Vec::new();
    f.take(max as u64 + 1)
        .read_to_end(&mut data)
        .map_err(|e| e.to_string())?;
    if data.len() > max {
        return Err("Local file exceeds size limit".into());
    }
    Ok(data)
}
#[derive(Clone)]
pub struct Store {
    root: PathBuf,
}
impl Store {
    pub fn new(root: PathBuf) -> Result<Self, String> {
        if !root.is_absolute() {
            return Err("The cache root must be an absolute local path".into());
        }
        fs::create_dir_all(root.join("firmware")).map_err(|e| e.to_string())?;
        fs::create_dir_all(root.join("state")).map_err(|e| e.to_string())?;
        Ok(Self { root })
    }
    pub fn read_user(&self, path: &Path) -> Result<Vec<u8>, String> {
        read_bounded(path, MAX_PACKAGE)
    }
    pub fn read_package(&self, hash: &str) -> Result<Vec<u8>, String> {
        if !valid_id(hash) {
            return Err("Invalid firmware identifier".into());
        }
        let data = read_bounded(
            &self.root.join("firmware").join(format!("{hash}.fwsc")),
            MAX_PACKAGE,
        )
        .map_err(|_| {
            "Required firmware is not available locally; import the exact user-supplied package"
                .to_owned()
        })?;
        if sha256::hex(&data) != hash {
            return Err("Cached firmware checksum mismatch; reimport the original package".into());
        }
        Ok(data)
    }
    /// Call only AFTER the parser and the machine validate this package.
    pub fn install(&self, hash: &str, data: &[u8]) -> Result<(), String> {
        if !valid_id(hash) || data.len() > MAX_PACKAGE || sha256::hex(data) != hash {
            return Err("Invalid package import".into());
        }
        let p = self.root.join("firmware").join(format!("{hash}.fwsc"));
        if p.exists() && self.read_package(hash).is_ok() {
            return Ok(());
        }
        atomic_write(&p, data)
    }
    /// `firmware/<hash>.json` beside the cached package, for the plugin's "Load cached firmware"
    /// menu: the file name it was imported from (`name`, given on an import from a path) and the
    /// package's identity. Without a name, a missing file is created with the identity as the
    /// name (packages cached before this existed) and an existing one is left alone.
    pub fn write_meta(&self, hash: &str, name: Option<&str>, identity: &str) -> Result<(), String> {
        if !valid_id(hash) {
            return Err("Invalid firmware identifier".into());
        }
        let p = self.root.join("firmware").join(format!("{hash}.json"));
        if name.is_none() && p.exists() {
            return Ok(());
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let text = format!(
            "{{\n  \"schema\": 1,\n  \"name\": {},\n  \"identity\": {},\n  \"imported\": {now}\n}}\n",
            json_string(name.unwrap_or(identity)),
            json_string(identity)
        );
        atomic_write(&p, text.as_bytes())
    }
    fn state_path(&self, hash: &str, id: &str) -> Result<PathBuf, String> {
        if !valid_id(hash) || !valid_id(id) {
            return Err("Invalid local state reference".into());
        }
        Ok(self
            .root
            .join("state")
            .join(hash)
            .join(format!("{id}.flash")))
    }
    /// A saved flash state, whole: FM1RFL01 (the whole flash) or FM1RFL02 (sectors over `base`,
    /// the package's own flash image; a state saved over a different base is refused rather
    /// than rebuilt wrong).
    pub fn read_state(&self, hash: &str, id: &str, base: &[u8]) -> Result<Vec<u8>, String> {
        let data = read_bounded(&self.state_path(hash, id)?, MAX_DELTA.max(HEADER + FLASH_SIZE))
            .map_err(|_| {
                "Referenced local flash state is missing; firmware alone cannot restore saved edits"
                    .to_owned()
            })?;
        let invalid = || "Local flash state header is invalid".to_owned();
        if data.len() < 76
            || u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize != FLASH_SIZE
            || &data[12..76] != hash.as_bytes()
        {
            return Err(invalid());
        }
        match &data[..8] {
            b"FM1RFL01" => {
                if data.len() != HEADER + FLASH_SIZE {
                    return Err(invalid());
                }
                if sha256::digest(&data[HEADER..]) != data[76..108] {
                    return Err("Local flash state checksum mismatch".into());
                }
                Ok(data[HEADER..].to_vec())
            }
            b"FM1RFL02" => decode_delta(hash, &data, base),
            _ => Err(invalid()),
        }
    }
}
/// `flash` as FM1RFL02: the sectors that differ from `base`.
pub fn encode_delta(hash: &str, flash: &[u8], base: &[u8]) -> Result<Vec<u8>, String> {
    if !valid_id(hash) || flash.len() != FLASH_SIZE || base.len() != FLASH_SIZE {
        return Err("Unexpected flash state".into());
    }
    let changed: Vec<usize> = (0..FLASH_SIZE / SECTOR)
        .map(|s| s * SECTOR)
        .filter(|&o| flash[o..o + SECTOR] != base[o..o + SECTOR])
        .collect();
    let mut data = Vec::with_capacity(HEADER2 + changed.len() * (4 + SECTOR));
    data.extend_from_slice(b"FM1RFL02");
    data.extend_from_slice(&(FLASH_SIZE as u32).to_le_bytes());
    data.extend_from_slice(hash.as_bytes());
    data.extend_from_slice(&sha256::digest(base));
    data.extend_from_slice(&sha256::digest(flash));
    data.extend_from_slice(&(SECTOR as u32).to_le_bytes());
    data.extend_from_slice(&(changed.len() as u32).to_le_bytes());
    for o in changed {
        data.extend_from_slice(&(o as u32).to_le_bytes());
        data.extend_from_slice(&flash[o..o + SECTOR]);
    }
    Ok(data)
}
/// The whole flash from an FM1RFL02 delta over `base`, checked: for this firmware, over this
/// base (a delta saved over another image is refused rather than rebuilt wrong), intact.
pub fn decode_delta(hash: &str, data: &[u8], base: &[u8]) -> Result<Vec<u8>, String> {
    let invalid = || "Flash state header is invalid".to_owned();
    if data.len() < HEADER2
        || &data[..8] != b"FM1RFL02"
        || u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize != FLASH_SIZE
        || &data[12..76] != hash.as_bytes()
        || base.len() != FLASH_SIZE
    {
        return Err(invalid());
    }
    if sha256::digest(base) != data[76..108] {
        return Err("Flash state was saved over a different package image".into());
    }
    let word = |at: usize| u32::from_le_bytes(data[at..at + 4].try_into().unwrap()) as usize;
    let (size, count) = (word(140), word(144));
    if size != SECTOR || count > FLASH_SIZE / SECTOR || data.len() != HEADER2 + count * (4 + SECTOR) {
        return Err(invalid());
    }
    let mut flash = base.to_vec();
    for i in 0..count {
        let at = HEADER2 + i * (4 + SECTOR);
        let off = word(at);
        if off % SECTOR != 0 || off + SECTOR > FLASH_SIZE {
            return Err(invalid());
        }
        flash[off..off + SECTOR].copy_from_slice(&data[at + 4..at + 4 + SECTOR]);
    }
    if sha256::digest(&flash) != data[108..140] {
        return Err("Flash state checksum mismatch".into());
    }
    Ok(flash)
}
fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}
fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let temp = path.with_extension(format!("{}.tmp", &session_id()[..16]));
    let result = (|| {
        let mut f = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)
            .map_err(|e| e.to_string())?;
        f.write_all(bytes)
            .and_then(|_| f.sync_all())
            .map_err(|e| e.to_string())?;
        drop(f);
        replace(&temp, path).map_err(|e| e.to_string())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}
#[cfg(not(windows))]
fn replace(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::rename(from, to)
}
#[cfg(windows)]
fn replace(from: &Path, to: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
    }
    let from: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let to: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 1 | 8) } == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ids_no_traversal() {
        assert!(valid_id(&"a".repeat(64)));
        for s in ["..", "/tmp/x", "AA", "a/b", ""] {
            assert!(!valid_id(s));
        }
    }
    #[test]
    fn delta_roundtrip_and_integrity() {
        let hash = "a".repeat(64);
        let base = vec![0xffu8; FLASH_SIZE];
        let mut flash = base.clone();
        flash[5 * SECTOR + 7] = 0x5a;
        flash[FLASH_SIZE - 1] = 0x01;
        let d = encode_delta(&hash, &flash, &base).unwrap();
        assert_eq!(d.len(), HEADER2 + 2 * (4 + SECTOR));
        assert_eq!(decode_delta(&hash, &d, &base).unwrap(), flash);
        let empty = encode_delta(&hash, &base, &base).unwrap();
        assert_eq!(empty.len(), HEADER2);
        assert_eq!(decode_delta(&hash, &empty, &base).unwrap(), base);
        assert!(decode_delta(&"b".repeat(64), &d, &base).is_err());
        // a different base is refused, not rebuilt wrong
        assert!(decode_delta(&hash, &d, &vec![0u8; FLASH_SIZE]).is_err());
        let mut corrupt = d.clone();
        let last = corrupt.len() - 1;
        corrupt[last] ^= 1;
        assert!(decode_delta(&hash, &corrupt, &base).is_err());
        assert!(decode_delta(&hash, &d[..d.len() - 1], &base).is_err());
        // a legacy local state file in the same format still reads
        let dir = std::env::temp_dir().join(format!("fm1-rack-test-{}", &session_id()[..16]));
        let s = Store::new(dir.clone()).unwrap();
        let id = session_id();
        let p = s.state_path(&hash, &id).unwrap();
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(&p, &d).unwrap();
        assert_eq!(s.read_state(&hash, &id, &base).unwrap(), flash);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn reads_whole_flash_format() {
        let dir = std::env::temp_dir().join(format!("fm1-rack-v1-{}", &session_id()[..16]));
        let s = Store::new(dir.clone()).unwrap();
        let (hash, id) = ("c".repeat(64), session_id());
        let flash = vec![0x33u8; FLASH_SIZE];
        let p = s.state_path(&hash, &id).unwrap();
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        let mut data = b"FM1RFL01".to_vec();
        data.extend_from_slice(&(FLASH_SIZE as u32).to_le_bytes());
        data.extend_from_slice(hash.as_bytes());
        data.extend_from_slice(&sha256::digest(&flash));
        data.extend_from_slice(&flash);
        fs::write(&p, data).unwrap();
        assert_eq!(s.read_state(&hash, &id, &[]).unwrap(), flash);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn oversized_package_rejected_before_read() {
        let dir = std::env::temp_dir().join(format!("fm1-rack-size-{}", &session_id()[..16]));
        let s = Store::new(dir.clone()).unwrap();
        let p = dir.join("synthetic-oversize.dat");
        let f = fs::File::create(&p).unwrap();
        f.set_len(MAX_PACKAGE as u64 + 1).unwrap();
        drop(f);
        assert!(s.read_user(&p).unwrap_err().contains("size limit"));
        assert_eq!(fs::read_dir(dir.join("firmware")).unwrap().count(), 0);
        fs::remove_dir_all(dir).unwrap();
    }
}
