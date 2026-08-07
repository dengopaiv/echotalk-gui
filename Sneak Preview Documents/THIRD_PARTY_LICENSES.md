# Third-party licenses

`say32.exe` and `say64.exe` include third-party source code. This file
reproduces the required notices and must be distributed alongside them.

---

## Fake6502 — public domain

The 6502 processor emulator.

**Author:** Mike Chambers (miker00lz@gmail.com), 2011, version 1.1
**Upstream:** <http://rubbermallet.org/fake6502.c>

License statement, verbatim from the source:

> LICENSE: This source code is released into the public domain, but if
> you use it please do give credit. I put a lot of effort into writing
> this!

Being public domain it imposes no conditions on redistribution. The
credit above is given at the author's request.

---

## MAME — TMS5220 speech synthesizer emulation — BSD-3-Clause

The speech chip emulation, ported from MAME into standalone C. The LPC
synthesis core — lattice filter, chirp table, interpolation, parameter
decoding — is a faithful port of MAME's implementation, with MAME's
device framework and save-state machinery removed.

**Copyright holders:** Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu,
Raphael Nabet, Couriersud, Michael Zapf
(coefficient and chirp ROM tables: Frank Palazzolo, Couriersud,
Jonathan Gevaryahu)

The Apple II Echo II card's bus protocol and `/READY` handshake are
modelled on MAME's `a2echoii` driver, **copyright R. Belmont**, with the
ready logic traced by Lord Nightmare and Tony Diaz. That code is not
itself included, but the behaviour reproduced here derives from it.

### BSD-3-Clause license text

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## The Textalker files in this folder

`Textalker 3.1.3.bin`, `Textalker 3.1.3 Loader.bin`,
`Textalker 1.3.bin` and `Textalker 1.3 Loader.bin` are the original
Textalker software, published by **Street Electronics Corporation**. The
3.1.3 release also carries a 1986 copyright by the **American Printing
House for the Blind**.

They are **not** covered by any of the licenses above. They are the
original commercial product, unmodified, and `say` runs them under
emulation rather than reimplementing them — which is the whole point of
the exercise, but does mean these files remain the property of their
copyright holders.
