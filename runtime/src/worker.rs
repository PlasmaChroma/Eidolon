use crate::{
    machine::Machine,
    store::{self, Store},
    *,
};
use fm1_soc::Action;
use std::{
    collections::VecDeque,
    sync::mpsc::Receiver,
    thread,
    time::{Duration, Instant},
};

struct Active {
    machine: Machine,
    package: Vec<u8>,
    hash: String,
    /// The flash as last published or loaded (an unchanged flash is not published again), and
    /// the package's own image (the published delta holds only the sectors that differ).
    saved: Vec<u8>,
    base: Vec<u8>,
    halted: bool,
    revision: u32,
    changed: Instant,
    dirty: bool,
    save_requested: bool,
    last_save: Instant,
    master: u16,
    keys: u64,
}
/// Where a load's flash comes from: the package alone, a patch's embedded delta, or (patches
/// saved before deltas were embedded) a state file in the local store.
enum StateSource {
    Fresh,
    Patch(Vec<u8>),
    Legacy(String),
}
struct Worker {
    shared: Arc<Shared>,
    store: Store,
    active: Option<Active>,
    pending: VecDeque<MidiEvent>,
    made: u64,
    host_origin: u64,
    cycle_origin: u64,
    last_view: Instant,
    speed_at: Instant,
    speed_ms: u64,
    busy: Duration,
    // Rack takes audio in bursts: a device period's worth at once (342 native frames measured
    // with WASAPI shared mode at 48 kHz; more with bigger periods). Largest frames taken between
    // two ticks, over the current and the previous 2 s window. The buffer is kept at the lead
    // plus this, so the lead is headroom after a burst, not swallowed by one.
    burst: [u64; 2],
    burst_at: Instant,
    seen_consumed: u64,
}
fn cycle_budget(elapsed_frames: u64, lead: u64, hz: u64) -> u64 {
    ((elapsed_frames.saturating_add(lead) as u128 * hz as u128 / 44100)
        .min(u64::MAX as u128)) as u64
}
impl Worker {
    fn error(&self, error: &str) {
        if let Some(a) = self.active.as_ref() {
            self.shared.message(
                if a.halted { HALTED } else { RUNNING },
                &format!("Request failed; previous firmware retained: {error}"),
            );
        } else {
            self.shared.message(ERROR, error);
        }
    }
    fn bump(&mut self) {
        self.shared.keys.store(0, Ordering::Relaxed);
        self.shared.primed.store(false, Ordering::Relaxed);
        self.shared.generation.fetch_add(1, Ordering::AcqRel);
        self.pending.clear();
        self.made = self.shared.consumed.load(Ordering::Acquire);
        self.host_origin = self.made;
        self.cycle_origin = self
            .active
            .as_ref()
            .map_or(0, |a| a.machine.soc.periph.cycles);
        self.speed_at = Instant::now();
        self.speed_ms = 0;
        self.busy = Duration::ZERO;
    }
    fn save_current(&mut self, force: bool) -> Result<(), String> {
        let Some(a) = self.active.as_mut() else {
            return Ok(());
        };
        if a.machine.soc.flash.busy() {
            return if force {
                Err("Firmware is writing flash; try again after its Save completes".into())
            } else {
                Ok(())
            };
        }
        if a.machine.soc.flash.revision != a.revision {
            a.revision = a.machine.soc.flash.revision;
            a.dirty = true;
            a.changed = Instant::now();
        }
        if !(a.dirty || a.save_requested) {
            return Ok(());
        }
        if !force && a.changed.elapsed() < Duration::from_millis(250) {
            return Ok(());
        }
        a.last_save = Instant::now();
        a.save_requested = false;
        a.dirty = false;
        // (a firmware that rewrites what is already there, as at every boot: nothing new)
        if a.machine.soc.flash.mem == a.saved {
            return Ok(());
        }
        let delta = store::encode_delta(&a.hash, &a.machine.soc.flash.mem, &a.base)?;
        *self.shared.flash_delta.lock().map_err(|_| "Flash state unavailable")? = delta;
        a.saved.copy_from_slice(&a.machine.soc.flash.mem);
        Ok(())
    }
    fn install(
        &mut self,
        package: Vec<u8>,
        expected: Option<String>,
        state: StateSource,
        name: Option<String>,
    ) -> Result<(), String> {
        let hash = sha256::hex(&package);
        if expected.as_ref().is_some_and(|e| *e != hash) {
            return Err("Firmware checksum mismatch".into());
        }
        let base = Machine::base_flash(&package)?;
        // A saved state that cannot be restored (a damaged patch, a deleted legacy file, one
        // saved over another image) does not stop the firmware: it boots fresh and says so.
        let found = match state {
            StateSource::Fresh => None,
            StateSource::Legacy(id) => Some(self.store.read_state(&hash, &id, &base)),
            StateSource::Patch(delta) => Some(store::decode_delta(&hash, &delta, &base)),
        };
        let restored = !matches!(found, Some(Err(_)));
        let saved = found.and_then(Result::ok);
        // Fully validate before creating a cache package or replacing the live machine.
        let machine = Machine::boot(
            &package,
            saved.as_deref(),
            self.shared.clock.load(Ordering::Relaxed),
        )?;
        self.save_current(true)?;
        self.store.install(&hash, &package)?;
        // (the menu's listing only: a failure to write it does not fail the load)
        let _ = self.store.write_meta(&hash, name.as_deref(), &machine.identity);
        let saved_flash = machine.soc.flash.mem.clone();
        let delta = store::encode_delta(&hash, &saved_flash, &base)?;
        *self.shared.flash_delta.lock().map_err(|_| "Flash state unavailable")? = delta;
        let revision = machine.soc.flash.revision;
        self.active = Some(Active {
            machine,
            package,
            hash: hash.clone(),
            saved: saved_flash,
            base,
            halted: false,
            revision,
            changed: Instant::now(),
            dirty: false,
            save_requested: false,
            last_save: Instant::now(),
            master: u16::MAX,
            keys: 0,
        });
        self.bump();
        self.shared.running.store(true, Ordering::Release);
        let mut v = self
            .shared
            .view
            .lock()
            .map_err(|_| "Display state unavailable")?;
        v.status.state = RUNNING;
        put(&mut v.status.firmware_sha256, &hash);
        put(&mut v.status.state_id, "");
        put(
            &mut v.status.message,
            if restored {
                "User firmware loaded"
            } else {
                "Saved flash missing or unreadable; booted fresh"
            },
        );
        v.status.realtime_speed = 0.;
        v.status.leds = [0.; 41];
        v.lcd.fill(0);
        v.status.lcd_revision += 1;
        Ok(())
    }
    fn reboot(&mut self, mhz: u32) -> Result<(), String> {
        if let Some(a) = self.active.as_mut() {
            if a.machine.soc.flash.busy() {
                return Err("Wait for the firmware to finish writing flash before reset".into());
            }
            let machine = Machine::boot(&a.package, Some(&a.machine.soc.flash.mem), mhz)?;
            a.machine = machine;
            a.revision = a.machine.soc.flash.revision;
            a.halted = false;
            a.master = u16::MAX;
            a.keys = 0;
            self.bump();
            self.shared.running.store(true, Ordering::Release);
            self.shared
                .message(RUNNING, "Emulated power cycle; local flash retained");
        }
        self.shared.clock.store(mhz, Ordering::Relaxed);
        Ok(())
    }
    fn command(&mut self, c: Command) -> Result<(), String> {
        match c {
            Command::LoadPath(path) => {
                self.shared
                    .message(LOADING, "Validating user-supplied firmware");
                let package = self.store.read_user(&path)?;
                let name = path.file_name().and_then(|n| n.to_str()).map(str::to_owned);
                self.install(package, None, StateSource::Fresh, name)?;
            }
            Command::LoadCached(hash, id) => {
                self.shared
                    .message(LOADING, "Restoring user-local firmware and flash");
                let p = self.store.read_package(&hash)?;
                let source = if id.is_empty() { StateSource::Fresh } else { StateSource::Legacy(id) };
                self.install(p, Some(hash), source, None)?;
            }
            Command::LoadPatch(hash, delta) => {
                self.shared
                    .message(LOADING, "Restoring firmware with the patch's flash");
                let p = self.store.read_package(&hash)?;
                self.install(p, Some(hash), StateSource::Patch(delta), None)?;
            }
            Command::Unload => {
                self.save_current(true)?;
                self.active = None;
                if let Ok(mut d) = self.shared.flash_delta.lock() {
                    d.clear();
                }
                self.bump();
                self.shared.running.store(false, Ordering::Release);
                let mut v = self
                    .shared
                    .view
                    .lock()
                    .map_err(|_| "Display state unavailable")?;
                v.status = Fm1Status::default();
                v.lcd.fill(0);
                put(&mut v.status.message, "No firmware loaded");
            }
            Command::Reset => self.reboot(self.shared.clock.load(Ordering::Relaxed))?,
            Command::Save => {
                if let Some(a) = self.active.as_mut() {
                    a.save_requested = true;
                }
            }
            Command::Options(clock, lead) => {
                if clock != self.shared.clock.load(Ordering::Relaxed) {
                    self.reboot(clock)?;
                }
                self.shared.lead.store(lead, Ordering::Relaxed);
            }
            Command::Encoder(enc, clicks) => {
                if let Some(a) = self.active.as_mut() {
                    a.machine.soc.periph.apply(&Action::Enc { enc, clicks });
                }
            }
        }
        Ok(())
    }
    fn tick(&mut self) -> Result<(), String> {
        let gen = self.shared.generation.load(Ordering::Acquire);
        // This queue is consumed only here. Bound the pending staging list too.
        for _ in 0..MIDI_CAP {
            if self.pending.len() >= MIDI_CAP {
                break;
            }
            let Some(e) = self.shared.midi.pop() else {
                break;
            };
            if e.generation != gen {
                continue;
            }
            let index = self
                .pending
                .iter()
                .position(|p| p.at > e.at)
                .unwrap_or(self.pending.len());
            self.pending.insert(index, e);
        }
        self.made = self.made.max(self.shared.consumed.load(Ordering::Acquire));
        let Some(a) = self.active.as_mut() else {
            self.pending.clear();
            thread::sleep(Duration::from_millis(2));
            return Ok(());
        };
        if a.halted {
            thread::sleep(Duration::from_millis(2));
            return Ok(());
        };
        let keys = self.shared.keys.load(Ordering::Relaxed);
        let mut changed_keys = keys ^ a.keys;
        while changed_keys != 0 {
            let id = changed_keys.trailing_zeros() as u8;
            let bit = 1u64 << id;
            a.machine.soc.periph.apply(&Action::Key {
                id,
                down: keys & bit != 0,
            });
            changed_keys &= !bit;
        }
        a.keys = keys;
        let master = self.shared.master.load(Ordering::Relaxed) as u16;
        if master != a.master {
            a.master = master;
            a.machine.soc.periph.apply(&Action::Adc {
                ch: 4,
                value: master,
            });
        }
        let lead = self.shared.lead.load(Ordering::Relaxed);
        let consumed = self.shared.consumed.load(Ordering::Acquire);
        if self.shared.primed.load(Ordering::Relaxed) {
            self.burst[0] = self.burst[0].max(consumed.saturating_sub(self.seen_consumed));
        }
        self.seen_consumed = consumed;
        if self.burst_at.elapsed() >= Duration::from_secs(2) {
            self.burst = [0, self.burst[0]];
            self.burst_at = Instant::now();
        }
        let target = lead as u64 + self.burst[0].max(self.burst[1]).min(AUDIO_CAP as u64 / 2);
        let need_audio = (self.shared.audio.len() as u64) < target;
        // Audio backpressure alone is insufficient before the DAC starts, or
        // for MIDI/control-only programs. Bound firmware time against Rack's
        // monotonic consumed-frame clock even when no sample is produced.
        // Check between finite instruction batches (not a cycle-exact limit).
        let host_elapsed = self
            .shared
            .consumed
            .load(Ordering::Acquire)
            .saturating_sub(self.host_origin);
        let have_time = a
            .machine
            .soc
            .periph
            .cycles
            .saturating_sub(self.cycle_origin)
            < cycle_budget(host_elapsed, target, a.machine.soc.periph.hz);
        if need_audio && have_time {
            while self.pending.front().is_some_and(|e| e.at <= self.made) {
                let e = *self.pending.front().unwrap();
                if !a.machine.soc.periph.midi_ready()
                    || a.machine.soc.periph.midi_queued() + e.len as usize > 3125
                {
                    break;
                }
                self.pending.pop_front();
                a.machine.midi(&e.bytes[..e.len as usize]);
            }
            let start = Instant::now();
            let result = catch_unwind(AssertUnwindSafe(|| a.machine.run(20_000)));
            self.busy += start.elapsed();
            let error = match result {
                Ok(Ok(())) => None,
                Ok(Err(e)) => Some(e),
                Err(_) => Some("Emulation trapped; reset or load another firmware".into()),
            };
            if let Some(e) = error {
                a.halted = true;
                self.shared.running.store(false, Ordering::Release);
                self.bump();
                self.shared.message(HALTED, &e);
                return Ok(());
            }
            for f in a.machine.soc.periph.audio.chunks_exact(2) {
                self.made = self.made.max(self.shared.consumed.load(Ordering::Acquire));
                let packet = AudioFrame {
                    at: self.made,
                    generation: gen,
                    samples: [f[0] as f32 / 32768., f[1] as f32 / 32768.],
                };
                if self.shared.audio.push(packet) {
                    self.made += 1;
                } else {
                    break;
                }
            }
            a.machine.soc.periph.audio.clear();
        } else {
            thread::sleep(Duration::from_micros(500));
        }
        if a.machine.soc.flash.revision != a.revision {
            a.revision = a.machine.soc.flash.revision;
            a.dirty = true;
            a.changed = Instant::now();
        }
        if self.last_view.elapsed() >= Duration::from_millis(33) {
            self.last_view = Instant::now();
            let mut v = self
                .shared
                .view
                .lock()
                .map_err(|_| "Display state unavailable")?;
            v.lcd.copy_from_slice(&a.machine.soc.lcd.fb);
            v.status.lcd_revision = v.status.lcd_revision.wrapping_add(1);
            v.status.leds = a.machine.soc.periph.led_take();
            if self.speed_at.elapsed() >= Duration::from_secs(1) {
                let now = a.machine.soc.periph.ms();
                if self.busy > Duration::ZERO {
                    v.status.realtime_speed =
                        now.saturating_sub(self.speed_ms) as f32 / self.busy.as_secs_f32() / 1000.;
                }
                self.speed_ms = now;
                self.speed_at = Instant::now();
                self.busy = Duration::ZERO;
            }
        }
        let should_save = a.save_requested || a.last_save.elapsed() >= Duration::from_secs(2);
        if should_save {
            self.save_current(false)?;
        }
        Ok(())
    }
    fn finish(&mut self) {
        // Drain an in-progress device Save, but never prevent shutdown forever.
        let deadline = Instant::now() + Duration::from_millis(750);
        if let Some(a) = self.active.as_mut() {
            while !a.halted
                && Instant::now() < deadline
                && (a.machine.soc.flash.busy() || a.changed.elapsed() < Duration::from_millis(250))
            {
                if catch_unwind(AssertUnwindSafe(|| a.machine.run(20_000)))
                    .ok()
                    .and_then(Result::ok)
                    .is_none()
                {
                    break;
                }
                a.machine.soc.periph.audio.clear();
                if a.machine.soc.flash.revision != a.revision {
                    a.revision = a.machine.soc.flash.revision;
                    a.dirty = true;
                    a.changed = Instant::now();
                }
                thread::sleep(Duration::from_millis(1));
            }
        }
        let _ = self.save_current(false);
    }
}
/// The machine thread keeps a small audio lead, so it must wake on time and not be preempted
/// for long. While the worker lives on Windows: 1 ms timer resolution (a plain thread's short
/// sleeps can otherwise end on the 15.6 ms scheduler tick), and the thread joins the multimedia
/// class "Pro Audio" (MMCSS), as audio threads do.
#[cfg(windows)]
struct Realtime(isize);
#[cfg(windows)]
mod realtime_ffi {
    #[link(name = "winmm")]
    extern "system" {
        pub fn timeBeginPeriod(ms: u32) -> u32;
        pub fn timeEndPeriod(ms: u32) -> u32;
    }
    #[link(name = "avrt")]
    extern "system" {
        pub fn AvSetMmThreadCharacteristicsW(task: *const u16, index: *mut u32) -> isize;
        pub fn AvRevertMmThreadCharacteristics(handle: isize) -> i32;
    }
}
#[cfg(windows)]
impl Realtime {
    fn enter() -> Self {
        let task: Vec<u16> = "Pro Audio".encode_utf16().chain(Some(0)).collect();
        let mut index = 0u32;
        unsafe {
            realtime_ffi::timeBeginPeriod(1);
            Realtime(realtime_ffi::AvSetMmThreadCharacteristicsW(task.as_ptr(), &mut index))
        }
    }
}
#[cfg(windows)]
impl Drop for Realtime {
    fn drop(&mut self) {
        unsafe {
            if self.0 != 0 {
                realtime_ffi::AvRevertMmThreadCharacteristics(self.0);
            }
            realtime_ffi::timeEndPeriod(1);
        }
    }
}
pub fn run(shared: Arc<Shared>, commands: Receiver<Command>, store: Store) {
    #[cfg(windows)]
    let _realtime = Realtime::enter();
    let mut w = Worker {
        shared,
        store,
        active: None,
        pending: VecDeque::with_capacity(MIDI_CAP),
        made: 0,
        host_origin: 0,
        cycle_origin: 0,
        last_view: Instant::now(),
        speed_at: Instant::now(),
        speed_ms: 0,
        busy: Duration::ZERO,
        burst: [0; 2],
        burst_at: Instant::now(),
        seen_consumed: 0,
    };
    while !w.shared.quit.load(Ordering::Acquire) {
        for _ in 0..64 {
            let Ok(c) = commands.try_recv() else { break };
            match catch_unwind(AssertUnwindSafe(|| w.command(c))) {
                Ok(Ok(())) => {}
                Ok(Err(e)) => w.error(&e),
                Err(_) => w.error("Firmware/control operation failed; no image installed"),
            }
        }
        match catch_unwind(AssertUnwindSafe(|| w.tick())) {
            Ok(Ok(())) => {}
            Ok(Err(e)) => {
                w.error(&e);
                thread::sleep(Duration::from_millis(20));
            }
            Err(_) => {
                w.shared.running.store(false, Ordering::Release);
                w.shared
                    .message(HALTED, "Runtime fault; unload or reset this instance");
                if let Some(a) = w.active.as_mut() {
                    a.halted = true;
                }
                w.bump();
            }
        }
    }
    let _ = catch_unwind(AssertUnwindSafe(|| w.finish()));
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn silent_machine_budget_tracks_host_frames() {
        let initial = cycle_budget(0, 512, 240_000_000);
        assert!(initial > 0 && initial < 3_000_000);
        assert_eq!(
            cycle_budget(44_100, 512, 240_000_000),
            initial + 240_000_000
        );
        assert_eq!(cycle_budget(0, 512, 240_000_000), initial);
        assert_eq!(cycle_budget(u64::MAX, 4096, 400_000_000), u64::MAX);
    }
}
