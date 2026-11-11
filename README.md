# Firmware Host — FM-1 runtime for VCV Rack

An initial native VCV Rack 2 port of Joakim Eriksson's MIT-licensed FM-1
emulator. The module executes a **user-supplied compiled `.fwsc` package**;
it is not a source-level reimplementation of a particular synth engine.

**No firmware is included, downloaded, or embedded in Rack patches.** This
includes stock recovery images, third-party firmware, factory sound banks,
the M-VAVE binary SDK, and emulated flash snapshots. An empty module is silent.

This is a development starter, not a firmware-compatibility-certified release.
See `docs/VALIDATION.md` for what was actually built and tested. No real
firmware was supplied for this development pass, so audible behavior, hardware
parity, all UI interactions, and firmware-specific save/restore still require
testing with your own lawful firmware files.

## What is implemented

- Native C++ Rack module, 32 HP, with a new procedural panel.
- Rust static-library bridge around pinned `fm1-isa`, `fm1-core`, `fm1-soc`.
- Emulation on a dedicated thread, bounded lock-free audio/MIDI queues, and
  generation-tagged data so reloads cannot replay an old machine's buffers.
- Dual-core machine execution and upstream native JIT support.
- 240×240 emulated LCD, 7 encoders, 14 function buttons, 27 key contacts,
  master ADC control, and the emulated LEDs.
- Polyphonic pitch/gate/velocity to MIDI, a CC input, 24-PPQN MIDI clock,
  transport start/stop/reset, and stereo audio outputs.
- Local content-addressed firmware import and checksummed local flash state.
- Strict patch metadata whitelist: schema, firmware SHA-256, local session ID,
  CPU clock and buffer lead. Ordinary host controls are Rack parameters.
- No browser, WebAssembly host, DAW plugin wrapper, networking, automatic
  firmware catalog, device flasher, or firmware-inclusive save export.

The module is currently FM-1 only. SMK-37 packages are explicitly rejected.

Some stock update packages omit the device's separate user/preset bank. The
upstream host normally fills a synthetic default DX7 bank; this port does not.
Such packages may boot with missing or silent voices until the user supplies
the missing content. A user `.syx` bank importer is **not implemented yet**.
Use a self-contained community firmware as the first audio smoke-test target;
do not assume that a stock update alone recreates a complete factory device.

## Build

Requirements: Rust/Cargo **1.90 or newer**, a C++17 compiler, GNU Make, `jq`,
and the official Rack 2 SDK. `zstd` is needed for `make dist`. The Rust bridge
has no crates.io dependencies; all three emulator crates are vendored source.
Normal builds use `--offline --locked` and do not fetch anything.

```sh
make -j2 RACK_DIR=/absolute/path/to/Rack-SDK
make dist RACK_DIR=/absolute/path/to/Rack-SDK
```

The output package is
`dist/Eidolon-2.0.0-<platform>.vcvplugin`. Place the package in the Rack
user folder's `plugins-<platform>` directory and restart Rack, or build with
the SDK's usual `make install` after confirming the destination.

For a nonstandard Rust installation:

```sh
make RACK_DIR=/absolute/path/to/Rack-SDK CARGO=/absolute/path/to/cargo
```

For cross-compilation set `RUST_TARGET` and the Rack compiler/toolchain to
matching ABIs. In particular, the Windows Rack toolchain uses GNU/MinGW;
do not try linking an MSVC Rust static library into that build. Example target:
`RUST_TARGET=x86_64-pc-windows-gnu`. On Apple silicon use
`aarch64-apple-darwin` with an arm64 Rack SDK. These paths are not validated
in this first pass; Linux x86-64 is the tested build target.

Do not use a host-built Linux binary as a universal Linux release. The
development build uses glibc 2.41; rebuild with VCV's supported toolchain or
an appropriate older sysroot for broad Linux distribution.

The repository tracks source only; no built `.vcvplugin` packages are
committed. Build from source with the steps above.

## First use

1. Add **Firmware Host / FM1 Host** in Rack.
2. Right-click the module and choose **Import firmware (.fwsc)...**. You can
   also drop one local file onto its panel.
3. Select a package you obtained independently and have permission to use.
   This module neither retrieves it nor supplies any missing factory data.
4. The status area reports validation, loading, or an error. If successful,
   the firmware draws its own display and its controls become usable.
5. Use the panel key contacts or connect pitch and gate; route L/R to an audio
   module. Select the appropriate MIDI channel in the context menu for the
   particular firmware. The default channel is 1, not a claim that every
   firmware listens on that channel.

**Firmware is guest code, not trusted by default.** This emulator is not a
security sandbox. Load only firmware you trust. Malformed inputs are bounded
and many parser/runtime failures are caught, but JIT/emulator bugs can still
affect the hosting Rack process.

### Controls and ports

| Control | Behavior |
| --- | --- |
| SELECT, PRESET, ALGO, K1–K4 | Drag vertically or scroll; sends encoder detents |
| Function buttons / key contacts | Left-click holds; right-click latches for combinations |
| MASTER | Drives the emulated hardware master ADC |
| PITCH | 1 V/oct, 0 V = MIDI note 60; quantized to semitones |
| GATE | Up to 16 lanes, rising threshold 1 V / falling threshold 0.1 V |
| VELOCITY | 0–10 V maps to 1–127; unpatched defaults to 100 |
| CC | 0–10 V; number in context menu; changed values at at most 250 Hz |
| CLOCK | Each rising edge sends one MIDI Clock byte; supply **24 PPQN**, not one pulse per beat |
| RUN | Gate transitions send MIDI Start/Stop |
| RESET | Stop, Song Position Pointer zero, then Start when RUN permits |
| L / R | Stereo audio; normalized firmware samples scaled to nominal ±5 V |

All CV voices use the selected MIDI channel. Duplicate held pitches are
reference-counted. Changing quantized pitch while gated retriggers that voice;
this first pass does not implement continuous pitch bend, MPE, or audio-rate
parameter modulation. Queue overflow triggers bounded MIDI panic recovery.

MIDI enters the emulated DIN UART at its hardware rate. The current frontend
does not expose external MIDI devices, SysEx import, MIDI output, USB audio,
audio input, or per-track audio taps. Encoders/buttons remain direct virtual
hardware events and do not depend on the firmware's MIDI mapping.

## Firmware and patch storage

All imported material stays under **`<Rack user folder>/Eidolon/`**:

```text
firmware/<sha256>.fwsc
state/<firmware-sha256>/<local-session-id>.flash
```

The `.flash` file is a private local device snapshot and **may contain firmware
and resources**. It is not a portable, firmware-free preset. Neither it nor
its changed sectors are placed in Rack patch storage.

Rack's custom module JSON contains only:

```json
{
  "schema": 1,
  "firmwareSha256": "64 lowercase hexadecimal characters",
  "stateId": "64 lowercase hexadecimal characters",
  "clockMhz": 240,
  "leadFrames": 1024
}
```

The example strings explain the fields; real patches contain validated
identifiers. No absolute original file path, package bytes, base64, flash data,
samples, or firmware download URL is serialized. Copying a patch to another
computer does not transfer the instrument. That computer must have the exact
matching package and relevant local state. Missing content produces a clear
error, not an automatic download or a silent substitution.

Importing a package makes a new local session. Restoring an existing session
forks it so duplicated module instances do not overwrite one another. Local
flash changes are saved after writes settle, on request, and best-effort at
shutdown. Firmware edits held only in emulated RAM still need the firmware's
own Save command. Use **Save local flash now** after that operation.

**Current persistence limitation:** the session file represents the latest
local device state, not an immutable checkpoint at each Rack Save. Later
firmware saves can change what an earlier patch referencing that session will
restore. Exact historical patch snapshots are a next milestone. The local
store intentionally has no automatic garbage collection yet; nothing deletes
your imported files or old sessions behind your back.

## Latency and CPU

The default clock is 240 MHz and the default lead is 1024 frames at a nominal
44.1 kHz (~23.2 ms). The menu offers 256, 512, 1024, 2048 and 4096 frames. This
is only the emulator's buffering, not total key-to-speaker latency: firmware
scan/DMA timing and Rack/audio-device buffering also contribute. No automatic
latency compensation is added to other Rack signal paths.

The source output follows upstream's nominal 44.1 kHz convention; the modeled
hardware timer is 24 MHz/544 (~44117.65 Hz). This preserves upstream host
behavior rather than claiming cycle/clock-perfect analog-hardware timing.
The initial output resampler is linear interpolation. High-quality
bandlimited conversion and stricter hardware-timing modes are future work.

Emulating a processor is heavier than running native synth DSP. Watch the
status/underrun counters and benchmark your actual firmware before lowering
the lead. Flash persistence currently performs disk I/O on the machine worker
(never the Rack DSP callback), so slow disks can still cause an underrun while
saving. A separate snapshot-writer thread is planned.

## Tests and source package

```sh
make test RACK_DIR=/absolute/path/to/Rack-SDK
# Linux: instantiate the actual compiled module using libRack, no GUI/firmware
make rack-smoke RACK_DIR=/absolute/path/to/Rack-SDK
make source-dist RACK_DIR=/absolute/path/to/Rack-SDK
```

Tests use instruction/hardware regression vectors and synthetic invalid
inputs, not a bundled playable firmware. See `docs/VALIDATION.md` and
`docs/ARCHITECTURE.md`. Upstream provenance and patches are in `UPSTREAM.md`.

## Licensing and publication

Host code is MIT. Preserve the upstream MIT notice and `THIRD_PARTY.md`.
The Rack SDK and user-loaded firmware remain under their own terms. Excluding
firmware from distribution does not grant users an off-device execution right
that their firmware license otherwise lacks. This project supplies no such
permission and makes no endorsement claim.

Use this first as a local development port. A public release needs firmware
compatibility testing, cross-platform builds, a final dependency/notice review,
and review of VCV Library naming/panel requirements. Do not describe it as an
official M-VAVE product.
