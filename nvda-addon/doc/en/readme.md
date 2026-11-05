# EchoTalk: an Echo II Emulator for NVDA by Jayson Smith

## What is this?

This NVDA addon emulates the Echo II speech synthesizer made by Street Electronics Corp. This synthesizer was used in the Apple II line of computers in the 1980's. For many of us who were using computers during that time period, the Echo was the first speech synthesizer we ever heard, and for some of us, it was all we had for years. Now, at last, you can use this same speech synthesizer directly in a modern screen reader, whether you're wanting to revisit old memories or you've never heard Echo before and are curious about how it sounds.

## How does it work?

This addon emulates the Texas Instruments TMS5220 chip which is the heart of the Echo speech synthesizer. It also emulates a MOS Technology 6502 microprocessor, the CPU used in the earliest Apple II series computers. This CPU natively runs the real, unmodified Textalker Echo speech generator software which drives the TMS5220 chip on the Echo speech synthesizer circuit board.

That is one of two ways this addon can produce speech, and it is the default. The other is **Native**, chosen with the Speech engine setting described below: Textalker's entire speech path, rewritten in ordinary C, with no 6502 anywhere in it. It is not an approximation. It was built by reading the original program and is checked byte for byte against the emulation at the point where Textalker hands data to the speech chip, so it says the same words in the same voice — and being ordinary code rather than an eight-bit program running in a simulated 48K machine, it can do things the original cannot, such as speaking much longer pieces of text at a time and using a pronunciation dictionary.

## Why you still have to supply Textalker files

Both engines need the Textalker disk files, and the native one is no exception. This surprises people, so it is worth explaining, because it is not a shortcut that could be tidied up later.

**What the native engine replaced is Textalker's code, not Textalker's data.** Textalker is a table-driven program: the part that decides how a word is pronounced is not really the 6502 instructions, it is around 3,000 bytes of letter-to-sound rules, plus the syllable word lists, the character attribute table, the names of the punctuation marks, and — most importantly — the packed speech frames that are the Echo's actual voice. Nearly two fifths of a Textalker image is table rather than program: 4,846 bytes of the 12,282 in version 3.1.3.

Those tables are Street Electronics' work, and version 3.1.3 carries an American Printing House for the Blind copyright as well. They are the part that took skill to make, and they are the part nobody here has any right to distribute. So EchoTalk reads them out of **your** copy of Textalker, every time it speaks, and **not one byte of them is built into this addon**. That is a deliberate design rule, it is why the addon can be given away freely, and it applies identically to both engines.

The consequence is worth stating plainly: rewriting more of Textalker would not remove this requirement. Even a completely native implementation, or a hybrid that took the best rules from one version and the best from another — which the design already allows, since the version and the engine are separate settings — would still be reading the rules and the voice out of a file you supplied. The engine can be given away. The voice cannot.

## AI Coding Disclosure

EchoTalk was coded entirely by Claude Code, with extensive guidance from me. I directed the work, supplied the Textalker disks, made the design
decisions and, importantly, did the listening. Several real bugs in this were found by ear and would not
otherwise have been caught, including a speech-pacing fault that had
survived five sessions of automated measurement saying everything was
fine.

## Settings and their Explanations

In the Speech category of NVDA's settings, when using this addon as your current synthesizer, you'll find the following settings:

- Voice: Switches the Textalker version. As this addon is packaged, your choices are Textalker 3.1.3 and Textalker 1.3. Version 3.1.3 is from 1986 while 1.3 is from 1981. Each has its own unique sound, and a few features are not available in 1.3. They also mispronounce different words, which matters if you use a pronunciation dictionary: see "A dictionary belongs to one Textalker version" below.

- Rate: changes the speaking rate. This uses a custom method of modifying how the emulated TMS5220 chip generates speech. This gives much more control over the speech rate than is normally available with Echo.

- Pitch: Textalker has sixty-three pitches, though due to the way NVDA renders these sliders using percentages, not every pitch setting can be represented.

- Volume: This uses Textalker's volume control commands, so lower values will sound fuzzy, and the highest values will sound distorted in a unique way.

- Delay between words: This can add an adjustable delay between words. This feature is only available in Textalker version 3.

- Repeat character filter: This can be used to filter out repeated characters that are often used for decoration, ASCII art, etc. Note, however, that Textalker doesn't discriminate between punctuations and letters, so too low of a setting here may make undesirable modifications to the text being spoken. This is a known Textalker limitation. This feature is also only available in Textalker version 3.

- Chip clock: This changes the clock rate on the emulated TMS5220 chip. Changing this will change both the speed and the pitch of the voice. It acts just like speeding up or slowing down an analog tape recorder.

- Output sample rate: You have several choices here. The TMS5220 chip outputs at a sample rate of 8000. Your choices are 8K, 11K, 16K, 22K, 32K, 44K and 48K. Audio is naively upsampled without an antialiasing filter, and this is intentional since it gives higher frequency response where none would otherwise be possible. Note that this setting acts as a minimum rather than an exact figure, because the Chip clock setting above raises what the chip itself produces: at a chip clock of 1.5 the chip is generating 12000, so the output rate is raised to at least 12000 rather than downsampling the speech and throwing away quality. Your own choice is remembered, and takes effect again as soon as you bring the chip clock back down.

- Monotone: This is a check box, and enables or disables Textalker's ability to speak text in a completely monotone voice at your chosen pitch.

- Compressed speech: Another check box, this chooses between Compressed (fast) and Expanded (slow) speech, the two traditional speech rates Textalker natively supports. Textalker's implementation of compressed speech works by skipping tiny portions of the speech, so it sounds a bit different in addition to being faster. This difference is most noticeable with soft G or J sounds as in "gentle" and "journey." The normal rate slider does not exhibit this behavior. This gives you the flexibility to experiment until you find what works best for you. Don't forget that you can also adjust the TMS5220 chip's clock speed for yet another way to adjust the speaking rate while also changing the pitch.

- Speech engine: This chooses which of two implementations of Textalker actually runs. **6502 emulation** is the default, and is the original approach: a real MOS 6502 processor is emulated, and the genuine unmodified Textalker program runs on it, exactly as described above. **Native** runs a rewrite of Textalker's speech path in ordinary C code, with no 6502 anywhere in it.

    Both engines say the same words in the same voice. That is not a hope, it is measured: the rewrite is checked byte for byte against the emulation at the point where Textalker hands data to the speech chip, across the reference passages, a thousand-word nonsense-story corpus, and thousands of randomly generated commands and phoneme strings. Every Textalker version this addon has been shown agrees exactly. The tiny difference that remains is in the timing of the silences between utterances, which can shift by a sample or two.

    So why have both? Because the emulation is the reference and always will be: it runs the real program, so it is right by construction, and it will run any Textalker build at all. The native engine is roughly a hundred times lighter on the processor, and, more usefully, it can be extended in ways the original program cannot. Two of those extensions are already here: it speaks much longer chunks of text at a time, so long sentences are broken up far less often, and it can use a pronunciation dictionary. Both are described below, under "Why is there still any chunking?" and "Pronunciation Dictionaries".

    If a Textalker build turns up that the native engine cannot read, the addon quietly stays on the emulation rather than falling silent, and says so in NVDA's log.

    Note that both engines read your own Textalker files, and the native one is no exception: see "Why you still have to supply Textalker files" above.

- Pronunciation dictionary: This chooses a pronunciation dictionary for the native engine. Any file named something.dict sitting in the addon's synthDrivers/echotalk folder is offered here by name, and "None" turns it off. **This setting does nothing unless the Speech engine above is set to Native**, because the emulated engine runs the real 6502 program, which has nowhere to put one.

    A dictionary is an override list, not a replacement. Words it names are pronounced the way it says; everything else, including all punctuation, still goes through Textalker's own letter-to-sound rules, so the voice and the intonation are unchanged. The "Pronunciation Dictionaries" section below explains how to write one. Note that a dictionary is written for a particular Textalker version, since the versions do not mispronounce the same words; the setting does not follow the Voice setting on its own, so change it yourself when you change voices.

- Line buffer: This is the size of Textalker's own text buffer, and it only means anything when the Speech engine above is set to Native. **256 bytes is what the real Textalker had**, and is the default. The larger sizes let the addon send longer pieces of text at a time, which means fewer places where a long sentence has to be broken in two. See "Why is there still any chunking?" below before changing it, because there is a genuine trade here rather than a free improvement.

## Why is there still any chunking?

A fair question, since a modern computer has gigabytes of memory and the Apple II had 48 kilobytes. The answer is that the eighty and the two hundred and forty are two different limits, and only one of them was ever about memory.

**The eighty is the emulation's.** Nothing in this addon plays the part of the disk operating system telling Textalker how wide the screen is, so Textalker never learns when it is supposed to speak a line, and text past about eighty characters is simply lost. The native engine does not have this problem at all.

**The two hundred and forty is Textalker's own**, and it is a number written into the program in 1981: when 240 bytes of its line buffer are used, it speaks what it has. The native engine reproduces that, because reproducing Textalker exactly is the thing that makes it worth trusting.

So turning chunking off would not remove any limit. It would just hand the splitting to Textalker, which is much worse at it. Textalker cuts at exactly 240 bytes wherever that lands, including in the middle of a word. On one test passage it cut the word "on" in half, and the result says "sentence oh ... in purpose" — not a pause in the wrong place, but the wrong words. The addon's own chunking exists to get there first and split at a comma instead.

That is what the Line buffer setting changes. Raising it raises Textalker's 240 in the same proportion, so the addon can send correspondingly longer text and split it less often. What it costs is exactness: the copy of Textalker running inside the native engine stops being a byte-for-byte match for the original in a few situations that depend on the buffer's size, all of them to do with what happens when a pronunciation rule reads past the end of the text into whatever the previous sentence left behind. That is a real Textalker behaviour, it is audible, and at 256 bytes this addon reproduces it exactly.

In short: leave it at 256 if you want the genuine article, and raise it if you would rather have long sentences spoken in one piece. Neither is wrong.

## Pronunciation Dictionaries

Textalker gets some words wrong, and always has — but not the same words in every version. In Textalker 3.1.3, NICKNAME loses its K entirely, COPY takes the vowel of "boat", FAVORITE comes out "faiv-or-ite", and BOOT, ROOT and SHOOT are all spoken with the vowel of BOOK. Textalker 1.3 says all of those correctly, and has mistakes of its own: it says BEGIN as "bih-jin". With the native engine selected you can fix these, one word at a time, without touching anything else.

A dictionary is a plain text file with a .dict extension, saved in the addon's synthDrivers/echotalk folder next to the Textalker images. One word per line: the word, some spaces, then how to say it. Lines beginning with ;;; or # are comments and are ignored.

There are three ways to spell a pronunciation, and you can mix them freely in one file.

**The easy one is a respelling.** Put an equals sign in front, then spell the word however makes Textalker say it correctly. Its own rules do the rest, and you need to know nothing about phonetics:

    BOOT      =booht
    ROOT      =rooht
    NICKNAME  =nicname

This is how most people who used an Echo already think, and it is the place to start. Say the word out loud, spell what you hear, and try it. The spelling does not have to look like English and nobody but Textalker ever sees it: the H in "booht" is not a sound, it is just the letter that makes Textalker's rules pick the right vowel, and dropping one of the K sounds out of "nicname" is what stops the two of them silencing each other.

**The second is ARPABET**, the notation used by the Carnegie Mellon pronouncing dictionary. A digit on a vowel marks stress: 1 is stressed, 0 is not.

    FAVORITE  F EY1 V ER0 IH0 T
    TOMATO    T AH0 M EY1 T OW0

Because this is exactly CMU's own format, a copy of their cmudict.dict file works here unmodified if you rename it to something.dict and drop it in the folder. That is around 134,000 words, and it loads in a fraction of a second. EchoTalk does not include a dictionary of its own; whichever one you use is yours, and whatever terms it comes with are between you and whoever wrote it.

**The third is Street Electronics' own phoneme notation**, the one the Echo manual documents and the one Control-V accepts. Put a slash in front. A digit from 1 to 3 before a vowel sets its pitch, 3 being the stressed one:

    ECHOTALK  /3EKOT3*K

This is the one to reach for when you want a particular sound and nothing else will do.

If a line cannot be understood it is skipped rather than rejecting the whole file, and if the file itself cannot be read the setting falls back to None and NVDA's log says why.

### A dictionary belongs to one Textalker version

The Voice setting chooses which Textalker is running, and the two do not make the same mistakes, so **a dictionary written for one version can be useless or actively wrong on the other**. Expect to keep a separate .dict file for each voice you use and to change the Pronunciation dictionary setting when you change the Voice; the setting lists every .dict file in the folder by name, so calling them something like textalker3.dict and textalker1.dict is enough to keep them straight.

ROUTE is the clearest case. Both versions say it to rhyme with "about". If you want it to rhyme with "shoot", then under Textalker 1.3 you can simply write

    ROUTE  =root

because 1.3 says ROOT correctly. Under 3.1.3 that same line accomplishes nothing at all, because 3.1.3 says ROOT wrongly in precisely the way it says BOOT, so there you need

    ROUTE  =rooht

It runs the other way too. BOOT =booht is the right fix for 3.1.3 and a small loss under 1.3, which says BOOT correctly already: the H that repairs the vowel in 3.1.3 also puts a faint break in the middle of the word, and under 1.3 you would be paying that for nothing.

Some entries are safe in both. NICKNAME =nicname is one — 1.3 says NICKNAME properly, and the respelling gives 1.3 exactly the same pronunciation it was giving anyway. But that is something to check per word rather than assume, and the quickest way to check is to listen.

## Things to Know

- EchoTalk divides the text to be spoken into chunks so it will fit within Textalker's text buffer. **How big a chunk can be depends on the Speech engine setting.** On the 6502 emulation it is eighty characters, because nothing in the emulation plays the part of the disk operating system telling Textalker how wide the screen is, so Textalker's own idea of when to speak a line is never set up and text past about eighty characters is simply lost. On the native engine that problem does not exist, and the only limit is Textalker's own line buffer, so a chunk can be a hundred and thirty characters in Textalker 3 and two hundred in Textalker 1.3. In practice that means most sentences are spoken in one piece instead of two or three, and the joins you can sometimes hear in long sentences largely go away. This is the main reason to use the native engine if you are not using a pronunciation dictionary. It splits chunks at clause boundaries (commas, periods, etc.) when it can, and never splits in the middle of a word unless one single word is longer than the whole chunk (in ordinary text this almost never happens). Each chunk of text is sent to Textalker as a separate utterance, so in especially long sentences you may be able to detect chunk boundaries because of the way they affect the synthesized speech.

- Textalker version 3 raises the pitch of single capital letters on its own. If you want capital letters raised in pitch the way Textalker does it, you need to set NVDA's Capital pitch change percentage setting to zero. Note that Textalker 1.3 does *not* have any facility for raising the pitch of capital letters.

## License

Since EchoTalk is an NVDA addon, it must be distributed under the terms of the GNU General Public License, version 2 (GPL). You can find a copy of this license [here](copying). The source code of the EchoTalk library which powers this addon, and of the related tools, is released under the 3-clause BSD license, which is one-way compatible into the GPL.

Please see the [Third Party Licenses](THIRD_PARTY_LICENSES.html) file in this folder for the licenses under which various components of this addon are distributed. In brief, code derived from MAME source code is distributed under the 3-clause BSD license, and the 6502 emulator is public domain. The Textalker binary files are not covered by any of these licenses.
