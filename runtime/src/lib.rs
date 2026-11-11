//! Native Rack bridge; no firmware, ROM, presets, downloads or network client.
//! The emulated machine belongs exclusively to one worker. The DSP thread
//! exchanges fixed-size messages and frames through bounded SPSC rings.
mod machine;
mod ring;
mod sha256;
mod store;
mod worker;

use ring::Ring;
use std::{
    ffi::{c_char, CStr},
    panic::{catch_unwind, AssertUnwindSafe},
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
        mpsc::{self, SyncSender},
        Arc, Mutex,
    },
    thread::JoinHandle,
};

const PIXELS: usize = 240 * 240;
const AUDIO_CAP: usize = 16384;
const MIDI_CAP: usize = 4096;
const ABI: u32 = 1;
pub const EMPTY: u32 = 0;
pub const LOADING: u32 = 1;
pub const RUNNING: u32 = 2;
pub const ERROR: u32 = 3;
pub const HALTED: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Fm1Status {
    pub abi_version: u32,
    pub state: u32,
    pub lead_frames: u32,
    pub clock_mhz: u32,
    pub generation: u64,
    pub audio_frame: u64,
    pub underrun_frames: u64,
    pub dropped_events: u64,
    pub lcd_revision: u64,
    pub realtime_speed: f32,
    pub leds: [f32; 41],
    pub firmware_sha256: [c_char; 65],
    pub state_id: [c_char; 65],
    pub message: [c_char; 256],
}
impl Default for Fm1Status {
    fn default() -> Self {
        Self {
            abi_version: ABI,
            state: EMPTY,
            lead_frames: 1024,
            clock_mhz: 240,
            generation: 1,
            audio_frame: 0,
            underrun_frames: 0,
            dropped_events: 0,
            lcd_revision: 0,
            realtime_speed: 0.,
            leds: [0.; 41],
            firmware_sha256: [0; 65],
            state_id: [0; 65],
            message: [0; 256],
        }
    }
}
fn put<const N: usize>(dst: &mut [c_char; N], text: &str) {
    dst.fill(0);
    for (d, b) in dst.iter_mut().take(N - 1).zip(text.bytes()) {
        *d = b as c_char;
    }
}
#[derive(Clone, Copy, Default)]
struct AudioFrame {
    at: u64,
    generation: u64,
    samples: [f32; 2],
}
#[derive(Clone, Copy, Default)]
struct MidiEvent {
    at: u64,
    generation: u64,
    bytes: [u8; 3],
    len: u8,
}
struct View {
    status: Fm1Status,
    lcd: Vec<u16>,
}
struct Shared {
    audio: Ring<AudioFrame, AUDIO_CAP>,
    midi: Ring<MidiEvent, MIDI_CAP>,
    view: Mutex<View>,
    consumed: AtomicU64,
    generation: AtomicU64,
    underruns: AtomicU64,
    // Set once a frame of the current generation reached the DSP; silence
    // before the firmware starts its DAC is not an underrun.
    primed: AtomicBool,
    dropped: AtomicU64,
    keys: AtomicU64,
    master: AtomicU32,
    lead: AtomicU32,
    clock: AtomicU32,
    quit: AtomicBool,
    running: AtomicBool,
}
impl Shared {
    fn new() -> Self {
        Self {
            audio: Ring::new(),
            midi: Ring::new(),
            view: Mutex::new(View {
                status: Fm1Status::default(),
                lcd: vec![0; PIXELS],
            }),
            consumed: AtomicU64::new(0),
            generation: AtomicU64::new(1),
            underruns: AtomicU64::new(0),
            primed: AtomicBool::new(false),
            dropped: AtomicU64::new(0),
            keys: AtomicU64::new(0),
            master: AtomicU32::new(1023),
            lead: AtomicU32::new(1024),
            clock: AtomicU32::new(240),
            quit: AtomicBool::new(false),
            running: AtomicBool::new(false),
        }
    }
    fn message(&self, state: u32, message: &str) {
        if let Ok(mut v) = self.view.lock() {
            v.status.state = state;
            put(&mut v.status.message, message);
        }
    }
}
enum Command {
    LoadPath(PathBuf),
    LoadCached(String, String),
    Unload,
    Reset,
    Save,
    Options(u32, u32),
    Encoder(u8, i32),
}

pub struct Fm1Handle {
    shared: Arc<Shared>,
    command: SyncSender<Command>,
    worker: Option<JoinHandle<()>>,
}
fn guarded<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}
unsafe fn string(p: *const c_char) -> Option<String> {
    if p.is_null() {
        None
    } else {
        CStr::from_ptr(p)
            .to_str()
            .ok()
            .filter(|s| s.len() <= 32768)
            .map(str::to_owned)
    }
}
unsafe fn send(h: *mut Fm1Handle, c: Command) -> i32 {
    h.as_ref().is_some_and(|h| h.command.try_send(c).is_ok()) as i32
}

#[no_mangle]
pub extern "C" fn fm1_abi_version() -> u32 {
    ABI
}
#[no_mangle]
pub unsafe extern "C" fn fm1_create(root: *const c_char) -> *mut Fm1Handle {
    guarded(std::ptr::null_mut(), || {
        let Some(root) = string(root) else {
            return std::ptr::null_mut();
        };
        let Ok(store) = store::Store::new(PathBuf::from(root)) else {
            return std::ptr::null_mut();
        };
        let shared = Arc::new(Shared::new());
        let s = shared.clone();
        let (tx, rx) = mpsc::sync_channel(64);
        let Ok(worker) = std::thread::Builder::new()
            .name("fm1-rack-machine".into())
            .stack_size(8 * 1024 * 1024)
            .spawn(move || worker::run(s, rx, store))
        else {
            return std::ptr::null_mut();
        };
        Box::into_raw(Box::new(Fm1Handle {
            shared,
            command: tx,
            worker: Some(worker),
        }))
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_destroy(h: *mut Fm1Handle) {
    guarded((), || {
        if h.is_null() {
            return;
        }
        let mut h = Box::from_raw(h);
        h.shared.quit.store(true, Ordering::Release);
        if let Some(w) = h.worker.take() {
            let _ = w.join();
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_load_path(h: *mut Fm1Handle, path: *const c_char) -> i32 {
    guarded(0, || {
        let Some(p) = string(path) else { return 0 };
        let p = PathBuf::from(p);
        if !p.is_absolute() {
            return 0;
        }
        send(h, Command::LoadPath(p))
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_load_cached(
    h: *mut Fm1Handle,
    hash: *const c_char,
    id: *const c_char,
) -> i32 {
    guarded(0, || {
        let (Some(hash), Some(id)) = (string(hash), string(id)) else {
            return 0;
        };
        if !store::valid_id(&hash) || (!id.is_empty() && !store::valid_id(&id)) {
            return 0;
        }
        send(h, Command::LoadCached(hash, id))
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_unload(h: *mut Fm1Handle) -> i32 {
    guarded(0, || send(h, Command::Unload))
}
#[no_mangle]
pub unsafe extern "C" fn fm1_reset(h: *mut Fm1Handle) -> i32 {
    guarded(0, || send(h, Command::Reset))
}
#[no_mangle]
pub unsafe extern "C" fn fm1_request_save(h: *mut Fm1Handle) -> i32 {
    guarded(0, || send(h, Command::Save))
}
#[no_mangle]
pub unsafe extern "C" fn fm1_set_options(h: *mut Fm1Handle, clock: u32, lead: u32) -> i32 {
    guarded(0, || {
        if !(256..=4096).contains(&lead) || ![120, 160, 240, 320, 400].contains(&clock) {
            return 0;
        }
        send(h, Command::Options(clock, lead))
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_key(h: *mut Fm1Handle, id: u32, down: i32) -> i32 {
    let Some(h) = h.as_ref() else { return 0 };
    if id >= 41 {
        return 0;
    }
    // Desired physical state does not use the bounded control queue: even
    // during a long import or save a release cannot be rejected and get stuck.
    let bit = 1u64 << id;
    if down != 0 {
        h.shared.keys.fetch_or(bit, Ordering::Relaxed);
    } else {
        h.shared.keys.fetch_and(!bit, Ordering::Relaxed);
    }
    1
}
#[no_mangle]
pub unsafe extern "C" fn fm1_encoder(h: *mut Fm1Handle, encoder: u32, detents: i32) -> i32 {
    guarded(0, || {
        if encoder < 7 && (-256..=256).contains(&detents) {
            send(h, Command::Encoder(encoder as u8, detents))
        } else {
            0
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_set_master(h: *mut Fm1Handle, value: f32) {
    if let Some(h) = h.as_ref() {
        if value.is_finite() {
            h.shared.master.store(
                (value.clamp(0., 1.) * 1023.).round() as u32,
                Ordering::Relaxed,
            );
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn fm1_audio_frame(h: *mut Fm1Handle) -> u64 {
    h.as_ref()
        .map_or(0, |h| h.shared.consumed.load(Ordering::Relaxed))
}
#[no_mangle]
pub unsafe extern "C" fn fm1_generation(h: *mut Fm1Handle) -> u64 {
    h.as_ref()
        .map_or(0, |h| h.shared.generation.load(Ordering::Acquire))
}
#[no_mangle]
pub unsafe extern "C" fn fm1_midi_push(
    h: *mut Fm1Handle,
    bytes: *const u8,
    len: u32,
    at: u64,
) -> i32 {
    let Some(h) = h.as_ref() else { return 0 };
    if bytes.is_null() || !(1..=3).contains(&len) {
        return 0;
    }
    let data = std::slice::from_raw_parts(bytes, len as usize);
    if data[0] < 0x80 {
        return 0;
    }
    let mut b = [0; 3];
    b[..len as usize].copy_from_slice(data);
    let now = h.shared.consumed.load(Ordering::Relaxed);
    let at = if at == u64::MAX {
        now.saturating_add(h.shared.lead.load(Ordering::Relaxed) as u64)
    } else {
        at
    };
    if at > now.saturating_add(441_000) {
        return 0;
    }
    let event = MidiEvent {
        at,
        generation: h.shared.generation.load(Ordering::Acquire),
        bytes: b,
        len: len as u8,
    };
    if h.shared.midi.push(event) {
        1
    } else {
        h.shared.dropped.fetch_add(1, Ordering::Relaxed);
        0
    }
}
#[no_mangle]
pub unsafe extern "C" fn fm1_audio_read(h: *mut Fm1Handle, output: *mut f32, frames: u32) -> u32 {
    if output.is_null() || frames > 4096 {
        return 0;
    }
    let out = std::slice::from_raw_parts_mut(output, frames as usize * 2);
    out.fill(0.);
    let Some(h) = h.as_ref() else { return 0 };
    let mut count = 0;
    for dest in out.chunks_exact_mut(2) {
        let at = h.shared.consumed.fetch_add(1, Ordering::Relaxed);
        let generation = h.shared.generation.load(Ordering::Acquire);
        let mut found = false;
        // Bounded stale-generation/timeline cleanup (never spin unboundedly).
        for _ in 0..64 {
            let Some(f) = h.shared.audio.peek() else {
                break;
            };
            if f.generation != generation || f.at < at {
                h.shared.audio.pop();
                continue;
            }
            if f.at == at {
                h.shared.audio.pop();
                dest.copy_from_slice(&f.samples);
                found = true;
                if !h.shared.primed.load(Ordering::Relaxed) {
                    h.shared.primed.store(true, Ordering::Relaxed);
                }
                count += 1;
            }
            break;
        }
        if !found && h.shared.running.load(Ordering::Relaxed) && h.shared.primed.load(Ordering::Relaxed) {
            h.shared.underruns.fetch_add(1, Ordering::Relaxed);
        }
    }
    count
}
#[no_mangle]
pub unsafe extern "C" fn fm1_try_get_status(h: *mut Fm1Handle, out: *mut Fm1Status) -> i32 {
    guarded(0, || {
        let (Some(h), Some(out)) = (h.as_ref(), out.as_mut()) else {
            return 0;
        };
        let Ok(v) = h.shared.view.try_lock() else {
            return 0;
        };
        *out = v.status;
        out.audio_frame = h.shared.consumed.load(Ordering::Relaxed);
        out.generation = h.shared.generation.load(Ordering::Acquire);
        out.underrun_frames = h.shared.underruns.load(Ordering::Relaxed);
        out.dropped_events = h.shared.dropped.load(Ordering::Relaxed);
        out.clock_mhz = h.shared.clock.load(Ordering::Relaxed);
        out.lead_frames = h.shared.lead.load(Ordering::Relaxed);
        1
    })
}
#[no_mangle]
pub unsafe extern "C" fn fm1_try_copy_lcd(
    h: *mut Fm1Handle,
    pixels: *mut u16,
    capacity: u32,
) -> i32 {
    guarded(0, || {
        let Some(h) = h.as_ref() else { return 0 };
        if pixels.is_null() || capacity < PIXELS as u32 {
            return 0;
        }
        let Ok(v) = h.shared.view.try_lock() else {
            return 0;
        };
        std::ptr::copy_nonoverlapping(v.lcd.as_ptr(), pixels, PIXELS);
        1
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn abi_layout() {
        assert_eq!(std::mem::size_of::<Fm1Status>(), 616);
        assert_eq!(std::mem::align_of::<Fm1Status>(), 8);
        assert_eq!(std::mem::offset_of!(Fm1Status, firmware_sha256), 224);
    }
    #[test]
    fn nulls_safe() {
        unsafe {
            let mut a = [1.; 8];
            assert_eq!(fm1_audio_read(std::ptr::null_mut(), a.as_mut_ptr(), 4), 0);
            assert_eq!(a, [0.; 8]);
            fm1_destroy(std::ptr::null_mut());
            assert_eq!(fm1_audio_frame(std::ptr::null_mut()), 0);
        }
    }
    fn handle_without_worker() -> (Box<Fm1Handle>, mpsc::Receiver<Command>) {
        let (tx, rx) = mpsc::sync_channel(64);
        (
            Box::new(Fm1Handle {
                shared: Arc::new(Shared::new()),
                command: tx,
                worker: None,
            }),
            rx,
        )
    }
    #[test]
    fn bounded_ingress_and_saturation_safe_keys() {
        let (mut h, _rx) = handle_without_worker();
        let p = &mut *h as *mut Fm1Handle;
        unsafe {
            for _ in 0..64 {
                assert_eq!(fm1_reset(p), 1);
            }
            assert_eq!(fm1_reset(p), 0);
            assert_eq!(fm1_key(p, 40, 1), 1);
            assert_eq!(h.shared.keys.load(Ordering::Relaxed), 1u64 << 40);
            assert_eq!(fm1_key(p, 40, 0), 1);
            assert_eq!(h.shared.keys.load(Ordering::Relaxed), 0);
            assert_eq!(fm1_key(p, 41, 1), 0);
            let note = [0x90, 60, 100];
            for _ in 0..MIDI_CAP {
                assert_eq!(fm1_midi_push(p, note.as_ptr(), 3, u64::MAX), 1);
            }
            assert_eq!(fm1_midi_push(p, note.as_ptr(), 3, u64::MAX), 0);
            assert_eq!(h.shared.dropped.load(Ordering::Relaxed), 1);
        }
    }
    #[test]
    fn audio_filters_old_epochs_and_keeps_timeline() {
        let (mut h, _rx) = handle_without_worker();
        let p = &mut *h as *mut Fm1Handle;
        h.shared.generation.store(2, Ordering::Release);
        assert!(h.shared.audio.push(AudioFrame {
            at: 0,
            generation: 1,
            samples: [9., 9.]
        }));
        assert!(h.shared.audio.push(AudioFrame {
            at: 0,
            generation: 2,
            samples: [0.25, -0.5]
        }));
        assert!(h.shared.audio.push(AudioFrame {
            at: 3,
            generation: 2,
            samples: [0.75, 0.5]
        }));
        unsafe {
            let mut out = [9.; 6];
            assert_eq!(fm1_audio_read(p, out.as_mut_ptr(), 3), 1);
            assert_eq!(out, [0.25, -0.5, 0., 0., 0., 0.]);
            assert_eq!(fm1_audio_frame(p), 3);
            assert_eq!(fm1_generation(p), 2);
            assert_eq!(fm1_audio_read(p, out.as_mut_ptr(), 1), 1);
            assert_eq!(&out[..2], &[0.75, 0.5]);
        }
    }
}
