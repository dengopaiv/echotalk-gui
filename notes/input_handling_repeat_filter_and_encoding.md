# Input handling: repeat filter, line endings, character encoding (session 10)

Three related fixes to what reaches Textalker, all raised by the user.

## 1. The repeat-character filter mangles ordinary words

Textalker v3.1.3 collapses runs of the same character, so that a
decorative line of `*****` is not read out one "star" at a time. It does
not distinguish decoration from content: `EEEEEEEEE` is spoken as
though it were `EE`.

Disable it by sending `Ctrl-E`, `99`, `R` -- a repeat threshold high
enough never to trigger in practice. This is sent once, after init
completes and before any real text. `--no-repeat-fix` restores the
default behavior.

Measured on `reference_text/repeat_letters.bin` (`EEEEEEEEE`):

| | duration |
|---|---|
| filter active (default Textalker) | 0.206 s (~two E's) |
| filter disabled | 0.956 s (~nine E's) |

**This was already corrupting real content.** The alphabet song ends
`MEEEE` -- the held final note of "now I've sung my ABCs, tell me what
you think of me-e-e-e" -- and it was being truncated to `MEE`. It also
contains `TELL`, `SEES`, `BEE` and `ZEE`. Enabling the fix lengthens
that render by 0.225 s, which is the song's ending being sung properly
for the first time.

Effect on everything else is negligible: after trimming, the other
reference renders change by at most 2 samples, which is sub-millisecond
phase shift from the four extra command characters taking their own
6502 time during init, not an audible difference. Verified that
`--no-repeat-fix` reproduces the pre-change output byte-for-byte.

v1.3 gets the same command. It predates several v3.1.3 commands and
silently discards any it does not recognise (see HANDOFF.md on the
`$D857` dispatch chain), so this is harmless either way, and confirmed
not to change v1.3 renders.

## 2. LF is not a line ending to Textalker

The Apple II terminates lines with CR. A bare LF is not a terminator to
Textalker -- in all-punctuation mode it announces it as "linefeed". So
LF is stripped, and CRLF collapses to the CR Textalker expects.

Other C0 control characters would likewise be spoken or misread, so they
are dropped too, with three exceptions that carry meaning: CR, `Ctrl-E`
(`$05`, introduces a command) and `Ctrl-V` (`$16`, phoneme mode). Tab
becomes a space.

## 3. Everything must arrive as 7-bit ASCII

Textalker predates every extended 8-bit encoding -- Latin-1, MS-ANSI,
Windows-1252, UTF-8, all of it. It knows the original 128-character
ASCII set and nothing else. The Apple II high-bit convention is a
*transport* detail applied by the emulation layer when handing bytes to
Textalker, not an encoding, so a raw byte >= `$80` in the input would
collide with it and be spoken as something arbitrary.

`src/text_prep.c` handles this, and like the chunker it is pure logic
with no emulator dependency and its own unit tests
(`tools/test_text_prep.c`, run via `make test`).

**Encoding is detected per character rather than declared.** A
well-formed UTF-8 sequence is decoded as UTF-8; any other byte >= `$80`
is read as Windows-1252, which is Latin-1 except for `$80-$9F` where it
holds the curly quotes and dashes real-world text is full of. This
handles UTF-8, legacy single-byte text, and the two mixed together in
one string, without the caller having to know which it has. It also
means a truncated UTF-8 sequence degrades to a plausible Latin-1
character rather than an error.

Mapping policy:
- Accented Latin letters lose their accents (`é` -> `e`), covering
  Latin-1 Supplement and Latin Extended-A, so most European text
  survives readably.
- Digraphs are spelled out: `æ` -> `ae`, `ß` -> `ss`, `œ` -> `oe`,
  `Þ` -> `TH`.
- Typographic punctuation becomes its ASCII ancestor: curly quotes ->
  `'` and `"`, en dash -> `-`, em dash -> `--`, ellipsis -> `...`,
  non-breaking space -> space.
- Symbols with no ASCII form become short words where that is clearly
  more useful than silence: `°` -> ` degrees`, `€` -> ` euros`,
  `±` -> ` plus or minus`, `©` -> ` copyright`.
- Zero-width characters and the BOM vanish entirely.
- Anything still unmapped becomes a space by default, so words cannot
  silently run together. Callers can ask for outright deletion instead.

Ordering matters: **prepare text before chunking.** Preparation can
change length (one character may become several), and the chunker's job
is fitting text to Textalker's buffer, which only means anything once
the text is in Textalker's own character set.

The `--raw` flag on the render tools bypasses preparation, for feeding
Textalker exact byte sequences during debugging. Confirmed that
preparation is a no-op on every existing `reference_text/` input, all of
which are already plain ASCII.
