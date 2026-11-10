# Architecture and next implementation milestones

## Boundary

The Rack shared library links a Rust `staticlib` through
`include/fm1_rack.h`. No Rust-owned pointer other than an opaque handle crosses
that boundary. Audio and MIDI are fixed-size POD values. Display and status
are copied to caller-owned memory, never borrowed. ABI version is checked
before creating a runtime.

The three firmware-independent core crates are pinned to upstream commit
`5c6ed85df0b0bf6f0a400ce0887750707c7509ed`. The runtime is intentionally not a
copy of the upstream DAW plugin: it excludes nih-plug, egui, CLAP, VST SDKs,
firmware catalogs and firmware-inclusive session serialization.

## Ownership and threads

- **Rack DSP:** sole producer of MIDI queue and consumer of audio queue.
  Processes CV edges, fixed-array held-note bookkeeping and sample conversion.
  No file I/O, waits, heap allocation, or blocking mutex in this path.
- **Machine worker:** exclusively owns both emulated CPUs, SoC, parser and
  private flash. Advances in bounded instruction budgets; handles imports,
  reboot, UI events, generation changes, and snapshots.
- **Rack UI/serialization:** sends bounded non-realtime commands; uses
  nonblocking display/status copies. The UI renders a procedural original
  panel and uploads LCD pixels to NanoVG.

The caller must stop calls before destroying the handle. Destruction joins
the worker and is non-realtime. Queue roles must not be used concurrently by
multiple producers/consumers. Firmware reload never resets ring indexes from
the wrong thread: generation tags invalidate old packets instead.

## Audio and MIDI time

A monotonic native-frame counter advances when Rack requests audio, including
underruns. MIDI events carry a frame target and machine generation. Immediate
Rack events target the consumed position plus the configured lead. The worker
stages events in time order and inserts them into the emulated serial UART.

Execution is bounded by both audio queue fill and a cycle budget derived from
host-consumed native frames plus the configured lead. The latter prevents
firmware that never enables audio/DMA from advancing indefinitely while Rack
is paused. The budget is checked between finite instruction batches, not at
every emulated cycle.

Missing or stale frames produce silence. Cleanup is bounded, and the worker
can resynchronize its output position after underrun. This prioritizes
recoverable live operation, not exact preservation of musical timing after
an overload. An overload must be shown to the user, not concealed as proof of
sample-accurate emulation.

The current SRC is linear at the same nominal native rate as upstream. A
later bandlimited implementation should retain state across blocks and be
benchmarked at 44.1/48/96/192 kHz. Don't call the CPU interpreter or JIT directly
from Rack's per-sample callback as a shortcut.

## Local content model

The import limit is 16 MiB. A package is parsed/validated before it is accepted
into the local content-addressed cache. Cached imports recheck SHA-256. Local
state binds a firmware digest to a fixed-size flash image and checksum.
State IDs are validated hex, never filesystem paths. Writes use a temporary
file and replacement within the same local store.

Only identity/options cross Rack's patch serializer. Flash differences are
not an exception: firmware can write program/resource data to flash, so even
delta snapshots remain private local content. Do not add `.fm1save` or a
portable "include firmware" export without a separate explicit product and
licensing decision.

Each cached restore forks a new local session. Current session files are
mutable. Future exact Rack Save should use immutable content-addressed
checkpoints with a save-completion protocol and surface failures. Do not
block the real-time callback on fsync; stage snapshots to a separate writer.

## Next milestones, in order

1. **User-supplied firmware smoke test.** Import one known working `.fwsc`,
   confirm LCD boot, sound, all buttons/encoders, DIN MIDI response, local Save,
   close/reopen and duplicated instances. Keep its bytes outside this repo.
2. **Audio timing.** Record deterministic MIDI sequences and compare to pinned
   upstream native output. Test rapid reload, sample-rate changes, queue
   overflow, heavy dual-core firmware and overload recovery. Establish a
   real latency/CPU budget before advertising low-latency performance.
3. **Persistence.** Move snapshots to an I/O worker and add immutable
   Rack-save checkpoints, missing-state recovery UI and explicit local cache
   management. Tests should simulate interrupted writes without touching user
   data.
4. **Cross-platform.** Test macOS arm64/x64 and Windows GNU targets. macOS host
   JIT permissions and Windows FFI/link/runtime library handling need actual
   host validation. Add reproducible CI without any firmware downloads.
5. **Control depth.** Optional MIDI device input and SysEx, MIDI output,
   pitch bend/MPE, configurable CV mappings and a larger LCD window. Do not
   assume every firmware implements the same MIDI parameters.
6. **Public release.** Review naming/brand rules, provenance and notices,
   plugin packaging/portability, malformed-file hardening, and complete a
   firmware-specific compatibility table contributed by users.

## Integration into Leviathan

For an initial independent module, leave this project standalone. To bring it
into an existing Rack plugin later, reuse the Rust staticlib and C header,
rename/register the model in that plugin, and adapt only the frontend/build
glue. Preserve a separate local cache namespace and the serialization
whitelist. Do not import the M-VAVE application, its engine archive, or an
example firmware as a convenience dependency.
