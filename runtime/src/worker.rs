use crate::{
    machine::Machine,
    store::{session_id, Store},
    *,
};
use fm1_soc::Action;
use std::{
    collections::VecDeque,
    sync::mpsc::{self, Receiver, Sender},
    thread,
    time::{Duration, Instant},
};

struct Active {
    machine: Machine,
    package: Vec<u8>,
    hash: String,
    id: String,
    halted: bool,
    revision: u32,
    changed: Instant,
    dirty: bool,
    save_requested: bool,
    last_save: Instant,
    master: u16,
    keys: u64,
}
// Flash snapshots are hashed, written and fsynced here, off the machine
// thread, so a save never stalls emulation. One FIFO thread: writes for the
// same state land in submission order; a backlog for one state is coalesced.
struct Saver {
    tx: Option<Sender<(String, String, Box<[u8]>)>>,
    thread: Option<thread::JoinHandle<()>>,
}
impl Saver {
    fn new(shared: Arc<Shared>, store: Store) -> Self {
        let (tx, rx) = mpsc::channel::<(String, String, Box<[u8]>)>();
        let thread = thread::Builder::new()
            .name("fm1-rack-save".into())
            .spawn(move || {
                while let Ok(first) = rx.recv() {
                    let mut batch = vec![first];
                    batch.extend(rx.try_iter());
                    for (i, (hash, id, flash)) in batch.iter().enumerate() {
                        if batch[i + 1..].iter().any(|(h, d, _)| h == hash && d == id) {
                            continue; // superseded by a newer snapshot of the same state
                        }
                        if let Err(e) = store.save_state(hash, id, flash) {
                            if let Ok(mut v) = shared.view.lock() {
                                put(&mut v.status.message, &format!("Local flash save failed: {e}"));
                            }
                        }
                    }
                }
            })
            .ok();
        Self { tx: Some(tx), thread }
    }
    fn submit(&self, hash: &str, id: &str, flash: &[u8]) -> Result<(), String> {
        self.tx
            .as_ref()
            .and_then(|tx| tx.send((hash.to_owned(), id.to_owned(), flash.into())).ok())
            .ok_or_else(|| "Local flash saver is not running".to_owned())
    }
    /// Writes everything submitted so far, then stops the thread.
    fn finish(&mut self) {
        self.tx = None;
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}
struct Worker {
    shared: Arc<Shared>,
    store: Store,
    saver: Saver,
    active: Option<Active>,
    pending: VecDeque<MidiEvent>,
    made: u64,
    host_origin: u64,
    cycle_origin: u64,
    last_view: Instant,
    speed_at: Instant,
    speed_ms: u64,
    busy: Duration,
}
fn cycle_budget(elapsed_frames: u64, lead: u32, hz: u64) -> u64 {
    ((elapsed_frames.saturating_add(lead as u64) as u128 * hz as u128 / 44100)
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
        self.saver
            .submit(&a.hash, &a.id, &a.machine.soc.flash.mem)?;
        a.dirty = false;
        Ok(())
    }
    fn install(
        &mut self,
        package: Vec<u8>,
        expected: Option<String>,
        state: Option<String>,
    ) -> Result<(), String> {
        let hash = sha256::hex(&package);
        if expected.as_ref().is_some_and(|e| *e != hash) {
            return Err("Firmware checksum mismatch".into());
        }
        let saved = state
            .as_ref()
            .filter(|s| !s.is_empty())
            .map(|s| self.store.read_state(&hash, s))
            .transpose()?;
        // Fully validate before creating a cache package or replacing the live machine.
        let machine = Machine::boot(
            &package,
            saved.as_deref(),
            self.shared.clock.load(Ordering::Relaxed),
        )?;
        self.save_current(true)?;
        let id = session_id();
        self.store.install(&hash, &package)?;
        self.store.save_state(&hash, &id, &machine.soc.flash.mem)?;
        let revision = machine.soc.flash.revision;
        self.active = Some(Active {
            machine,
            package,
            hash: hash.clone(),
            id: id.clone(),
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
        put(&mut v.status.state_id, &id);
        put(
            &mut v.status.message,
            "User firmware loaded; local flash state only",
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
                self.install(package, None, None)?;
            }
            Command::LoadCached(hash, id) => {
                self.shared
                    .message(LOADING, "Restoring user-local firmware and flash");
                let p = self.store.read_package(&hash)?;
                self.install(p, Some(hash), Some(id))?;
            }
            Command::Unload => {
                self.save_current(true)?;
                self.active = None;
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
        let need_audio = self.shared.audio.len() < (lead as usize);
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
            < cycle_budget(host_elapsed, lead, a.machine.soc.periph.hz);
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
        self.saver.finish();
    }
}
pub fn run(shared: Arc<Shared>, commands: Receiver<Command>, store: Store) {
    let saver = Saver::new(shared.clone(), store.clone());
    let mut w = Worker {
        shared,
        store,
        saver,
        active: None,
        pending: VecDeque::with_capacity(MIDI_CAP),
        made: 0,
        host_origin: 0,
        cycle_origin: 0,
        last_view: Instant::now(),
        speed_at: Instant::now(),
        speed_ms: 0,
        busy: Duration::ZERO,
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
