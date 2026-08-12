# EchoTalk: an Echo II Emulator for NVDA by Jayson Smith

## What is this?

This NVDA addon emulates the Echo II speech synthesizer made by Street Electronics Corp. This synthesizer was used in the Apple II line of computers in the 1980's. For many of us who were using computers during that time period, the Echo was the first speech synthesizer we ever heard, and for some of us, it was all we had for years. Now, at last, you can use this same speech synthesizer directly in a modern screen reader, whether you're wanting to revisit old memories or you've never heard Echo before and are curious about how it sounds.

## How does it work?

This addon emulates the Texas Instruments TMS5220 chip which is the heart of the Echo speech synthesizer. It also emulates a MOS Technology 6502 microprocessor, the CPU used in the earliest Apple II series computers. This CPU natively runs the real, unmodified Textalker Echo speech generator software which drives the TMS5220 chip on the Echo speech synthesizer circuit board.

## AI Coding Disclosure

EchoTalk was coded entirely by Claude Code, with extensive guidance from me. I directed the work, supplied the Textalker disks, made the design
decisions and, importantly, did the listening. Several real bugs in this were found by ear and would not
otherwise have been caught, including a speech-pacing fault that had
survived five sessions of automated measurement saying everything was
fine.

## Settings and their Explanations

In the Speech category of NVDA's settings, when using this addon as your current synthesizer, you'll find the following settings:

- Voice: Switches the Textalker version. As this addon is packaged, your choices are Textalker 3.1.3 and Textalker 1.3. Version 3.1.3 is from 1986 while 1.3 is from 1981. Each has its own unique sound, and a few features are not available in 1.3.

- Rate: changes the speaking rate. This uses a custom method of modifying how the emulated TMS5220 chip generates speech. This gives much more control over the speech rate than is normally available with Echo.

- Pitch: Textalker has sixty-three pitches, though due to the way NVDA renders these sliders using percentages, not every pitch setting can be represented.

- Volume: This uses Textalker's volume control commands, so lower values will sound fuzzy, and the highest values will sound distorted in a unique way.

- Delay between words: This can add an adjustable delay between words. This feature is only available in Textalker version 3.

- Repeat character filter: This can be used to filter out repeated characters that are often used for decoration, ASCII art, etc. Note, however, that Textalker doesn't discriminate between punctuations and letters, so too low of a setting here may make undesirable modifications to the text being spoken. This is a known Textalker limitation. This feature is also only available in Textalker version 3.

- Chip clock: This changes the clock rate on the emulated TMS5220 chip. Changing this will change both the speed and the pitch of the voice. It acts just like speeding up or slowing down an analog tape recorder.

- Output sample rate: You have several choices here. The TMS5220 chip outputs at a sample rate of 8000. Your choices are 8K, 11K, 16K, 22K, 32K, 44K and 48K. Audio is naively upsampled without an antialiasing filter, and this is intentional since it gives higher frequency response where none would otherwise be possible. Note that this setting acts as a minimum rather than an exact figure, because the Chip clock setting above raises what the chip itself produces: at a chip clock of 1.5 the chip is generating 12000, so the output rate is raised to at least 12000 rather than downsampling the speech and throwing away quality. Your own choice is remembered, and takes effect again as soon as you bring the chip clock back down.

- Monotone: This is a check box, and enables or disables Textalker's ability to speak text in a completely monotone voice at your chosen pitch.

- Compressed speech: Another check box, this chooses between Compressed (fast) and Expanded (slow) speech, the two traditional speech rates Textalker natively supports. Textalker's implementation of compressed speech works by skipping tiny portions of the speech, so it sounds a bit different in addition to being faster. This difference is most noticeable with soft G or J sounds as in "giraffe" and "journey." The normal rate slider does not exhibit this behavior. This gives you the flexibility to experiment until you find what works best for you. Don't forget that you can also adjust the TMS5220 chip's clock speed for yet another way to adjust the speaking rate while also changing the pitch.

## Things to Know

- EchoTalk divides the text to be spoken into chunks so it will fit within Textalker's text buffer. The maximum chunk size is eighty characters. It splits chunks at clause boundaries (commas, periods, etc.) when it can, and never splits in the middle of a word unless one single word is longer than eighty bytes (in ordinary text this almost never happens). Each chunk of text is sent to Textalker as a separate utterance, so in especially long sentences you may be able to detect chunk boundaries because of the way they affect the synthesized speech.

- Textalker version 3 raises the pitch of single capital letters on its own. If you want capital letters raised in pitch the way Textalker does it, you need to set NVDA's Capital pitch change percentage setting to zero. Note that Textalker 1.3 does *not* have any facility for raising the pitch of capital letters.

## License

Since EchoTalk is an NVDA addon, it must be distributed under the terms of the GNU General Public License, version 2 (GPL). You can find a copy of this license [here](copying). The source code of the EchoTalk library which powers this addon, and of the related tools, is released under the 3-clause BSD license, which is one-way compatible into the GPL.

Please see the [Third Party Licenses](THIRD_PARTY_LICENSES.html) file in this folder for the licenses under which various components of this addon are distributed. In brief, code derived from MAME source code is distributed under the 3-clause BSD license, and the 6502 emulator is public domain. The Textalker binary files are not covered by any of these licenses.
