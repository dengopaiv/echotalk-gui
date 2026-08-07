# Single-character utterances also spoke "return" — fixed

Found by Jayson by ear, after the library was written; fixed in session
11. The suggested line of attack in HANDOFF.md (dig into `$D009`, the
keyboard-echo trampoline, for a "speak this one character" subroutine)
turned out to be unnecessary. The fix is a one-line reordering.

## The symptom

`say <loader> <obj> "," out.wav` spoke "comma **return**". Any
single-character utterance got two spoken items instead of one — which
matters most in exactly the case this code path exists for, character
review in a screen reader.

## Why it happened

`send_utterance()` in `src/echotalk.c` wrapped a one-character utterance
like this:

```c
if (single) send_string(et, "\x05L\x05" "A");   /* letter, all-punct  */
for (...) send_char(et, s[i]);
send_char(et, '\r');                            /* CR: speak it       */
if (single) send_string(et, "\x05S\x05W");      /* some-punct, word   */
```

The wrapping is needed: without it a lone `,` is silent. But the CR that
makes Textalker speak is itself a character, and in all-punctuation mode
Textalker announces it as "return". The restore sat *after* the CR, so
all-punctuation was still in force when the CR was processed.

## The fix

Move the restore before the CR:

```c
if (single) send_string(et, "\x05L\x05" "A");
for (...) send_char(et, s[i]);
if (single) send_string(et, "\x05S\x05W");
send_char(et, '\r');
```

This works because **Textalker buffers the whole line and processes it
in order when the CR arrives**, so an embedded Ctrl-E command takes
effect partway through that pass. The character was buffered ahead of
the restore, so it is still processed in all-punctuation mode and still
gets announced; the CR is reached after the restore and is therefore
silent.

That ordering property is the load-bearing fact here, and it was not
obvious in advance — it is equally consistent with Textalker applying
mode commands as they are typed, in which case moving the restore
earlier would have silenced the character too. It was settled by
measurement, not by reading the disassembly.

## Measurements that established it

All via `render_text_loader --raw --no-chunk`, Textalker 3.1.3, sample
counts at 8 kHz. Trailing `\r` shown explicitly.

| bytes sent | samples | what it is |
|---|---|---|
| `,\r` | 1195 | silence — bare comma says nothing |
| `\x05L,\r\x05S` | 1222 | silence — letter mode alone does not help |
| `\x05A\r` | 4436 | **"return" on its own** |
| `\x05A,\r\x05W` | 7444 | "comma return" |
| `\x05L\x05A,\r\x05S\x05W` | 7650 | "comma return" — the old library behaviour |
| `\x05L\x05A,\x05S\x05W\r` | **3082** | **"comma"** — the fix |
| `\x05A,\x05S\r` | 3075 | "comma" — `\x05S` alone is what matters |

7650 − 3082 = 4568, which is the 4436-sample "return" plus the pause
that precedes it. The arithmetic closes.

Note the third row from the bottom of the pre-fix group: `\x05A,\r\x05W`
still spoke "return" at 7444. That is not a counterexample — `\x05W` is
*word mode*, not punctuation mode. Only `\x05S` (some-punctuation) turns
off the all-punctuation announcement. Getting L/A/S/W straight matters:

- `\x05L` letter mode, `\x05W` word mode
- `\x05A` all punctuation, `\x05S` some punctuation, `\x05N` none

## Verified after the fix

Through `say`, both Textalker versions, sample counts within the ~50
samples the library's settings block adds over the harness:

| character | v3.1.3 | v1.3 | spoken |
|---|---|---|---|
| `,` | 3131 | 3101 | "comma" |
| `a` | 1329 | 1299 | "ay" |
| `Z` | 2931 | 2900 | "zee" |
| `.` | 4732 | 4701 | "period" |
| `5` | 3532 | 3501 | "five" |
| `?` | 8931 | 8901 | "question mark" (two words, hence the length) |

The whole HANDOFF.md baseline table reproduces unchanged under both
versions, `hi_only` still renders at −12127/+26833, `make test` passes,
and no run produced a `WARNING` or a wild-jump trap. Multi-character
speech is untouched by construction — every change is inside
`if (single)`.

## What this leaves

The `$D009` investigation is no longer needed for this bug, and the
fallback recorded in HANDOFF.md (drop all-punctuation and accept silent
punctuation marks) is not needed either — it would have traded away
functionality this fix keeps. `notes/session2_findings.md` remains
accurate about `$D009` blocking on a keypress; that just is not a
problem we have to solve.
