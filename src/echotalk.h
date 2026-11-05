/*
 * echotalk.h -- speech synthesis by emulating an Apple II Echo II card
 * running Street Electronics' Textalker.
 *
 * Give it a Textalker loader and OBJ image and it speaks. Which
 * Textalker version you get is decided by which images you pass;
 * everything else about the two versions is worked out from the files
 * themselves, so 3.1.3, 1.3 and near-identical builds like 3.1.2 all
 * work without being named. See notes/multi_version_support_design.md.
 *
 * SINGLE INSTANCE. The 6502 core (Fake6502) keeps its registers in
 * globals, so exactly one echotalk may exist at a time.
 * echotalk_create() fails if one is already live. This is not a problem
 * for a screen reader, which wants one voice at a time, but it is worth
 * knowing before designing around it.
 *
 * Threading: not thread-safe. Serialise calls, or confine an instance
 * to one thread.
 */
#ifndef ECHOTALK_H
#define ECHOTALK_H

#include <stddef.h>
#include <stdint.h>

/* --- linkage ---
 *
 * Building the shared library defines ECHOTALK_BUILD_DLL; a C or C++
 * consumer that links against the import library defines ECHOTALK_DLL.
 * Neither is needed to compile the sources straight into a program,
 * which is what tools/say.c does, and it is the case that has to keep
 * working with no defines at all.
 *
 * Everything is plain cdecl. On x86-64 there is only one convention; on
 * 32-bit the exported names are undecorated, so ctypes.CDLL finds them
 * by their plain C names on both. */
#if defined(_WIN32)
#  if defined(ECHOTALK_BUILD_DLL)
#    define ECHOTALK_API __declspec(dllexport)
#  elif defined(ECHOTALK_DLL)
#    define ECHOTALK_API __declspec(dllimport)
#  else
#    define ECHOTALK_API
#  endif
#elif defined(ECHOTALK_BUILD_DLL)
#  define ECHOTALK_API __attribute__((visibility("default")))
#else
#  define ECHOTALK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct echotalk echotalk;

/* ABI version of this header, bumped when the exported surface changes
 * in a way a caller could notice. A host that loads the library at
 * runtime -- which is what a screen reader does -- has no compile-time
 * check available, so it should call echotalk_abi_version() and compare
 * against this before anything else. */
#define ECHOTALK_ABI_VERSION 12
ECHOTALK_API unsigned echotalk_abi_version(void);

/* --- lifecycle --- */

/* Loads the images and boots Textalker. On failure returns NULL and,
 * if errbuf is non-NULL, writes a human-readable reason into it. */
ECHOTALK_API echotalk *echotalk_create(const char *loader_path,
                                       const char *obj_path,
                                       char *errbuf, size_t errbuf_len);

/* The same, from an Apple II disk image -- no extraction step at all.
 *
 * Takes a 143,360-byte DOS 3.3 image in either sector ordering (.do,
 * .dsk, .po) or a .2mg wrapper round one, finds Textalker's loader and
 * engine on it, and boots them. Nothing is matched by filename: the
 * loader is recognised by the entry trampoline it carries, the engine
 * by matching a known layout, and the two are paired by the load
 * address the trampoline names. So this works on a 3.1.x build nobody
 * has catalogued, exactly as the two-file form does.
 *
 * Nibble images (.nib, .woz) are not supported and say so by name.
 * `tools/tk_extract` does the same job as a standalone program when the
 * .bin files themselves are what is wanted. */
ECHOTALK_API echotalk *echotalk_create_from_disk(const char *disk_path,
                                                 char *errbuf,
                                                 size_t errbuf_len);

/* The same, asking for one particular version -- "3.1.3", "1.3", or
 * "unknown" for a build the C engine does not recognise (which the
 * emulated engine still runs perfectly). NULL means "the best
 * available", which is what echotalk_create_from_disk() passes.
 *
 * A disk really can carry more than one: one of the test disks has
 * v3.1.3 and v1.3 side by side. */
ECHOTALK_API echotalk *echotalk_create_from_disk_version(
    const char *disk_path, const char *want_version,
    char *errbuf, size_t errbuf_len);

/* The versions on a disk, comma-separated into `out`, so a host can
 * offer each as its own voice. Returns how many, or -1 if the disk
 * could not be read at all -- in which case `out` holds the reason. */
ECHOTALK_API int echotalk_disk_versions(const char *disk_path,
                                        char *out, size_t cap);
ECHOTALK_API void echotalk_destroy(echotalk *et);

/* Version banner read out of the loader, e.g. "3.1.3" or "1.3", or
 * "unknown" if it could not be parsed. Display only -- nothing in this
 * library keys behaviour off it. */
ECHOTALK_API const char *echotalk_version(const echotalk *et);

/* --- settings ---
 *
 * All return 0 on success, -1 if the value is out of range. Settings
 * take effect on the next echotalk_speak(); they do not disturb speech
 * already queued.
 *
 * The four rate-ish controls are deliberately independent:
 *
 *   pitch            pitch only     Textalker's own nP command
 *   speed            speed only     continuous, 0.25 to 4.0 -- USE THIS
 *   frame rate       speed only     TMS5220's own four fixed steps
 *   clock multiplier speed + pitch  the "sped-up tape" character
 *   sample rate      neither        output format only
 */

/* Output sample rate in Hz. The chip is native 8000; anything else is
 * resampled by linear interpolation with no anti-aliasing, which keeps
 * the original grit rather than smoothing it. 0 selects native.
 *
 * HOSTS: the clock multiplier below scales what the chip produces, so
 * the native rate is really 8000 * multiplier -- 12000 at 1.5. Asking
 * for less than that downsamples, and with no anti-aliasing filter
 * everything above the new Nyquist folds back into the audible band
 * rather than being removed. Treat this as a FLOOR and raise it to at
 * least 8000 * multiplier whenever the clock is above 1.0. The library
 * does not do it for you: a host may have an output device it cannot
 * renegotiate, and silently changing the format it asked for would be
 * worse than the aliasing. `say` and the NVDA driver both enforce it. */
ECHOTALK_API int echotalk_set_sample_rate(echotalk *et, unsigned hz);

/* Pretends the TMS5220 runs at a different clock. Changes speed and
 * pitch together, exactly as over/underclocking the real chip would.
 * 1.0 is the real Echo II. Range 0.25 to 4.0. */
ECHOTALK_API int echotalk_set_clock_multiplier(echotalk *et, double multiplier);

/* TMS5220 frame rate, 0-3: 8, 6, 4 or 2 interpolation periods per
 * frame, measuring roughly 1.00x, 1.31x, 1.89x and 3.40x speed with
 * pitch unchanged. 0 is what real Echo II hardware does; the rest are
 * an emulator capability, since the card carries a plain TMS5220 whose
 * SET RATE command is a no-op. Higher rates skip the gentler early
 * interpolation steps, so timbre changes as well as speed. */
ECHOTALK_API int echotalk_set_frame_rate(echotalk *et, int rate);

/* Continuous speech rate, 0.25 to 4.0, default 1.0. Changes speed only;
 * the pitch does not move.
 *
 * This is the one to reach for. The frame rate above is real 5220C
 * behaviour but offers four fixed steps, all at or above normal speed,
 * and the top one is rough -- not enough for a screen reader that wants
 * a smooth range either side of normal.
 *
 * It works by scaling how fast the chip's parameter state machine walks
 * a frame while the lattice filter keeps producing one sample per
 * output sample. Pitch comes from the sample-rate side of that split,
 * so it stays put. 1.0 is off and byte-exact -- the default path is
 * indistinguishable from MAME's.
 *
 * Not something a real Echo II could do, and worth saying so in a UI.
 * Far from 1.0 the interpolation is being stretched or compressed well
 * past anything the chip was designed for, so judge the extremes by
 * ear rather than assuming the whole range is equally usable. */
ECHOTALK_API int echotalk_set_speed(echotalk *et, double speed);

/* Textalker's own two speech rates: 0 expanded (default), 1 compressed.
 * It implements these by skipping phoneme segments rather than by
 * changing playback rate, so this is a different effect from either
 * control above and composes with both. */
ECHOTALK_API int echotalk_set_compressed(echotalk *et, int compressed);

/* Textalker pitch 0-63 (default 24) and volume 0-15 (default 12). */
ECHOTALK_API int echotalk_set_pitch(echotalk *et, int pitch);
ECHOTALK_API int echotalk_set_volume(echotalk *et, int volume);

/* Monotone: 0 (default) speaks with normal intonation, 1 flattens it.
 *
 * This is not a separate Textalker setting from the pitch. Its "nP"
 * command means "pitch n, normal", and "nF" means "pitch n, monotone" --
 * one setting, two spellings. The two are split here because that is
 * how a caller thinks about them, and recombined when the command goes
 * out, so setting one never disturbs the other. */
ECHOTALK_API int echotalk_set_flat(echotalk *et, int flat);

/* How Textalker reads what it is given.
 *
 * letter mode: 0 (default) speaks words, 1 spells them out.
 * punctuation: 0 none, 1 some (default), 2 all -- at 2 even a line
 * terminator is announced, as "return".
 *
 * These are only sent to Textalker once one of them has been called at
 * least once. Left alone, the library assumes Textalker's own startup
 * modes and says nothing about them, because "some" is this code's
 * assumption rather than a measured fact: a lone comma being silent
 * rules out all-punctuation, but does not tell "some" from "none".
 *
 * A one-character utterance briefly switches both, to get a lone letter
 * or punctuation mark announced at all, and puts back whatever these
 * say afterwards -- so a caller's choice survives. */
ECHOTALK_API int echotalk_set_letter_mode(echotalk *et, int letter);
ECHOTALK_API int echotalk_set_punctuation(echotalk *et, int mode);

/* Pause Textalker inserts between words, 0-15 (default 0).
 * Textalker 1.3 does not implement this command at all and silently
 * discards it, so it has no effect there. */
ECHOTALK_API int echotalk_set_word_delay(echotalk *et, int delay);

/* Longest run of text handed to Textalker between CRs, in characters.
 *
 * Textalker speaks when its line buffer fills, and that boundary lands
 * wherever it lands -- including mid-word. Chunking ahead of it is what
 * keeps text splitting at clause and word boundaries instead.
 *
 * THE DEFAULT DEPENDS ON THE ENGINE, and follows it automatically until
 * a host calls this:
 *
 *   emulated            80   nothing here plays the part of DOS setting
 *                            up a screen, so Textalker's own bound is
 *                            uninitialised and it truncates a line at
 *                            79 characters. Measured.
 *   native, 3.1.x      130   the only bound is $DB6A's own flush at 240
 *                            BYTES, and the syllabifier's sentinels cost
 *                            up to 1.8 bytes per character.
 *   native, 1.3        200   no syllabifier, so barely any growth.
 *
 * Calling this pins the value: it then stays where the host put it
 * across engine changes. echotalk_reset_chunk_size() undoes that and
 * returns the value now in force.
 *
 * 0 disables chunking entirely. On the native engine that is merely
 * "let Textalker split it", which it does at 240 bytes and possibly
 * mid-word; on the emulated engine it means losing text past 79
 * characters. Supported for experimentation either way. */
ECHOTALK_API int echotalk_set_chunk_size(echotalk *et, unsigned chars);

/* Stop pinning it: the window goes back to following the engine.
 * Returns the size now in force. */
ECHOTALK_API int echotalk_reset_chunk_size(echotalk *et);

/* EXTENDED LINE BUFFER -- native engine only, and a stated divergence.
 *
 * Textalker's line buffer is 256 bytes and it speaks when 240 of them
 * are used. That is a 1981 constraint, not a property of the speech,
 * and the native engine does not need it -- but it keeps it by default,
 * because reproducing Textalker exactly is what makes the rewrite worth
 * trusting. Three audible behaviours depend on the size, and all three
 * are reproduced deliberately:
 *
 *   - a rule that consumes more characters than are left walks on
 *     through the previous utterance's leftovers until the phoneme cap
 *     stops it;
 *   - that cap, 240 phonemes;
 *   - the one-code lookahead at the end of a phoneme string, which
 *     wraps within the buffer.
 *
 * Ask for more and those stop matching the real machine. Nothing is
 * "broken" -- the speech is still Textalker's -- but the output is no
 * longer byte-identical to the emulation, and the differential tests
 * are run at 256 for that reason.
 *
 * What it buys: THE CHUNK WINDOW SCALES WITH IT. At 1024 bytes a chunk
 * is four times longer, so most paragraphs are spoken in one piece
 * instead of five, and the joins go away.
 *
 * 256 is the default and exact. The maximum is 4096. Takes effect from
 * the next utterance; -1 if the size is out of range or this image has
 * no native engine at all. echotalk_buffer_size() returns 0 in that
 * case, and the size in force otherwise. */
ECHOTALK_API int echotalk_set_buffer_size(echotalk *et, unsigned bytes);
ECHOTALK_API unsigned echotalk_buffer_size(const echotalk *et);
ECHOTALK_API unsigned echotalk_chunk_size(const echotalk *et);

/* Raw mode: 0 (default) prepares text as echotalk_speak() describes,
 * 1 passes bytes to Textalker untouched. Raw mode is how you send
 * Ctrl-E command sequences that contain bytes text preparation would
 * otherwise fold away; note that ordinary Ctrl-E commands survive
 * preparation already and do not need it. */
ECHOTALK_API int echotalk_set_raw(echotalk *et, int raw);

/* Which engine turns text into chip bytes.
 *
 *   ECHOTALK_ENGINE_EMULATED (default) runs the real Textalker binary
 *       under the 6502 emulator, as this library always has.
 *   ECHOTALK_ENGINE_NATIVE runs the C rewrite in src/tk_*.c, which
 *       produces the identical byte stream with no 6502 in it. It reads
 *       its tables out of the same user-supplied image, so it needs no
 *       extra files and changes nothing about what a host must ship.
 *
 *       **EXPERIMENTAL. Its audio is right for most material and not
 *       all of it, and the gap is Ctrl-V phoneme mode.** The chip byte
 *       stream is byte-exact always; what differs is when each byte
 *       reaches the chip, and the chip's output depends on that.
 *
 *       Measured with tools/engine_diff.py over random multi-line
 *       command/phoneme/text streams: v1.3 matches on about 92% of
 *       them, v3.1.3 on about 75%, and EVERY case that differed
 *       contained a Ctrl-V phoneme string -- none of the cases without
 *       one ever did. Whole-file lengths agree to within half a
 *       percent, and the reference corpus is better than the random
 *       material -- `engine_diff.py --refs` reports 8 of 9 for v1.3 and
 *       5 of 9 for v3.1.3, with two more of v3.1.3's differing by one
 *       block in a thousand -- which is exactly why the random material
 *       exists.
 *
 *       The NVDA driver replaces Ctrl-V with a space before the
 *       library sees it, so an add-on cannot reach the failing case;
 *       any other host can.
 *
 * This is ORTHOGONAL to which Textalker version is loaded: the version
 * is decided by the loader/OBJ pair given to echotalk_create(), and
 * both engines handle both versions. A host offering a choice therefore
 * has two independent controls -- which voice (image pair), and which
 * engine.
 *
 * The engine may be changed at any time; it takes effect on the next
 * utterance. Settings, voice and queued text are unaffected.
 *
 * That last sentence takes work, and is worth knowing about if you are
 * reading the code: the two engines each hold their OWN copy of
 * Textalker's voice state, because the settings reach Textalker as
 * Ctrl-E commands in the input stream and that stream goes to one
 * engine at a time. Everything pushed before a switch went to the other
 * engine's Textalker. So a switch marks the settings block dirty and it
 * is sent again, ahead of the next utterance. Before that was done,
 * switching engines mid-session quietly reverted pitch, volume, word
 * delay, the repeat filter and compressed speech to Textalker's own
 * startup values while every getter still reported what the caller had
 * asked for.
 *
 * Returns 0, or -1 if the engine is unknown or unavailable (the native
 * engine needs an image it recognises; echotalk_engine() reports what
 * is actually in force). */
#define ECHOTALK_ENGINE_EMULATED 0
#define ECHOTALK_ENGINE_NATIVE   1
ECHOTALK_API int echotalk_set_engine(echotalk *et, int engine);
ECHOTALK_API int echotalk_engine(const echotalk *et);

/* A pronunciation dictionary for the NATIVE engine.
 *
 * The native engine's letter-to-sound stage is replaceable -- that is
 * what the rewrite exists for -- and this is the first thing that plugs
 * into it. The file is in CMUdict's format, so `cmudict.dict` works
 * unmodified:
 *
 *     NICKNAME  N IH1 K N EY2 M
 *     TOMATO    T AH0 M EY1 T OW0
 *
 * plus two extensions, either of which may replace the ARPABET:
 *
 *     BOOT      =booht          a RESPELLING, run through Textalker's
 *                               own rules -- it says BOOT with the
 *                               vowel of BOOK, and this fixes it
 *     ECHOTALK  /3EKOT3*K       Street Electronics' own Ctrl-V
 *                               notation, for one exact allophone; a
 *                               digit 1-3 sets the pitch group
 *
 * It is an OVERRIDE LIST, not a replacement. Words it does not know,
 * every mark of punctuation and every space still go through
 * Textalker's rules, so the voice, the intonation and the spoken
 * punctuation names are untouched; a dictionary matching nothing gives
 * byte-identical audio to no dictionary at all.
 *
 * ECHOTALK SHIPS NO DICTIONARY. The file is yours, read at run time,
 * exactly as the Textalker image is. Nothing about its licence reaches
 * EchoTalk's.
 *
 * A DICTIONARY BELONGS TO ONE TEXTALKER VERSION, and the format has no
 * way of saying which. The versions do not mispronounce the same words:
 * v3.1.3 says BOOT, ROOT and SHOOT with the vowel of BOOK and v1.3 says
 * all three correctly, so `BOOT =booht` fixes one and costs the other a
 * faint break in the middle of the word. A host offering a choice of
 * dictionaries should expect one per voice.
 *
 * `path` NULL removes the dictionary. Returns 0, or -1 with a message
 * in `errbuf` (which may be NULL). Only the native engine consults it;
 * loading one while the emulated engine is selected is allowed and
 * takes effect if the engine is switched. */
ECHOTALK_API int echotalk_load_dictionary(echotalk *et, const char *path,
                                          char *errbuf, size_t errbuf_len);

/* How many words the dictionary holds, and how many word lookups have
 * hit and missed it since it was loaded. Useful for telling "my
 * dictionary is not being consulted" from "my dictionary does not
 * contain that word", which are otherwise indistinguishable by ear. */
ECHOTALK_API long echotalk_dictionary_entries(const echotalk *et);
ECHOTALK_API long echotalk_dictionary_hits(const echotalk *et);
ECHOTALK_API long echotalk_dictionary_misses(const echotalk *et);

/* Whether an index mark ends the utterance it sits in. 0 (default) keeps
 * speech continuous across marks; 1 is the older behaviour.
 *
 * A mark's position can only be known exactly at an utterance boundary,
 * because Textalker buffers a whole line and emits nothing until the
 * terminating CR -- there is no way to observe which character is being
 * spoken. Ending the utterance at each mark bought that exactness, and
 * cost this: NVDA sends a Say All as ONE sequence with a mark between
 * every line, so a sentence wrapped over several lines was read as
 * several separate sentences.
 *
 * At 0, marks inside an utterance are placed proportionally by character
 * offset -- approximate, and the same thing any synthesiser that keeps
 * speech continuous must do. Marks at an utterance boundary, which is
 * where all but a handful of NVDA's are, stay exact either way. */
ECHOTALK_API int echotalk_set_index_break(echotalk *et, int on);
ECHOTALK_API int echotalk_index_break(const echotalk *et);

/* Threshold for Textalker's repeat-character filter, 0-99.
 *
 * The filter exists so a decorative run like "*****" is not read out
 * one "star" at a time, but it does not distinguish decoration from
 * content: at its default setting "EEEEEEEEE" is spoken as "EE". The
 * default here is 99, high enough that it never triggers, which is
 * almost certainly what a screen reader wants. Lower it to get
 * Textalker's original behaviour back. */
ECHOTALK_API int echotalk_set_repeat_filter(echotalk *et, int threshold);

/* --- reading settings back ---
 *
 * The library watches text on its way to Textalker for Ctrl-E commands
 * and mirrors them here, so these report the voice actually in force
 * whether it was set through the calls above or by a command embedded
 * in the text. The commands still reach Textalker untouched; this only
 * stops the two from drifting apart.
 *
 * Every value returned is one its matching setter accepts, so a voice
 * can be carried across to another instance by reading here and writing
 * there. That is what makes offering both Textalker versions as voice
 * variants workable: loading the other version means a fresh 6502 and a
 * fresh Textalker at ITS defaults, and the only way to restore the
 * voice is to know what it was.
 *
 *     int p = echotalk_pitch(old), v = echotalk_volume(old);
 *     echotalk_destroy(old);
 *     echotalk *new = echotalk_create(other_loader, other_obj, e, sizeof e);
 *     echotalk_set_pitch(new, p); echotalk_set_volume(new, v);
 *
 * One limit: Textalker does something of its own with values outside
 * the documented ranges -- "\x05 99P" is audibly not "\x05 63P" -- and
 * a sniffed value is clamped to what the setters accept. Replaying
 * out-of-spec input therefore is not bit-exact. Keeping getter and
 * setter agreeing was judged worth more. */
ECHOTALK_API int      echotalk_pitch(const echotalk *et);
ECHOTALK_API int      echotalk_flat(const echotalk *et);
ECHOTALK_API int      echotalk_volume(const echotalk *et);
ECHOTALK_API int      echotalk_word_delay(const echotalk *et);
ECHOTALK_API int      echotalk_repeat_filter(const echotalk *et);
ECHOTALK_API int      echotalk_compressed(const echotalk *et);
ECHOTALK_API int      echotalk_letter_mode(const echotalk *et);
ECHOTALK_API int      echotalk_punctuation(const echotalk *et);
ECHOTALK_API int      echotalk_frame_rate(const echotalk *et);
ECHOTALK_API double   echotalk_clock_multiplier(const echotalk *et);
ECHOTALK_API double   echotalk_speed(const echotalk *et);
ECHOTALK_API unsigned echotalk_sample_rate(const echotalk *et);
ECHOTALK_API int      echotalk_raw(const echotalk *et);

/* --- speaking --- */

/* Queues `text` to be spoken. Returns 0 on success.
 *
 * Accepts UTF-8 or legacy single-byte text, detected per character, and
 * reduces it to the 7-bit ASCII Textalker understands. Long lines are
 * split at clause then word boundaries so Textalker's own buffer never
 * decides where to break, which it would otherwise sometimes do
 * mid-word.
 *
 * THIS DOES NOT SYNTHESISE. It returns as soon as the text is copied;
 * echotalk_read() synthesises one utterance at a time, on demand, so
 * the first chunk can be playing while the rest is still being made.
 * echotalk_available() is therefore 0 immediately after this call --
 * use echotalk_pending() to ask whether there is work outstanding, and
 * treat a 0 return from echotalk_read() as the end of speech.
 *
 * Calling it again before the queue drains appends; it does not
 * interrupt. Use echotalk_stop() to abandon what is queued.
 *
 * --- Ctrl-D driver commands ---
 *
 * The text may contain driver commands introduced by Ctrl-D (0x04),
 * deliberately echoing the shape of Textalker's own Ctrl-E commands:
 * an optional number, then a letter.
 *
 *   \x04 2F     frame rate 2          \x04F   frame rate back to default
 *   \x04 1.5S   speed, pitch kept     \x04S   speed back to 1.0
 *   \x04 0.75C  clock multiplier      \x04C   clock back to 1.0
 *   \x04 0B     chunking off          \x04 80B  chunk at 80 characters
 *   \x04 1R     raw text on           \x04 0R   raw text off
 *   \x04 7I     index mark 7          (no default; a bare I is an error)
 *   \x04\x04    one literal 0x04 byte, spoken rather than obeyed
 *
 * The namespaces are disjoint on purpose: Ctrl-E addresses the 1985
 * synthesiser, Ctrl-D addresses the driver around it. Ctrl-E sequences
 * survive text preparation untouched, so there is no need to duplicate
 * them here. Note that Ctrl-E's own letters differ -- Ctrl-E F is
 * flatness and Ctrl-E C is compressed, where Ctrl-D F is frame rate and
 * Ctrl-D C is clock.
 *
 * A command ENDS THE CURRENT UTTERANCE. Textalker buffers a whole line
 * and does not synthesise anything until the terminating CR arrives, so
 * a command's position in the text would otherwise bear no relation to
 * its position in the audio: applying it the moment it is seen would
 * apply it to everything already buffered. Flushing first means what
 * precedes the command speaks at the old settings and what follows it
 * at the new. The cost is an utterance boundary, and hence a small
 * pause, wherever a command appears.
 *
 * Settings changed this way PERSIST past the end of this call, exactly
 * as Ctrl-E commands persist inside Textalker.
 *
 * A malformed or unknown command is swallowed, never spoken -- a screen
 * reader reading its own control codes aloud would be worse than the
 * command being ignored -- and counted, see echotalk_command_errors(). */
ECHOTALK_API int echotalk_speak(echotalk *et, const char *text);

/* Number of malformed or unknown Ctrl-D commands seen since the last
 * call to echotalk_clear_command_errors(). Since bad commands are
 * silently dropped, this is the only way a host can notice a typo. */
ECHOTALK_API unsigned echotalk_command_errors(const echotalk *et);

/* How many times a runaway guard has tripped.
 *
 * NON-ZERO IS ALWAYS A FAULT and the audio for those utterances is
 * wrong. The guards stop a wild jump or a stuck poll from hanging the
 * host, and are sized well above anything Textalker legitimately needs;
 * tripping one means the 6502 was cut off part-way through a routine and
 * left in a state the next character is then entered on top of.
 *
 * This exists because that used to happen in complete silence. A host
 * should log a non-zero value rather than ignore it. */
ECHOTALK_API unsigned echotalk_overruns(const echotalk *et);
ECHOTALK_API void echotalk_clear_command_errors(echotalk *et);

/* Copies up to `frames` samples of 16-bit mono PCM into `out` and
 * returns how many were written. Synthesises on demand when the queue
 * runs dry, so a 0 return means the utterance is finished and drained,
 * not merely that nothing is ready yet.
 *
 * Synthesis measures around 136x real time, so calling this straight
 * from an audio callback has ample headroom. */
ECHOTALK_API size_t echotalk_read(echotalk *et, int16_t *out, size_t frames);

/* Samples already synthesised and waiting to be read. This is NOT how
 * much speech is left -- see echotalk_pending(). */
ECHOTALK_API size_t echotalk_available(const echotalk *et);

/* Bytes of queued text not yet turned into audio. Zero together with a
 * zero echotalk_available() means everything has been spoken and read. */
ECHOTALK_API size_t echotalk_pending(const echotalk *et);

/* Synthesises until at least `min_samples` are queued, or until the
 * text runs out, and returns echotalk_available(). For a host that
 * would rather run synthesis on its own thread than inside its audio
 * callback: call this from that thread, echotalk_read() from the other,
 * and serialise the two -- the library is not thread-safe. */
ECHOTALK_API size_t echotalk_synthesize(echotalk *et, size_t min_samples);

/* --- index events ---
 *
 * An index mark is placed with the Ctrl-D I command, e.g. "\x04 7I".
 * Each becomes an event that is ready once echotalk_read() has handed
 * out the audio preceding it, which is what lets a host report progress
 * through an utterance.
 *
 * These are exact rather than estimated: the mark's position is the
 * sample count at the moment the text before it finished synthesising,
 * and we generated every one of those samples.
 *
 * The cost is that an index mark ENDS THE CURRENT UTTERANCE, like every
 * other Ctrl-D command -- a position inside an utterance is not
 * knowable until it has been spoken, by which time it is too late to
 * split. Marking at clause or sentence granularity is therefore free,
 * since the chunker breaks there anyway; marking every word will make
 * Textalker's prosody noticeably choppier. */

/* Pops the oldest index event whose audio has been read. Returns 1 and
 * writes the index into *index, or 0 if none is ready.
 *
 * Drain it in a loop after each echotalk_read(), INCLUDING the one that
 * returns 0: a mark at the very end of the text only becomes ready on
 * that last call, and an end-of-speech marker is exactly what a host is
 * most likely to put there. */
ECHOTALK_API int echotalk_next_index(echotalk *et, int *index);

/* Abandons everything: queued audio, text not yet synthesised, and any
 * index events still outstanding, which would otherwise fire against
 * audio nobody is going to hear. This is what a screen reader calls
 * when the user moves on.
 *
 * Textalker's own state is untouched and needs no attention, because
 * synthesis always runs to the end of an utterance before returning --
 * there is never anything in flight to interrupt. Subsequent speech
 * sounds the same as if this had not been called. */
ECHOTALK_API void echotalk_stop(echotalk *et);

#ifdef __cplusplus
}
#endif

#endif /* ECHOTALK_H */
