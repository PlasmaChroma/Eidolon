# Pinned upstream and local changes

- Repository: <https://github.com/joakimeriksson/fm1-emu>
- Commit: `5c6ed85df0b0bf6f0a400ce0887750707c7509ed`
- Upstream package version at this revision: 0.4.0
- License: MIT, copyright Joakim Eriksson
- Included crates: `fm1-isa`, `fm1-core`, `fm1-soc`
- Omitted: DAW and browser hosts, frontend dependencies, firmware catalogs,
  firmware download scripts, SID submodule, portable firmware-inclusive saves

The three vendored crates have no external Cargo dependencies. The generated
instruction decoder is upstream emulator source, not a firmware image.

## Local adaptation inventory

1. A reduced vendor Cargo workspace includes only the three runtime crates.
2. Two experimental `if let` match guards in the instruction decoder are
   rewritten as equivalent nested conditionals for stable Rust 1.90.
3. Native JIT executable-memory ownership is repaired for repeated reloads.
4. The Rack loader does not inject upstream's synthetic default DX7 bank;
   the user-supplied package determines its own initial content.
5. A read-only MIDI-ready accessor exposes UART initialization state, allowing
   the bridge to apply backpressure without dropping/splitting messages.

The new runtime, C ABI, Rack adapter and panel live outside the vendor folder.
The native host initialization intentionally follows upstream `fm1-live`:
initialize SFC decryption with `sfc_boot`, and restore flash through
`Soc::restore_flash` so executable flash mappings remain synchronized.

Before updating the vendor snapshot, review these differences, rerun the
complete test suite, and validate user-supplied firmware on both interpreter
and native JIT paths. Do not replace the vendor folder from an unpinned branch
as part of the normal build.
