# Initial-port validation — 2026-11-10

This is an integration milestone, not a tested firmware compatibility release.
No real firmware, proprietary SDK, factory bank, or user flash image was
downloaded, executed, or bundled during this development pass.

## Build environment

- Linux x86-64, GNU C++ 14.2, glibc 2.41.
- Rust 1.90.0; runtime sources formatted with that toolchain's rustfmt.
- Official VCV Rack SDK 2.6.4 for Linux x86-64.
- Upstream emulator pinned at
  `5c6ed85df0b0bf6f0a400ce0887750707c7509ed`.
- Offline, locked Cargo builds; no external Cargo dependencies.

`plugin.so` compiled and linked successfully against the real Rack SDK. A
host-built Linux binary is not portable to every distribution (the
development build referenced glibc symbols through `GLIBC_2.39`); rebuild
against an appropriate supported sysroot before public distribution. No built
binaries are tracked in the repository.

Windows (MSYS2 MinGW64, Rust 1.97 `x86_64-pc-windows-gnu`): `plugin.dll` and
`make dist` built successfully; `cpp-test` and `abi-test` passed. The plugin
has not yet been loaded in Rack on Windows. macOS has not been built.

## Executed checks

| Check | Result | What it establishes |
| --- | --- | --- |
| Bridge/runtime Rust tests | 12 passed | ABI, hashes, local-store integrity, malformed/oversized input handling, bounded queues, generation filtering, key-release safety, silent-machine time-budget arithmetic |
| Vendored CPU/JIT tests | 71 passed | Interpreter/JIT instruction and control-flow regression coverage |
| Vendored ISA tests | 7 passed | Instruction decoder regression coverage |
| Vendored SoC tests | 90 passed | Peripheral, dual-core, clock, flash, MIDI and display-model regression coverage |
| Vendored hardware-vector integration tests | 3 passed | DSP/EQ/SRC models against upstream's recorded text vectors |
| C++ control helpers | Passed | CV conversion, gate hysteresis, note ownership, panic recovery, token validation, CC pacing and SRC accounting |
| C ABI executable | Passed | Actual Rust static library, ABI layout, empty-state silence, invalid inputs, 100,000 enqueue attempts, malformed import rejection and shutdown |
| Actual Rack module, headless | Passed | Dynamic load, module creation/process/destruction, 44.1/48/96/192 kHz, 16 CV lanes, missing-content behavior and exact custom JSON whitelist |
| Firmware-free source audit | Passed | Payload extensions/vendor binaries excluded; no patch-storage or base64 path; custom JSON keys limited to identity/options |

Total Rust tests: **183 passed, zero failed**. Most emulator regression tests
come from upstream; they are not 183 new Rack-specific tests. The compiler
reports existing upstream unused-assignment/dead-code warnings; this is not a
warning-free build.

The headless Rack test exercises the real compiled module without a widget,
which also checks that generation/reset handling does not require an open UI.
It does not render the panel or establish that every graphical interaction
works correctly in the Rack application.

Run the checks with:

```sh
make test RACK_DIR=/absolute/path/to/Rack-SDK
make rack-smoke RACK_DIR=/absolute/path/to/Rack-SDK
```

The second command is currently Linux-specific. The test inputs are emulator
instruction vectors and temporary synthetic invalid data, not a playable
firmware image. A static audit is a regression guard, not a proof of copyright
provenance or a security audit of arbitrary guest code.

## Not yet established

- Successful boot, usable LCD, audible output, or correct sound from any
  user-supplied `.fwsc` in this Rack host.
- Hardware parity, end-to-end musical latency, or real-world CPU/voice limits.
- Firmware-specific MIDI mapping, controls, user-bank requirements, flash Save,
  reopening, or multiple-instance behavior while real firmware is running.
- Visual layout and interaction quality in a running Rack GUI.
- Windows/macOS loading, JIT permissions, or cross-platform ABI behavior.
- Malicious-firmware isolation. Guest code runs in-process through a native
  emulator/JIT, not a hardened sandbox.

## Known first-pass limitations

1. Each patch save embeds the flash as it was after the firmware's last settled
   write (a write in the last ~250 ms before saving may be missed). Volatile
   RAM is not persisted.
2. Stock update packages may omit a preset/user bank. Automatic default-bank
   injection is intentionally disabled; a user SysEx/bank importer is not yet
   implemented. Missing content may leave a booted device silent.
3. Output uses nominal 44.1 kHz and linear interpolation. The modeled hardware
   timer is approximately 44,117.647 Hz. No cycle-perfect or high-quality SRC
   claim is made.
4. Worker scheduling is bounded between 20,000-instruction batches. A
   host-frame cycle budget prevents no-audio firmware from free-running, but
   this is not cycle-exact synchronization with the host.
5. Flash persistence uses the emulation worker. Slow disk I/O can starve audio,
   even though no file access or wait occurs in Rack's DSP callback.
6. There is no SysEx import, MIDI output, external MIDI-device selector, MPE,
   audio input, USB-audio bridge, or multitrack audio extraction yet.

The next meaningful validation step is a trusted, self-contained user-provided
`.fwsc`: boot it, exercise every control, play deterministic MIDI, compare its
audio with the pinned upstream host, and test local Save/reopen. Keep the file
and any generated flash data outside the distributable source and plugin.
