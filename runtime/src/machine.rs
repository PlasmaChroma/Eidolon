use fm1_core::{Bus, Cpu};
use fm1_soc::{Action, Board, Soc};
pub const RAM_SIZE: usize = 1 << 20;
pub struct Machine {
    pub cpu: Cpu,
    pub cpu1: Option<Cpu>,
    pub soc: Soc,
    pending: bool,
}
impl Machine {
    pub fn boot(pkg: &[u8], flash: Option<&[u8]>, mhz: u32) -> Result<Self, String> {
        let st = std::panic::catch_unwind(|| fm1_soc::stock_from_package(pkg, None))
            .map_err(|_| "Malformed firmware package".to_owned())??;
        if st.board != Board::Fm1 {
            return Err("This prototype supports FM-1 packages, not SMK-37".into());
        }
        if st.flash.len() != fm1_soc::FLASH_SIZE
            || st.entry < fm1_soc::XIP_BASE
            || st.entry as u64 + 2 > fm1_soc::XIP_BASE as u64 + st.xip.len() as u64
        {
            return Err("Invalid firmware memory layout".into());
        }
        let mut soc = Soc::new(RAM_SIZE, st.xip);
        soc.periph.hz = mhz as u64 * 1_000_000;
        soc.periph.skip_idle = true;
        soc.lcd_timed = false;
        let mut cpu = Cpu::new(st.entry);
        cpu.jit_on = true;
        cpu.user = true;
        let params = fm1_soc::RAM_BASE + 0x7fc00;
        soc.write32(params, params + 0x40);
        cpu.regs[0] = fm1_soc::stock_boot(&mut soc, &st.flash, params, st.board);
        soc.sfc_boot(st.key, st.app_end);
        if let Some(f) = flash {
            if f.len() != fm1_soc::FLASH_SIZE {
                return Err("Invalid local flash size".into());
            }
            soc.restore_flash(f);
        }
        // Decode the firmware's XIP code now, while loading, rather than in an audio period
        // the first time each path runs (~0.7 s here; removes most first-run stalls).
        let xip_end = fm1_soc::XIP_BASE + soc.xip.len() as u32;
        cpu.prewarm_xip(&mut soc, fm1_soc::XIP_BASE, xip_end);
        Ok(Self {
            cpu,
            cpu1: None,
            soc,
            pending: false,
        })
    }
    pub fn run(&mut self, n: u32) -> Result<(), String> {
        let jit = self.cpu.jit_on;
        fm1_soc::machine::run_dual(&mut self.cpu1, &mut self.soc, n, RAM_SIZE, jit, |s, k| {
            fm1_soc::machine::run_blocks(&mut self.cpu, s, k, &mut self.pending)
        })
    }
    pub fn midi(&mut self, bytes: &[u8]) {
        for b in bytes {
            self.soc.periph.apply(&Action::Midi { b: *b });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn malformed_headers_never_escape_loader() {
        // Original synthetic invalid bytes, not a real device firmware fixture.
        for len in [0, 1, 47, 48, 959, 960, 1024, 4096] {
            for byte in [0, 0xff, 0x7d] {
                assert!(Machine::boot(&vec![byte; len], None, 240).is_err());
            }
        }
    }
}
