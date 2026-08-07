# Ctrl-D driver commands

Jayson's idea, session 11: settings that belong to the *driver* rather
than to Textalker should be reachable from inside the text stream, the
same way Textalker's own settings are reachable via Ctrl-E. Ctrl-E is
taken, so Ctrl-D — D for Driver.

## The command form

An introducer, optional whitespace, an optional number, a letter:

```
\x04 2F        frame rate 2          \x04F      frame rate to default
\x04 0.75C     clock multiplier      \x04C      clock to 1.0
\x04 0B        chunking off          \x04 80B   chunk at 80
\x04 1R        raw text on           \x04 0R    raw text off
\x04\x04       one literal 0x04, spoken rather than obeyed
```

Number-then-letter is deliberate: it is the shape of Textalker's own
`\x05 12P`, and the audience for this is people whose fingers already
know Echo commands. A bare letter restores that setting's default, which
costs nothing and saves having to remember what the default was.

Whitespace is allowed after the introducer, because that is where
someone writing "Ctrl-D 2F" naturally puts it, but **not** between the
number and the letter — `\x04 2 Fox` would then be ambiguous between a
command and text. The number and letter are one token, as in Ctrl-E.

Numbers always use `.` as the decimal point regardless of locale. This
is a wire format, not a display format.

## The namespaces are disjoint on purpose

Ctrl-D does **not** duplicate Textalker's commands, because it does not
need to: `text_prep.c` already preserves Ctrl-E (and Ctrl-V) through
normalization while dropping every other C0 control, so a Ctrl-E
sequence embedded in ordinary text reaches Textalker untouched. Verified
by measurement — `\x05 8P` before a sentence changes its amplitude
profile identically whether raw mode is on or off.

So: **Ctrl-E addresses the 1985 synthesiser, Ctrl-D addresses the driver
around it.** That keeps the letter space clear and the mental model
simple.

One consequence to know rather than discover: the letters mean different
things in the two namespaces. Ctrl-E `F` is flatness and Ctrl-E `C` is
compressed, where Ctrl-D `F` is frame rate and Ctrl-D `C` is clock. The
mnemonics are right in both cases and the introducer disambiguates, so
this was chosen knowingly rather than worked around.

## The hard part: a command has to end the utterance

Textalker buffers a whole line and synthesises nothing until the
terminating CR arrives. This is the same fact behind the "return" bug
fixed earlier in this session (`single_char_return_bug_fixed.md`), and
it has a sharp consequence here:

> `Hello \x04 3F world.`

By the time the whole line has been fed and the CR sent, *none* of it
has been synthesised. Applying frame rate 3 the moment the code is seen
would apply it to "Hello" as well. **A command's position in the text
does not correspond to its position in the audio.**

So a Ctrl-D command flushes the current utterance before it applies.
What precedes it speaks at the old settings, what follows at the new.
The cost is an utterance boundary — a small pause — wherever a command
appears. For a screen reader that is usually free, since commands will
sit at the start of an utterance anyway.

There is no way around this that does not involve predicting Textalker's
internal timing, which is a class of thing this project has repeatedly
learned not to attempt.

## What that cost in structure

`echotalk_speak()` split in two:

- `speak_run(et, text, len)` — everything the old `echotalk_speak` did:
  prepare (or not), split into lines, chunk, send, drain, trim, resample.
- `echotalk_speak()` — parses Ctrl-D, and calls `speak_run` once per
  stretch of text between commands.

Trimming and resampling both had to move inside the run, because both
depend on settings a later command may change. The clock multiplier in
particular was applied as a *single* resample over the whole speak()
call, declaring a different source rate for the same samples; with
per-run resampling each run gets its own pass. That introduces a
fractional-phase reset at each run boundary. Inaudible in testing, but
it is a new seam in a pipeline that previously had none, and this
project's history says seams are where the ear finds things.

With no commands in the text there is exactly one run, so the path is
unchanged — confirmed by the baselines below.

## A latent bug this uncovered

`echotalk_chunk_text()` stops when the caller's array fills and reports
nothing about the text it never reached. `echotalk_speak` called it once
with `echotalk_chunk chunks[256]`, so **any single line needing more
than 256 chunks silently lost its tail** — 20,480 characters at the
default chunk size of 80, which is why nobody hit it, but only 2,048 at
a chunk size of 8, which `\x04 8B` now makes reachable.

`send_line()` now calls the chunker in a loop until the line is
consumed, which removes the ceiling entirely. Verified: a 6,300-character
single line renders to 4,522,736 samples at chunk size 8 against
4,526,949 at chunk size 80 — within 0.1%, where truncation would have
cost two thirds of it.

`tools/test_chunker.c` gained a case that pins the underlying behaviour,
since it is a real property of the chunker's contract and not a bug in
it: 3 chunks written, 23 of 48 bytes reached, no error.

**`tools/render_common.h` has the same single-call pattern** at its
`echotalk_chunk chunks[256]`. It was left alone deliberately — it is the
reference harness the baseline table is measured with, and changing it
in the same session as the measurements would undermine them. Worth
fixing separately.

## Decisions taken

- **Settings persist** past the end of the `speak()` call that contained
  them, exactly as Ctrl-E commands persist inside Textalker.
- **Parsed in the library**, not in `say`, so NVDA gets the same
  behaviour without needing its own copy of the parser.
- **`0B` is allowed**, and `say` warns whenever chunking ends up off.
  Textalker's own line-buffer bound is never initialised under this
  emulation (`$FD80-$FD82` read zero), so its auto-flush point is
  undefined; chunking is the only thing standing between long text and
  behaviour nobody has characterised. Supported for experimentation,
  with a warning rather than silently.
- **Malformed commands are swallowed and counted**, never spoken. A
  screen reader reading its own control codes aloud would be worse than
  the command being ignored. `echotalk_command_errors()` is the only way
  a host can notice a typo; `say` prints a warning.
- **Raw mode honours Ctrl-D.** It has to, or `1R` would be a one-way
  door with no way back. That is what forces `\x04\x04` to exist as an
  escape for text that means a literal 0x04.

## Verification

Frame rate, clock and chunking all measured through `say --file`, using
"The quick brown fox jumps over the lazy dog." (32,384 samples plain):

| input | samples | check |
|---|---|---|
| plain | 32384 | baseline |
| `\x04 2F` | 17584 | 1.84x faster, against the 1.89x the frame rate promises |
| `\x04 0.75C` | 43162 | 1.333x longer, exactly 1/0.75 |
| `\x04 2F` … `\x04F` … | 49919 | 17584 + 32384 − 49, i.e. each half at its own rate |
| `\x04 1R \x05 8P` | — | amplitude identical to the same Ctrl-E command without raw mode |
| `\x04 1R` … `\x04 0R` … | 64719 | 2 × 32384 − 49, both halves spoken |
| `\x04B` after `\x04 8B` | 323399 | bit-identical to never having changed it |

Rejected as expected, text still spoken, one error counted each:
`\x04 9Z` (unknown letter), `\x04 1.5F` (fraction where a whole number
belongs), `\x04 999C` (out of range), a trailing `\x04` at end of text.
Command-only input and empty input both produce no speech and return 0.

The full HANDOFF baseline table reproduces exactly under both Textalker
versions, `hi_only` still renders at −12127/+26833, single-character
utterances are unchanged at 3131 / 1329 / 8931 samples for `,` / `a` /
`?`, and `make test` passes.

## Not done

The output **sample rate** is deliberately absent from the command set.
The stream has one format, fixed when the host opens it; changing it
partway through is not a thing a caller could act on.
