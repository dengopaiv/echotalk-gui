# Mirroring Ctrl-E commands into the library's own settings

Jayson's request, session 11: the library should keep passing Ctrl-E
commands through to Textalker exactly as before, but also update its own
variables to match.

## Why it matters

The motivating case is offering both Textalker versions as voice
variants. Loading the other version means resetting the 6502 and all of
its memory, which puts Textalker back at ITS defaults. To restore the
voice afterwards you have to know what the voice was -- and if the
caller set it with a Ctrl-E command embedded in the text, the library
had no idea.

A second reason turned up while implementing it, and is arguably worse:
the library's settings block was **actively undoing** what the text had
asked for. `apply_settings()` fires whenever any setting is touched, and
it sent `\x05 %dP` unconditionally. So a caller who put `\x05 40F` in
their text got monotone speech until the next time anything else
changed, at which point it silently reverted.

## What Textalker's commands actually are

Corrected by Jayson mid-investigation, after I had started measuring my
way towards the wrong model:

> There is no default flatness value, since flatness is either on or
> off. The pitch (P) commands set pitch and normal (not flat) speech,
> and the same commands with F instead of P set the pitch, but make the
> speech monotone.

So `nP` and `nF` are **one setting with two spellings**, not two
settings. That explains a probe result that had looked odd: sending no
F command differs from sending `0F`, because the first is normal speech
at the default pitch and the second is monotone at pitch zero. I had
been about to search 64 values for a "default flatness" that does not
exist.

The library stores pitch and flatness separately, because that is how a
caller thinks about them, and recombines them when the command goes out.

## Measured while implementing

- **Command letters are case-insensitive.** `\x05 10p` produces audio
  identical to `\x05 10P`, and both differ from sending nothing. A
  sniffer that only recognised uppercase would silently miss half the
  commands and drift out of step.
- **Pitch does not saturate at 63.** `\x05 99P` is audibly not
  `\x05 63P`. Flatness *appeared* to saturate there -- 63, 64 and 99 all
  identical -- which fits: monotone holds one pitch, so anything past
  the chip's range clips to the same value, whereas with intonation the
  contour still differs.

Sniffed values are therefore **clamped to the ranges the setters
accept**, which means replaying out-of-spec input is not bit-exact. That
was a deliberate trade for a property worth more: every value a getter
returns is accepted by its matching setter, so the round trip always
works.

## What is tracked

`sniff_ctrl_e()` watches text on its way to Textalker and mirrors:

| command | effect |
|---|---|
| `nP` | pitch, normal intonation |
| `nF` | the same pitch, monotone |
| `nV` `nD` `nR` | volume, word delay, repeat filter |
| `C` / `E` | compressed / expanded |
| `L` / `W` | letter / word mode |
| `A` / `S` / `N` | all / some / no punctuation |

Only the caller's own text is examined, never the wrapper
`send_utterance()` puts around a single character -- that restores the
tracked modes, so reading it back would be circular.

Twelve getters were added alongside, covering the driver-side settings
too. Plain getters rather than a settings struct: a struct in the ABI
breaks binary compatibility every time a field is added, and the host
loop is eight lines either way.

## A bug this exposed

The single-character path wrapped the character in `\x05L\x05A` and then
restored `\x05S\x05W` **unconditionally**. A caller who had chosen
all-punctuation or letter mode lost it the first time a one-character
utterance went past. It now restores the tracked modes, so a caller's
choice survives.

`echotalk_set_letter_mode()` and `echotalk_set_punctuation()` are only
pushed to Textalker once one of them has actually been called. Sending
them at startup would assert this code's guess at Textalker's startup
punctuation mode as though it were measured -- a lone comma being silent
rules out all-punctuation, but does not tell "some" from "none" -- and
would change the byte stream every instance emits before its first word.

## Verified

- Every command above mirrored, including lowercase, with out-of-range
  values clamped and every getter value accepted by its setter.
- **Flatness set from the text survives a settings push**: the audio
  after the push is byte-identical to the same sentence with flatness
  set through the API, and both differ from the normal-intonation
  rendering. Getting this comparison right took two attempts -- the
  first compared a call that emitted a settings block against one that
  did not, and the block's untrimmed lead-in made them differ whatever
  the voice was doing. The library was right and the check was wrong.
- Punctuation mode 2 survives a one-character utterance.
- **The whole point, end to end**: a voice built partly through the API
  and partly from embedded Ctrl-E was read off a 3.1.3 instance, the
  instance destroyed, a 1.3 instance created (arriving at its own
  defaults, confirmed), the settings pushed across, and every value read
  back identical.
- Harness baselines exact for all nine files under both versions,
  `hi_only` at -12127/+26833, library counts unchanged, `make test`
  passes, both DLL tests pass on both architectures, and
  `listen_check.py` passes all 19 checks.

ABI bumped to **3**; 39 exports.
