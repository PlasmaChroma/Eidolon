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
    pub fn read_state(&self, hash: &str, id: &str) -> Result<Vec<u8>, String> {
        let data =
            read_bounded(&self.state_path(hash, id)?, HEADER + FLASH_SIZE).map_err(|_| {
                "Referenced local flash state is missing; firmware alone cannot restore saved edits"
                    .to_owned()
            })?;
        if data.len() != HEADER + FLASH_SIZE
            || &data[..8] != b"FM1RFL01"
            || u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize != FLASH_SIZE
            || &data[12..76] != hash.as_bytes()
        {
            return Err("Local flash state header is invalid".into());
        }
        if sha256::digest(&data[HEADER..]) != data[76..108] {
            return Err("Local flash state checksum mismatch".into());
        }
        Ok(data[HEADER..].to_vec())
    }
    pub fn save_state(&self, hash: &str, id: &str, flash: &[u8]) -> Result<(), String> {
        if flash.len() != FLASH_SIZE {
            return Err("Unexpected flash size".into());
        }
        let p = self.state_path(hash, id)?;
        fs::create_dir_all(p.parent().unwrap()).map_err(|e| e.to_string())?;
        let mut data = Vec::with_capacity(HEADER + FLASH_SIZE);
        data.extend_from_slice(b"FM1RFL01");
        data.extend_from_slice(&(FLASH_SIZE as u32).to_le_bytes());
        data.extend_from_slice(hash.as_bytes());
        data.extend_from_slice(&sha256::digest(flash));
        data.extend_from_slice(flash);
        atomic_write(&p, &data)
    }
}
fn atomic_write(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let temp = path.with_extension(format!("{}.tmp", session_id()));
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
    fn roundtrip_integrity_and_fork_ids() {
        let dir = std::env::temp_dir().join(format!("fm1-rack-test-{}", session_id()));
        let s = Store::new(dir.clone()).unwrap();
        let hash = "a".repeat(64);
        let id = session_id();
        let data = vec![0x5a; FLASH_SIZE];
        s.save_state(&hash, &id, &data).unwrap();
        assert_eq!(s.read_state(&hash, &id).unwrap(), data);
        let data2 = vec![0xa5; FLASH_SIZE];
        s.save_state(&hash, &id, &data2).unwrap();
        assert_eq!(s.read_state(&hash, &id).unwrap(), data2);
        assert_ne!(id, session_id());
        assert!(s.read_state(&"b".repeat(64), &id).is_err());
        let p = s.state_path(&hash, &id).unwrap();
        let mut corrupt = fs::read(&p).unwrap();
        corrupt[HEADER + 19] ^= 1;
        fs::write(&p, corrupt).unwrap();
        assert!(s.read_state(&hash, &id).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn oversized_package_rejected_before_read() {
        let dir = std::env::temp_dir().join(format!("fm1-rack-size-{}", session_id()));
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
