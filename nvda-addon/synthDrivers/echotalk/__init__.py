# EchoTalk (emulated Apple II Echo II) synth driver for NVDA.
#
# Drives echotalk.dll -- a 6502 emulation of Street Electronics' Textalker
# running a port of MAME's TMS5220 -- as an NVDA speech synthesizer.
#
# Files expected next to this __init__.py:
#   echotalk64.dll / echotalk32.dll   built with `make dll`
#   <name>.ram.bin + <name>.obj.bin   one pair per Textalker version.
#                                     Proprietary to Street Electronics
#                                     (3.1.3 also carries an American
#                                     Printing House for the Blind
#                                     copyright) and NOT distributed with
#                                     this add-on. Supply your own.
#
# Each image pair becomes a voice, labelled with the version banner read
# out of the loader itself, so a pair this code has never heard of still
# shows up under its own name.

import ctypes
import math
import os
import re
import threading
import queue

# ctypes never unloads a library, so without an explicit FreeLibrary every
# switch to this synth bumps the loader refcount again and the image stays
# mapped for the life of the NVDA process. The public spelling
# (windll.kernel32.FreeLibrary) truncates the HMODULE on 64-bit unless its
# argtypes are set by hand, so use the private one. Borrowed, with the
# reasoning, from David Sexton's doubletalkpc driver.
from _ctypes import FreeLibrary

import config
import nvwave
from autoSettingsUtils.driverSetting import (
	BooleanDriverSetting, DriverSetting, NumericDriverSetting)
from autoSettingsUtils.utils import StringParameterInfo
from synthDriverHandler import (
	SynthDriver, VoiceInfo, synthIndexReached, synthDoneSpeaking)
from speech.commands import IndexCommand, PitchCommand
from logHandler import log

_DIR = os.path.dirname(__file__)

SAMPLES_PER_READ = 1024

# The ABI this driver was written against. echotalk_abi_version() is the
# only check available to a host that loads the library at runtime.
REQUIRED_ABI = 4

# The library's own ranges, which every slider below converts to and from.
PITCH_MAX = 63          # Textalker nP
VOLUME_MAX = 15         # Textalker nV
DELAY_MAX = 15          # Textalker nD
REPEAT_MAX = 99         # Textalker nR

# Defaults in LIBRARY units: what a real Echo II comes up at, not the
# middle of a slider. Converted to slider positions for defaultVal below.
DEFAULT_PITCH = 24
DEFAULT_VOLUME = 12
DEFAULT_DELAY = 0
DEFAULT_REPEAT = 99     # high enough that the filter never triggers
DEFAULT_SAMPLERATE = "8000"

# Bytes the pipeline would act on rather than speak: Ctrl-D introduces a
# driver command, Ctrl-E a Textalker command, Ctrl-V phoneme mode. They
# have to be stripped from anything that came off the screen, or a
# document containing one silently changes the voice -- measured, not
# theoretical. Everything else is left alone so the library's own text
# preparation can still fold UTF-8 and typographic punctuation properly,
# which it does better than a driver could.
_UNSAFE = re.compile(r"[\x04\x05\x16]")


def _toCard(pct, maxVal):
	"""NVDA's 0-100 slider -> a library value."""
	return max(0, min(maxVal, int(round(pct * maxVal / 100.0))))


def _toPct(card, maxVal):
	"""The inverse. NVDA re-reads a setting through its getter right after
	setting it, so a mapping that does not round-trip makes the slider
	jump under the user's fingers."""
	return max(0, min(100, int(round(card * 100.0 / maxVal))))


# Rate and clock are multipliers from 0.25x to 4x. A linear slider would
# put 1.0 at 20% and spend most of its travel on speeds nobody wants, so
# map them logarithmically: two octaves either side of normal, 50% is
# exactly 1.0, every 25% doubles. That also puts the finest control where
# it gets used, around normal speed.
def _toMult(pct):
	return 2.0 ** ((pct - 50) / 25.0)


def _toMultPct(mult):
	return max(0, min(100, int(round(50 + 25 * math.log(mult, 2)))))


def _dllName():
	# NVDA 2025.2+ runs as a 64-bit process; earlier versions are 32-bit.
	return "echotalk64.dll" if ctypes.sizeof(ctypes.c_void_p) == 8 else "echotalk32.dll"


def _imagePairs():
	"""Finds Textalker image pairs sitting next to this file.

	A pair is <stem>.ram.bin (or <stem>.loader.bin) plus <stem>.obj.bin,
	matching the names the project's own roms/ directory uses. Returns
	[(stem, loader, obj)], sorted so the voice order is stable.
	"""
	found = []
	try:
		names = sorted(os.listdir(_DIR))
	except OSError:
		return found
	for name in names:
		for suffix in (".ram.bin", ".loader.bin"):
			if not name.endswith(suffix):
				continue
			stem = name[:-len(suffix)]
			obj = os.path.join(_DIR, stem + ".obj.bin")
			if os.path.isfile(obj):
				found.append((stem, os.path.join(_DIR, name), obj))
			break
	return found


# Settings are built from NVDA's own factories where it has them, so they
# keep NVDA's translated display names and standard keyboard handling,
# then adjusted. defaultVal IS settable -- NVDA does not insist on 50% --
# which is what lets the Echo's real defaults be the starting point.
#
# minStep is chosen per setting so that one arrow-key press always changes
# the library value. Volume and word delay have only 16 steps across the
# slider, so a 1% press would often do nothing at all.
def _rateSetting():
	s = SynthDriver.RateSetting(minStep=2)      # ~5.7% speed per press
	s.defaultVal = _toMultPct(1.0)              # 50 -> 1.0x
	return s


def _pitchSetting():
	s = SynthDriver.PitchSetting(minStep=2)
	s.defaultVal = _toPct(DEFAULT_PITCH, PITCH_MAX)
	return s


def _volumeSetting():
	s = SynthDriver.VolumeSetting(minStep=7)
	s.defaultVal = _toPct(DEFAULT_VOLUME, VOLUME_MAX)
	return s


class _EchoTalkDLL:
	"""Loads the library and declares every signature.

	Declaring argtypes is not optional detail: without them ctypes guesses
	from the Python value, which happens to work for ints on 64-bit and
	breaks silently for doubles and for pointers above 2GB.
	"""

	def __init__(self):
		self.lib = ctypes.cdll.LoadLibrary(os.path.join(_DIR, _dllName()))
		p = ctypes.c_void_p
		lib = self.lib

		lib.echotalk_abi_version.restype = ctypes.c_uint
		lib.echotalk_abi_version.argtypes = []
		lib.echotalk_create.restype = p
		lib.echotalk_create.argtypes = [
			ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
		lib.echotalk_destroy.restype = None
		lib.echotalk_destroy.argtypes = [p]
		lib.echotalk_version.restype = ctypes.c_char_p
		lib.echotalk_version.argtypes = [p]

		for fname, arg in (
			("set_pitch", ctypes.c_int), ("set_flat", ctypes.c_int),
			("set_volume", ctypes.c_int), ("set_word_delay", ctypes.c_int),
			("set_repeat_filter", ctypes.c_int), ("set_compressed", ctypes.c_int),
			("set_frame_rate", ctypes.c_int), ("set_raw", ctypes.c_int),
			("set_letter_mode", ctypes.c_int), ("set_punctuation", ctypes.c_int),
			("set_speed", ctypes.c_double),
			("set_clock_multiplier", ctypes.c_double),
			("set_sample_rate", ctypes.c_uint),
			("set_chunk_size", ctypes.c_uint),
		):
			fn = getattr(lib, "echotalk_" + fname)
			fn.restype, fn.argtypes = ctypes.c_int, [p, arg]

		lib.echotalk_sample_rate.restype = ctypes.c_uint
		lib.echotalk_sample_rate.argtypes = [p]
		lib.echotalk_speak.restype = ctypes.c_int
		lib.echotalk_speak.argtypes = [p, ctypes.c_char_p]
		lib.echotalk_read.restype = ctypes.c_size_t
		lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
		lib.echotalk_available.restype = ctypes.c_size_t
		lib.echotalk_available.argtypes = [p]
		lib.echotalk_pending.restype = ctypes.c_size_t
		lib.echotalk_pending.argtypes = [p]
		lib.echotalk_next_index.restype = ctypes.c_int
		lib.echotalk_next_index.argtypes = [p, ctypes.POINTER(ctypes.c_int)]
		lib.echotalk_stop.restype = None
		lib.echotalk_stop.argtypes = [p]

		abi = lib.echotalk_abi_version()
		if abi != REQUIRED_ABI:
			self.close()
			raise RuntimeError(
				"echotalk DLL reports ABI %d, this driver needs %d" % (abi, REQUIRED_ABI))

	def close(self):
		# Never free the same HMODULE twice: that underflows the loader
		# refcount, and if Windows has recycled the handle value since, it
		# unloads somebody else's module.
		if self.lib:
			FreeLibrary(self.lib._handle)
			self.lib = None


class SynthDriver(SynthDriver):
	name = "echotalk"
	description = "EchoTalk (emulated Echo II)"

	supportedSettings = (
		SynthDriver.VoiceSetting(),
		# Rate drives the library's continuous speed control, which changes
		# speed without moving the pitch. The chip's own four-step frame
		# rate is deliberately NOT exposed: this sounds better and is smooth.
		_rateSetting(),
		_pitchSetting(),
		_volumeSetting(),
		# Textalker 1.3 never implemented the nD command and discards it, so
		# say so in the label rather than leaving a control that looks broken.
		NumericDriverSetting("worddelay", "Delay between &words (Textalker 3 only)",
			availableInSettingsRing=True,
			defaultVal=_toPct(DEFAULT_DELAY, DELAY_MAX), minStep=7),
		# At Textalker's own setting a run of identical characters is
		# collapsed, so "EEEEEEEEE" is spoken as "EE" -- wrong for a screen
		# reader, hence a default high enough that it never triggers.
		NumericDriverSetting("repeatfilter", "&Repeat-character filter",
			availableInSettingsRing=True,
			defaultVal=_toPct(DEFAULT_REPEAT, REPEAT_MAX), minStep=5),
		# Over/underclocking the speech chip: speed AND pitch together, the
		# sped-up-tape effect. Quite different from Rate, which leaves the
		# pitch alone.
		NumericDriverSetting("clock", "&Chip clock",
			availableInSettingsRing=True, defaultVal=_toMultPct(1.0), minStep=2),
		DriverSetting("samplerate", "Output &sample rate",
			availableInSettingsRing=True, defaultVal=DEFAULT_SAMPLERATE),
		BooleanDriverSetting("monotone", "&Monotone",
			availableInSettingsRing=True, defaultVal=False),
		BooleanDriverSetting("compressed", "C&ompressed speech",
			availableInSettingsRing=True, defaultVal=False),
	)
	supportedCommands = {IndexCommand, PitchCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	# The chip is native 8 kHz; everything else is resampled by linear
	# interpolation with no anti-aliasing, which keeps the original grit
	# rather than smoothing it into a nicer DAC than the Echo II ever had.
	# Higher rates add no detail -- they are here because some output
	# devices are happier at their own native rate.
	_sampleRates = {
		"8000": StringParameterInfo("8000", "8 kHz (native)"),
		"11025": StringParameterInfo("11025", "11 kHz"),
		"16000": StringParameterInfo("16000", "16 kHz"),
		"22050": StringParameterInfo("22050", "22 kHz"),
		"32000": StringParameterInfo("32000", "32 kHz"),
		"44100": StringParameterInfo("44100", "44 kHz"),
		"48000": StringParameterInfo("48000", "48 kHz"),
	}

	@classmethod
	def check(cls):
		return os.path.isfile(os.path.join(_DIR, _dllName())) and bool(_imagePairs())

	def __init__(self):
		self._dll = _EchoTalkDLL()
		self._lib = self._dll.lib
		self._handle = None
		# Serialises every call into the (not thread-safe) library handle
		# across the main thread and the synthesis thread.
		self._libLock = threading.Lock()
		self._player = None

		self._voices = {}
		self._pairs = {}
		for stem, loader, obj in _imagePairs():
			banner = self._probeBanner(loader, obj)
			if banner is None:
				continue
			self._pairs[stem] = (loader, obj)
			self._voices[stem] = VoiceInfo(stem, "Textalker %s" % banner)
		if not self._voices:
			self._dll.close()
			raise RuntimeError("no usable Textalker image pairs found next to the driver")

		# Settings in library units. NVDA overwrites these from its saved
		# config where there is one; these are what a fresh profile gets.
		self._pitch_c = DEFAULT_PITCH
		self._volume_c = DEFAULT_VOLUME
		self._delay_c = DEFAULT_DELAY
		self._repeat_c = DEFAULT_REPEAT
		self._speed = 1.0
		self._clock = 1.0
		self._monotone = False
		self._compressed = False
		self._samplerate = DEFAULT_SAMPLERATE
		self._voice = self._defaultVoice()

		self._queue = queue.Queue()
		self._stopping = threading.Event()
		self._openVoice(self._voice)

		self._thread = threading.Thread(target=self._synthLoop, daemon=True)
		self._thread.start()
		super().__init__()

	# --- lifecycle -----------------------------------------------------

	def _probeBanner(self, loader, obj):
		"""Creates an instance just long enough to read its version banner.

		Costs well under a millisecond, and it is the only way to label a
		voice with what the images ARE rather than what they are called.
		Only one instance may exist at a time -- the 6502 core keeps its
		registers in globals -- so this must destroy before returning.
		"""
		err = ctypes.create_string_buffer(256)
		h = self._lib.echotalk_create(
			loader.encode("mbcs"), obj.encode("mbcs"), err, len(err))
		if not h:
			log.debugWarning("echotalk: %s unusable: %s"
				% (os.path.basename(loader), err.value.decode("mbcs", "replace")))
			return None
		try:
			banner = self._lib.echotalk_version(h).decode("ascii", "replace")
		finally:
			self._lib.echotalk_destroy(h)
		return banner or "unknown"

	def _defaultVoice(self):
		"""Textalker 3.x if it is there, otherwise whatever is.

		A user with only 1.3 installed gets a working synth rather than an
		error, and with one pair present NVDA shows a single voice that
		cannot be switched away from.
		"""
		for stem, info in self._voices.items():
			version = info.displayName.rsplit(" ", 1)[-1]
			if version.split(".")[0] == "3":
				return stem
		return next(iter(self._voices))

	def _openVoice(self, stem):
		"""Tears down the current instance and boots the requested images.

		A voice change really is a new machine: the 6502 and all of its
		memory are reset, so Textalker comes back at ITS defaults. That is
		why every setting is pushed again here rather than assumed to have
		survived. Booting costs well under 10 ms, so this is cheap enough
		to do on a settings change.
		"""
		loader, obj = self._pairs[stem]
		err = ctypes.create_string_buffer(256)
		with self._libLock:
			if self._handle:
				self._lib.echotalk_destroy(self._handle)
				self._handle = None
			h = self._lib.echotalk_create(
				loader.encode("mbcs"), obj.encode("mbcs"), err, len(err))
			if not h:
				raise RuntimeError("echotalk_create: %s"
					% err.value.decode("mbcs", "replace"))
			self._handle = h
		self._applyAll()
		self._openPlayer()

	def _openPlayer(self):
		rate = int(self._samplerate)
		if self._player is not None:
			if getattr(self._player, "samplesPerSec", None) == rate:
				return
			self._player.close()
			self._player = None
		# NVDA 2025.1 moved the output device setting from the "speech"
		# config section to "audio" (and changed its value from a device name
		# to an endpoint ID). Each era's WavePlayer expects its own era's
		# value, so read whichever key this NVDA has.
		try:
			outputDevice = config.conf["audio"]["outputDevice"]
		except KeyError:
			outputDevice = config.conf["speech"]["outputDevice"]
		self._player = nvwave.WavePlayer(
			channels=1, samplesPerSec=rate, bitsPerSample=16,
			outputDevice=outputDevice)

	def terminate(self):
		self.cancel()
		self._queue.put(None)
		self._thread.join(timeout=5)
		if self._player:
			self._player.close()
			self._player = None
		# Only unload if the synthesis thread is really gone. It caches the
		# handle in a local, and _libLock does not help: a worker parked on
		# the lock would acquire it the moment close() returned and call
		# straight into a freed image. A timed-out join leaks a daemon
		# thread; unloading anyway would turn that leak into an access
		# violation that takes NVDA with it.
		alive = self._thread.is_alive()
		if alive:
			log.warning("EchoTalk synthesis thread still running; leaving the DLL loaded")
		with self._libLock:
			if self._handle:
				self._lib.echotalk_destroy(self._handle)
				self._handle = None
			if not alive:
				self._dll.close()

	# --- settings -------------------------------------------------------

	def _applyAll(self):
		"""Pushes every setting into the library."""
		with self._libLock:
			lib, h = self._lib, self._handle
			if not h:
				return
			lib.echotalk_set_pitch(h, self._pitch_c)
			lib.echotalk_set_flat(h, 1 if self._monotone else 0)
			lib.echotalk_set_volume(h, self._volume_c)
			lib.echotalk_set_word_delay(h, self._delay_c)
			lib.echotalk_set_repeat_filter(h, self._repeat_c)
			lib.echotalk_set_compressed(h, 1 if self._compressed else 0)
			lib.echotalk_set_speed(h, self._speed)
			lib.echotalk_set_clock_multiplier(h, self._clock)
			lib.echotalk_set_sample_rate(h, int(self._samplerate))

	def _push(self, fname, value):
		with self._libLock:
			if self._handle:
				getattr(self._lib, "echotalk_" + fname)(self._handle, value)

	def _getAvailableVoices(self):
		return self._voices

	def _get_voice(self):
		return self._voice

	def _set_voice(self, value):
		if value not in self._voices or value == self._voice:
			return
		# Switching images tears the machine down, so stop first: otherwise
		# the synthesis thread is reading from a handle about to be freed.
		self.cancel()
		previous = self._voice
		self._voice = value
		try:
			self._openVoice(value)
		except Exception:
			log.error("EchoTalk could not switch voice; staying on %s" % previous,
				exc_info=True)
			self._voice = previous
			self._openVoice(previous)

	def _get_rate(self):
		return _toMultPct(self._speed)

	def _set_rate(self, value):
		self._speed = _toMult(value)
		self._push("set_speed", ctypes.c_double(self._speed))

	def _get_pitch(self):
		return _toPct(self._pitch_c, PITCH_MAX)

	def _set_pitch(self, value):
		self._pitch_c = _toCard(value, PITCH_MAX)
		self._push("set_pitch", self._pitch_c)

	def _get_volume(self):
		return _toPct(self._volume_c, VOLUME_MAX)

	def _set_volume(self, value):
		self._volume_c = _toCard(value, VOLUME_MAX)
		self._push("set_volume", self._volume_c)

	def _get_worddelay(self):
		return _toPct(self._delay_c, DELAY_MAX)

	def _set_worddelay(self, value):
		self._delay_c = _toCard(value, DELAY_MAX)
		self._push("set_word_delay", self._delay_c)

	def _get_repeatfilter(self):
		return _toPct(self._repeat_c, REPEAT_MAX)

	def _set_repeatfilter(self, value):
		self._repeat_c = _toCard(value, REPEAT_MAX)
		self._push("set_repeat_filter", self._repeat_c)

	def _get_clock(self):
		return _toMultPct(self._clock)

	def _set_clock(self, value):
		self._clock = _toMult(value)
		self._push("set_clock_multiplier", ctypes.c_double(self._clock))

	def _get_availableSamplerates(self):
		return self._sampleRates

	def _get_samplerate(self):
		return self._samplerate

	def _set_samplerate(self, value):
		if value not in self._sampleRates or value == self._samplerate:
			return
		# The output format is changing, so the player has to be rebuilt.
		# Stop before touching it: feeding a player that is about to close
		# is a crash waiting for a slow machine.
		self.cancel()
		self._samplerate = value
		self._push("set_sample_rate", ctypes.c_uint(int(value)))
		self._openPlayer()

	def _get_monotone(self):
		return self._monotone

	def _set_monotone(self, value):
		self._monotone = bool(value)
		self._push("set_flat", 1 if self._monotone else 0)

	def _get_compressed(self):
		return self._compressed

	def _set_compressed(self, value):
		self._compressed = bool(value)
		self._push("set_compressed", 1 if self._compressed else 0)

	# --- speech ---------------------------------------------------------

	def speak(self, speechSequence):
		parts = []
		for item in speechSequence:
			if isinstance(item, str):
				parts.append(_UNSAFE.sub(" ", item))
			elif isinstance(item, IndexCommand):
				# The library takes the index value directly -- no rolling
				# 0-99 map is needed, unlike a real card with a two-digit
				# command field.
				parts.append("\x04%dI" % item.index)
			elif isinstance(item, PitchCommand):
				# NVDA raises the pitch before a capital and resets after.
				# offset is in NVDA slider units; 0 means "back to base".
				if item.offset == 0:
					card = self._pitch_c
				else:
					card = _toCard(
						min(100, max(0, self._get_pitch() + item.offset)), PITCH_MAX)
				# P and F are one Textalker setting, not two: pitch with
				# normal intonation, or the same pitch monotone. Emitting P
				# here would quietly un-flatten a monotone voice.
				parts.append("\x05%d%s" % (card, "F" if self._monotone else "P"))
		self._queue.put("".join(parts))

	def cancel(self):
		self._stopping.set()
		try:
			while True:
				self._queue.get_nowait()
		except queue.Empty:
			pass
		with self._libLock:
			if self._handle:
				# Abandons queued audio, text not yet synthesised, AND index
				# events still outstanding -- which would otherwise fire
				# against audio nobody is going to hear.
				self._lib.echotalk_stop(self._handle)
		if self._player:
			self._player.stop()

	def pause(self, switch):
		if self._player:
			self._player.pause(switch)

	# --- synthesis thread ------------------------------------------------

	def _synthLoop(self):
		buf = (ctypes.c_int16 * SAMPLES_PER_READ)()
		idx = ctypes.c_int()
		while True:
			text = self._queue.get()
			if text is None:
				return
			try:
				self._stopping.clear()
				with self._libLock:
					if not self._handle:
						continue
					self._lib.echotalk_speak(self._handle, text.encode("utf-8"))
				while not self._stopping.is_set():
					marks = []
					data = b""
					with self._libLock:
						h = self._handle
						if not h:
							break
						# read() is where synthesis happens -- speak() only
						# queued the text. At around 100x real time there is
						# ample headroom to do it on this thread.
						n = self._lib.echotalk_read(h, buf, SAMPLES_PER_READ)
						if n:
							data = ctypes.string_at(buf, n * 2)
						# Marks whose audio lies inside the block just read.
						# Drain after EVERY read, including the one returning
						# 0: a mark at the very end of the text only becomes
						# ready then, and an end-of-speech marker is exactly
						# what a host is most likely to put there.
						while self._lib.echotalk_next_index(h, ctypes.byref(idx)):
							marks.append(idx.value)
					if not data:
						if marks:
							# Trailing marks fire once the audio really has
							# played, not when the buffer ran dry.
							self._player.idle()
							for m in marks:
								synthIndexReached.notify(synth=self, index=m)
						break

					def onDone(fired=marks):
						for m in fired:
							synthIndexReached.notify(synth=self, index=m)

					# 16-bit signed little-endian mono is exactly what
					# WavePlayer wants, so the bytes go straight through.
					self._player.feed(data, onDone=onDone if marks else None)
				if not self._stopping.is_set():
					self._player.idle()
					synthDoneSpeaking.notify(synth=self)
			except Exception:
				log.error("EchoTalk synthesis thread", exc_info=True)
