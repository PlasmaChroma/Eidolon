# Third-party components

## FM-1 emulator

The `vendor/fm1-emu` subset comes from
<https://github.com/joakimeriksson/fm1-emu> at commit
`5c6ed85df0b0bf6f0a400ce0887750707c7509ed` (2026-11-08).
It contains only the `fm1-isa`, `fm1-core`, and `fm1-soc` crates, their build
metadata, and hardware test vectors. The vectors are measurements for testing
emulated DSP hardware, not firmware, sample banks, or a boot ROM.

Copyright (c) 2026 Joakim Eriksson. Distributed under the MIT license:

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

See `UPSTREAM.md` for the local patch inventory. No third-party firmware,
M-VAVE engine archive, firmware SDK, recovery image, or factory sound bank is
included. The GPL `fm1-sid` submodule and the DAW plugin dependency stack are
not part of this distribution.

## Rust toolchain

Rust's standard library is built into the native bridge. Rust is distributed
under MIT OR Apache-2.0; its compiler runtime components retain their original
notices. The toolchain itself is not included in this source package.
The Rust 1.90 standard-library attribution inventory and license texts are
included in `licenses/rust`. See <https://www.rust-lang.org/policies/licenses>
and refresh that inventory from your installed toolchain when building with a
different release.

## GNU C++ runtime in the experimental Linux binary

The Rack SDK's Linux link flags statically link the unmodified GCC 14
`libstdc++` and `libgcc` runtimes. Their terms are GPL-3.0-or-later with the
GCC Runtime Library Exception 3.1. The installed compiler distribution's
complete copyright inventory, including the exception, is preserved in
`licenses/gcc-14-copyright.txt`. This notice does not mean that the GCC
compiler itself is bundled. Recheck the runtime inventory when changing
compilers or target platforms.

## VCV Rack SDK

The Rack SDK is a user-supplied build dependency, not vendored here. The Rack
API is used under VCV Rack's Non-Commercial Plugin License Exception for a
free plugin. Rack Component Library graphics referenced at runtime retain
their own VCV/Grayscale terms; they are not copied into this package.

See <https://vcvrack.com/manual/PluginLicensing> and the SDK's `LICENSE.md`.
The MIT license of this project does not grant permission to sell a non-GPL
Rack plugin without any separate permission required by VCV. Hardware brand,
model, screen artwork in user firmware, and firmware licenses are separate.

The panel here is a new FirmwareHost layout. It is not an official M-VAVE or
VCV product, and neither company endorses it.
