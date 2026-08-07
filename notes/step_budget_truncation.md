# The step budget was too small, and failed silently

The real cause of what Jayson reported as "set the delay to max or the
rate to 0 and it goes all weird". The cancellation defects fixed first
(`nvda_cancel_race.md`) were real, but they were not this.

## The clue that solved it

> I can't reproduce the problem by setting the clock rate to 0%, which
> should accomplish the same thing (synthesized speech taking four times
> as long to play).

That killed the duration theory outright, and pointed straight at the
answer. All three settings make speech four times longer, but:

- **clock multiplier** stretches the OUTPUT by resampling. The emulation
  does exactly the same work.
- **speed** and **word delay** make the EMULATION run longer -- both
  leave Textalker sitting and waiting on the chip.

So the trigger was never duration. It was how long the 6502 runs per
character. A negative result, offered unprompted, was worth more than
any amount of further guessing.

## The bug

`send_char()` ran the 6502 with a budget of **5,000,000** instructions:

```c
run_to_halt(et, et->entry, 5000000, 0x0201);
```

At word delay 15 with speed 0.25, a single character needs up to about
**20,000,000**. The budget cut the CPU off part-way through Textalker's
routine, leaving a half-finished call stack -- and `send_char` then
resets SP and re-enters at the entry point for the next character, on
top of the wreckage.

The return value was discarded, so **nothing said a word about it**.

## Measured

Same line, `The quick brown fox jumps over the lazy dog while the cat
watches from a wall.`, samples produced:

| setting | 5M budget | budget raised | |
|---|---|---|---|
| defaults | 53,784 | 53,784 | untouched |
| word delay 15 | 102,999 | 213,115 | **half the speech lost** |
| speed 0.25 | 140,872 | 204,972 | truncated |
| both | 115,487 | 367,302 | **two thirds lost** |

Bisecting the budget, output stops changing at **20,000,000** and is
identical at 40M, 80M and 160M. Raising the drain guard alone changed
nothing, which isolated it to the step budget rather than the chip
drain.

## The fix

`STEP_BUDGET` is now 64,000,000 -- about three times the worst case
measured across both Textalker versions at the slowest exposed settings.
It costs nothing in normal use, because a normal character finishes
thousands of times sooner; it only sets how long a genuine hang takes to
give up, which at roughly 30M emulated instructions per second is about
two seconds. `DRAIN_BUDGET` went from 500,000 to 4,000,000 for the same
reason, though it was never the one being hit.

Defaults are unaffected: all nine reference files reproduce exactly under
both Textalker versions, `hi_only` still at -12127/+26833.

## The part that matters more than the number

A budget that truncates in silence will be got wrong again. Both guards
now increment a counter, exposed as `echotalk_overruns()`, and **any
non-zero value is a fault**: `say` prints a warning, the NVDA driver logs
an error naming the settings, and both DLL test suites assert it stays
zero at the slowest speed and longest word delay.

Picking a bigger number fixes today's bug. Making it impossible to hit
one quietly is what stops the next one.

## A testing lesson, for the fourth time this session

Two checks broke while fixing this, neither because the code was wrong:

- The flatness check compared whole audio buffers for byte equality. The
  two renderings differed by **one sample** of length while every state
  getter agreed flatness had survived. Replaced with a content
  comparison -- and the first attempt at THAT was also wrong, because a
  one-sample alignment shift makes sample-wise comparison meaningless:
  it reported a mean difference of 2271 for two renderings of the same
  voice. Allowing a shift of a few samples gives 0.0 against 5656 for
  the genuinely different one.
- The cancel-race check counted "feeds since the first ever stop", so
  after one cancel every legitimate feed thereafter counted as stale. It
  passed originally only because that run was short. Now it measures
  what it means: feeds arriving between a cancel and the next speak.

The pattern is the same each time -- asserting exact equality of audio
when the claim is about content, or counting a running total when the
claim is about a window.
