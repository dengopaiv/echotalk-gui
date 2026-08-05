# Long text split mid-word: chunker was never wired in (session 10)

## The report
`reference_text/Hedge Trimmer Story.bin` -- a 465-character run-on
sentence with no punctuation until the very end -- broke mid-word, once
between the second "m" and the "e" of "trimmer" (~char 168) and again
after "call" (~char 332).

## Two separate causes

### 1. The render tools never used the chunker
`src/chunker.c` has existed since session 6, is unit-tested, and was
documented as the plan for keeping text inside Textalker's buffer -- but
nothing ever called it. The render tools sent the input byte stream
straight through, so splitting was left entirely to Textalker's own
line-buffer auto-flush, which lands wherever it lands, including the
middle of a word.

Measured flush points on the original file, by watching which character
produced audio: char 166 (the second "m" in "trimmer") and char 330 (the
space after "call"), then the final CR. Buffered runs of 165, 164 and
134 characters.

### 2. Textalker's buffer bounds are never initialised in this harness
165 characters matches neither the 40 of a 40-column Apple II nor the 80
of 80-column mode. Reading Textalker's own bounds after boot explains
why:

```
line-buffer bounds: $FD80=0 $FD81=0 $FD82=0
```

All zero. Textalker computes these at `$D781` from the Apple II text
window width, and a PC watch confirms **`$D781` is never reached** in our
boot -- setting zero page $21 (WNDWDTH) and the $C01F 80-column flag
beforehand makes no difference. On a real machine DOS/Applesoft screen
handling drives that path; nothing in this harness plays that part.

So the ~165-character flush point is emergent behaviour from an
uninitialised bound, not a real buffer size. It must not be relied on.

## Fix
The chunker is now wired into `render_common.h`, so all three harnesses
get it. Policy:

- CR is an explicit utterance boundary in the input, so existing lines
  are honoured as-is and only over-long ones are split.
- Over-long lines go through `echotalk_chunk_text()`, which prefers
  clause boundaries, then word boundaries, and only hard-splits a token
  longer than the whole window.
- Each resulting chunk is terminated with its own CR, which is what
  makes Textalker speak it.

Default limit is 80 characters (`--chunk N` to change, `--no-chunk` to
disable): the largest buffer Textalker natively supports, and well under
the observed flush point.

Result on the hedge trimmer story: six chunks split at word boundaries,
every flush now occurring at a CR we inserted rather than mid-word.
Every other file in `reference_text/` is shorter than 80 characters per
line, so all of them render byte-identically -- verified.

## Loose end worth knowing about
Because `$D781` never runs, Textalker's own notion of line width is
undefined here. That no longer matters for splitting, since we now split
before it can, but it would matter to anything else that reads those
bounds. If a future change needs Textalker's buffer to be genuinely
configured -- or if the 80-column decision in
`notes/buffer_chunking_and_indexing.md` is ever revisited -- the first
step is finding what actually calls `$D781` on a real machine, since
supplying WNDWDTH alone does not get there.
