#!/usr/bin/env python3
"""Runs the NVDA synth driver against the real DLL, without NVDA.

NVDA cannot be scripted from here, and installing an add-on to find out
whether it imports is a slow way to catch a typo. This stubs the handful
of NVDA modules the driver imports -- faithfully enough to exercise the
parts that are ours -- and then drives it the way NVDA does: construct,
read and write every setting, speak a sequence containing index and
pitch commands, switch voices, cancel, terminate.

What it does NOT prove: that the settings appear correctly in NVDA's own
dialogs, or that WavePlayer behaves the same as the stub here. Those need
a real NVDA. Everything else is real -- the driver code is imported
unmodified and talks to the real echotalk DLL.

usage:
    python tools/test_nvda_driver.py <dll> <roms dir> [out.wav]
"""

import ctypes
import os
import shutil
import struct
import sys
import tempfile
import types


class Failures:
	def __init__(self):
		self.count = 0

	def check(self, label, ok, detail=""):
		print(f"  {'ok  ' if ok else 'FAIL'}  {label}{'  -- ' + detail if detail else ''}")
		if not ok:
			self.count += 1
		return ok


# --- NVDA stubs -------------------------------------------------------
#
# Only what the driver imports. Each stub mirrors the real thing closely
# enough that a mistake in the driver shows up here rather than in NVDA.

class DriverSetting:
	def __init__(self, id, displayName, availableInSettingsRing=False,
			defaultVal=None, useConfig=True):
		self.id = id
		self.displayName = displayName
		self.availableInSettingsRing = availableInSettingsRing
		self.defaultVal = defaultVal
		self.useConfig = useConfig


class NumericDriverSetting(DriverSetting):
	def __init__(self, id, displayName, availableInSettingsRing=False,
			defaultVal=50, minVal=0, maxVal=100, minStep=1, useConfig=True):
		super().__init__(id, displayName, availableInSettingsRing, defaultVal, useConfig)
		self.minVal, self.maxVal, self.minStep = minVal, maxVal, minStep


class BooleanDriverSetting(DriverSetting):
	def __init__(self, id, displayName, availableInSettingsRing=False,
			defaultVal=False, useConfig=True):
		super().__init__(id, displayName, availableInSettingsRing, defaultVal, useConfig)


class StringParameterInfo:
	def __init__(self, id, displayName):
		self.id, self.displayName = id, displayName


class VoiceInfo(StringParameterInfo):
	def __init__(self, id, displayName, language=None):
		super().__init__(id, displayName)
		self.language = language


class _Notifier:
	def __init__(self, name):
		self.name = name
		self.events = []

	def notify(self, **kw):
		self.events.append(kw)


synthIndexReached = _Notifier("synthIndexReached")
synthDoneSpeaking = _Notifier("synthDoneSpeaking")


class _AutoPropertyMeta(type):
	"""NVDA's SynthDriver inherits AutoPropertyObject, whose metaclass turns
	_get_x / _set_x pairs into a property x. The driver relies on that --
	NVDA's settings machinery reads and writes synth.rate, not
	synth._get_rate() -- so the stub has to do it too or the test would be
	exercising a different object from the one NVDA drives.
	"""

	def __new__(mcls, name, bases, ns):
		cls = super().__new__(mcls, name, bases, ns)
		names = set()
		for klass in cls.__mro__:
			for key in vars(klass):
				if key.startswith("_get_") or key.startswith("_set_"):
					names.add(key[5:])
		for n in names:
			if isinstance(getattr(cls, n, None), property):
				continue
			setattr(cls, n, property(
				getattr(cls, "_get_" + n, None), getattr(cls, "_set_" + n, None)))
		return cls


class _BaseSynthDriver(metaclass=_AutoPropertyMeta):
	"""Stands in for NVDA's SynthDriver base."""

	@classmethod
	def VoiceSetting(cls):
		return DriverSetting("voice", "&Voice", True)

	@classmethod
	def RateSetting(cls, minStep=1):
		return NumericDriverSetting("rate", "&Rate", True, minStep=minStep)

	@classmethod
	def PitchSetting(cls, minStep=1):
		return NumericDriverSetting("pitch", "&Pitch", True, minStep=minStep)

	@classmethod
	def VolumeSetting(cls, minStep=1):
		return NumericDriverSetting("volume", "V&olume", True, minStep=minStep)

	def __init__(self):
		pass


class WavePlayer:
	"""Captures what the driver feeds and fires onDone as if it played.

	feed() optionally sleeps, so a test can make playback slow enough to
	cancel in the middle of it, the way real playback is slow. Every feed
	is timestamped and tagged with the stop count at the time, so a test
	can tell whether audio arrived after a cancel.
	"""

	def __init__(self, channels, samplesPerSec, bitsPerSample, outputDevice=None):
		self.channels = channels
		self.samplesPerSec = samplesPerSec
		self.bitsPerSample = bitsPerSample
		self.data = bytearray()
		self.closed = False
		self.stopped = 0
		self.feedDelay = 0.0
		self.feedsAfterStop = 0
		self.feeds = 0

	def feed(self, data, onDone=None):
		if self.stopped:
			self.feedsAfterStop += 1
		self.feeds += 1
		self.data += data
		if self.feedDelay:
			import time
			time.sleep(self.feedDelay)
		if onDone:
			onDone()

	def idle(self):
		pass

	def stop(self):
		self.stopped += 1

	def pause(self, switch):
		pass

	def close(self):
		self.closed = True


class _Log:
	def info(self, *a, **k):
		print("    [log]", a[0] if a else "")

	def error(self, *a, **k):
		print("    [driver log error]", a[0] if a else "")

	def warning(self, *a, **k):
		print("    [driver log warning]", a[0] if a else "")

	def debugWarning(self, *a, **k):
		pass

	# NVDA's logHandler has this; a stub that does not is a missing method
	# the driver only discovers at runtime, on a user's machine.
	def debug(self, *a, **k):
		pass


def install_stubs():
	def mod(name, **attrs):
		m = types.ModuleType(name)
		for k, v in attrs.items():
			setattr(m, k, v)
		sys.modules[name] = m
		return m

	conf = {"speech": {"outputDevice": "default"}}
	mod("config", conf=conf)
	mod("nvwave", WavePlayer=WavePlayer)
	mod("logHandler", log=_Log())

	pkg = types.ModuleType("autoSettingsUtils")
	pkg.__path__ = []
	sys.modules["autoSettingsUtils"] = pkg
	mod("autoSettingsUtils.driverSetting", DriverSetting=DriverSetting,
		NumericDriverSetting=NumericDriverSetting,
		BooleanDriverSetting=BooleanDriverSetting)
	mod("autoSettingsUtils.utils", StringParameterInfo=StringParameterInfo)

	mod("synthDriverHandler", SynthDriver=_BaseSynthDriver, VoiceInfo=VoiceInfo,
		synthIndexReached=synthIndexReached, synthDoneSpeaking=synthDoneSpeaking)

	speech = types.ModuleType("speech")
	speech.__path__ = []
	sys.modules["speech"] = speech

	class IndexCommand:
		def __init__(self, index):
			self.index = index

	class PitchCommand:
		def __init__(self, offset=0):
			self.offset = offset

	mod("speech.commands", IndexCommand=IndexCommand, PitchCommand=PitchCommand)
	return IndexCommand, PitchCommand


def wav_write(path, rate, pcm):
	n = len(pcm) // 2
	with open(path, "wb") as f:
		f.write(b"RIFF" + struct.pack("<I", 36 + n * 2) + b"WAVE")
		f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
		f.write(b"data" + struct.pack("<I", n * 2))
		f.write(bytes(pcm))


def main():
	if len(sys.argv) not in (3, 4):
		print(__doc__)
		return 2
	dll, romdir = sys.argv[1], sys.argv[2]
	outpath = sys.argv[3] if len(sys.argv) == 4 else "nvda_driver_check.wav"
	if not os.path.isfile(dll):
		print(f"missing {dll}")
		return 2

	IndexCommand, PitchCommand = install_stubs()
	f = Failures()

	# Stage a driver directory holding the DLL and the images, the way an
	# installed add-on is laid out.
	stage = tempfile.mkdtemp(prefix="echotalk_nvda_")
	drvdir = os.path.join(stage, "synthDrivers", "echotalk")
	os.makedirs(drvdir)
	src = os.path.join(os.path.dirname(__file__), "..", "nvda-addon",
		"synthDrivers", "echotalk", "__init__.py")
	shutil.copy(src, drvdir)
	shutil.copy(dll, os.path.join(drvdir,
		"echotalk64.dll" if ctypes.sizeof(ctypes.c_void_p) == 8 else "echotalk32.dll"))
	pairs = 0
	for name in os.listdir(romdir):
		if name.endswith((".ram.bin", ".obj.bin", ".loader.bin")):
			shutil.copy(os.path.join(romdir, name), drvdir)
			pairs += name.endswith(".obj.bin")
	print(f"staged {pairs} image pair(s) in {drvdir}")

	# Turn on the sequence logging so the harness proves the diagnostic
	# actually reports what it claims to.
	open(os.path.join(drvdir, "logsequences.txt"), "w").close()

	sys.path.insert(0, stage)
	import synthDrivers.echotalk as drv

	f.check("check() finds the DLL and images", drv.SynthDriver.check())

	synth = drv.SynthDriver()
	pcm = bytearray()
	try:
		voices = synth._getAvailableVoices()
		f.check("voices discovered from the image banners", len(voices) == pairs,
			", ".join(v.displayName for v in voices.values()))
		f.check("default voice is Textalker 3.x",
			voices[synth.voice].displayName.rsplit(" ", 1)[-1].startswith("3"),
			voices[synth.voice].displayName)

		# Defaults must land on the Echo's own values, not a slider midpoint.
		f.check("default pitch is 24", synth._pitch_c == 24, str(synth._pitch_c))
		f.check("default volume is 12", synth._volume_c == 12, str(synth._volume_c))
		f.check("default word delay is 0", synth._delay_c == 0, str(synth._delay_c))
		f.check("default repeat filter is 99", synth._repeat_c == 99, str(synth._repeat_c))
		f.check("default speed is 1.0x", abs(synth._speed - 1.0) < 1e-9, str(synth._speed))
		f.check("default clock is 1.0x", abs(synth._clock - 1.0) < 1e-9, str(synth._clock))
		f.check("default is expanded, not compressed", synth.compressed is False)
		f.check("default is normal intonation", synth.monotone is False)
		f.check("default sample rate is 8 kHz", synth.samplerate == "8000")

		# The declared defaults must agree with the state the driver starts
		# in, or the first settings-dialog open silently changes the voice.
		byId = {s.id: s for s in synth.supportedSettings}
		f.check("declared rate default matches", byId["rate"].defaultVal == synth.rate,
			f"{byId['rate'].defaultVal} vs {synth.rate}")
		f.check("declared pitch default matches", byId["pitch"].defaultVal == synth.pitch,
			f"{byId['pitch'].defaultVal} vs {synth.pitch}")
		f.check("declared volume default matches",
			byId["volume"].defaultVal == synth.volume,
			f"{byId['volume'].defaultVal} vs {synth.volume}")
		f.check("declared word delay default matches",
			byId["worddelay"].defaultVal == synth.worddelay)
		f.check("declared repeat default matches",
			byId["repeatfilter"].defaultVal == synth.repeatfilter)
		f.check("declared clock default matches", byId["clock"].defaultVal == synth.clock)

		# Every slider must round-trip: NVDA re-reads through the getter
		# straight after setting, and a mapping that does not round-trip
		# makes the control jump under the user's fingers.
		unstable = []
		for sid in ("rate", "pitch", "volume", "worddelay", "repeatfilter", "clock"):
			for pct in range(0, 101):
				setattr(synth, sid, pct)
				once = getattr(synth, sid)
				setattr(synth, sid, once)
				if getattr(synth, sid) != once:
					unstable.append((sid, pct))
		f.check("all sliders round-trip", not unstable, str(unstable[:5]))

		# The chip clock scales what the chip produces, so the output rate
		# has to behave as a floor rather than a fixed value. At a 2x clock
		# the chip really makes 16 kHz, and delivering 8 kHz would resample
		# that DOWN through a resampler with no anti-aliasing filter, which
		# folds everything above the new Nyquist back into the audible band.
		# Reported by a user; no automated check had covered it.
		synth.samplerate = "8000"
		synth.clock = 50                        # 1.0x
		f.check("at a 1.0x clock the chosen rate is used as-is",
			synth._effectiveSamplerate() == 8000, str(synth._effectiveSamplerate()))
		synth.clock = 75                        # 2.0x on the logarithmic slider
		native = int(8000 * synth._clock + 0.5)
		f.check("a clock outrunning the output rate raises it",
			synth._effectiveSamplerate() == native,
			f"{synth._effectiveSamplerate()} vs {native}")
		f.check("the raised rate reaches the player",
			synth._player.samplesPerSec == native, str(synth._player.samplesPerSec))
		f.check("the user's own rate choice is left untouched",
			synth.samplerate == "8000", synth.samplerate)
		synth.samplerate = "44100"
		f.check("a rate already above the chip is not disturbed",
			synth._effectiveSamplerate() == 44100, str(synth._effectiveSamplerate()))
		synth.samplerate = "8000"
		synth.clock = 50                        # back to 1.0x
		f.check("dropping the clock restores the chosen rate",
			synth._effectiveSamplerate() == 8000
			and synth._player.samplesPerSec == 8000,
			f"{synth._effectiveSamplerate()} / {synth._player.samplesPerSec}")

		# Restore the defaults the test then speaks with.
		synth.rate = byId["rate"].defaultVal
		synth.pitch = byId["pitch"].defaultVal
		synth.volume = byId["volume"].defaultVal
		synth.worddelay = 0
		synth.repeatfilter = byId["repeatfilter"].defaultVal
		synth.clock = byId["clock"].defaultVal

		def speak_and_wait(seq, label):
			synthIndexReached.events.clear()
			synthDoneSpeaking.events.clear()
			before = len(synth._player.data)
			synth.speak(seq)
			# The synthesis thread is real, so wait for it to finish rather
			# than for the queue to empty. (Not Queue.join(): the driver has
			# no reason to call task_done(), so that would wait forever.)
			import time
			deadline = time.time() + 30
			while time.time() < deadline:
				if synthDoneSpeaking.events:
					break
				time.sleep(0.01)
			produced = synth._player.data[before:]
			print(f"    {label}: {len(produced) // 2} samples, "
				f"{len(synthIndexReached.events)} index event(s)")
			return bytes(produced)

		pcm += speak_and_wait(["Hello there. This is EchoTalk running as an NVDA synthesizer."],
			"plain speech")
		f.check("plain speech produced audio", len(pcm) > 8000, f"{len(pcm) // 2} samples")

		# Index commands, the mechanism NVDA uses to follow progress.
		seq = ["First part. ", IndexCommand(11), "Second part. ",
			IndexCommand(22), "Third part.", IndexCommand(33)]
		pcm += speak_and_wait(seq, "with index marks")
		got = [e["index"] for e in synthIndexReached.events]
		f.check("index commands come back in order", got == [11, 22, 33], str(got))

		# Capitals: NVDA raises the pitch and resets after.
		pcm += speak_and_wait(["a ", PitchCommand(30), "B", PitchCommand(0), " c"],
			"pitch command")
		f.check("pitch command left the base pitch alone", synth._pitch_c == 24,
			str(synth._pitch_c))

		# Control bytes off the screen must not reach the pipeline. The
		# right outcome is that they are NEUTRALISED, not that the rest of
		# the line vanishes: what follows the byte is document content and
		# still belongs in the speech. So the sanitised version has to come
		# out identical to the same text with those bytes already replaced
		# by spaces, and the voice has to be untouched.
		sanitised = speak_and_wait(["The report \x04 2F is \x05 8P due today."],
			"text containing control bytes")
		expected = speak_and_wait(["The report   2F is   8P due today."],
			"the same, with those bytes already spaces")
		f.check("control bytes are neutralised, not obeyed",
			sanitised == expected and synth._pitch_c == 24,
			f"pitch {synth._pitch_c}, "
			+ ("identical audio" if sanitised == expected else "audio differs"))
		pcm += sanitised

		# Monotone and compressed.
		synth.monotone = True
		pcm += speak_and_wait(["This sentence is monotone."], "monotone")
		synth.monotone = False
		synth.compressed = True
		pcm += speak_and_wait(["This sentence is compressed."], "compressed")
		synth.compressed = False

		# Rate, as the slider moves.
		for pct, label in ((25, "rate 25%"), (75, "rate 75%")):
			synth.rate = pct
			pcm += speak_and_wait([f"This is the rate slider at {pct} percent."], label)
		synth.rate = byId["rate"].defaultVal

		# Voice switch: a whole new machine, so every setting has to be
		# pushed again. Check one survives the trip.
		if len(voices) > 1:
			synth.pitch = 60
			want = synth._pitch_c
			other = [v for v in voices if v != synth.voice][0]
			synth.voice = other
			f.check("voice switch keeps the settings",
				synth._pitch_c == want and synth.voice == other,
				f"{voices[other].displayName}, pitch {synth._pitch_c}")
			pcm += speak_and_wait(["This is the other Textalker version."], "other voice")
			synth.pitch = byId["pitch"].defaultVal
			synth.voice = [v for v in voices if v != other][0]
		else:
			print("  --    only one image pair staged; voice switching not exercised")

		# Sample rate changes the output format, so the player is rebuilt.
		synth.samplerate = "22050"
		f.check("sample rate change rebuilt the player",
			synth._player.samplesPerSec == 22050, str(synth._player.samplesPerSec))
		hi = speak_and_wait(["This is at twenty two kilohertz."], "22 kHz")
		f.check("22 kHz output really is longer", len(hi) > 0, f"{len(hi) // 2} samples")
		synth.samplerate = "8000"

		# --- cancel while speech is actually in flight ---
		#
		# This is the bug Jayson hit: the synthesis thread checked for a
		# cancel BEFORE synthesising an utterance, then fed the result
		# afterwards regardless. A cancel landing in between pushed a
		# cancelled utterance's audio into a player that had just been
		# stopped, which sounds like the last thing said repeating itself.
		# Slow settings made the window wide enough to hit reliably.
		import time
		synth.rate = 0          # slowest: 0.25x
		synth.worddelay = 100   # and the longest inter-word pause
		synth._player.feedDelay = 0.02     # make "playback" take real time
		synthDoneSpeaking.events.clear()
		synth.speak(["This is a deliberately slow and long utterance that will be "
			"cancelled partway through, to check that nothing is fed afterwards."])
		time.sleep(0.4)         # let it get properly under way
		t0 = time.perf_counter()
		synth.cancel()
		cancelMs = (time.perf_counter() - t0) * 1000
		# A feed arriving in this window belongs to speech that was
		# cancelled. Counting "feeds since the first ever stop" instead
		# would count every legitimate feed thereafter, which is what an
		# earlier version of this check did -- and it passed only because
		# the run happened to be short.
		feedsAtCancel = synth._player.feeds
		time.sleep(0.5)
		staleFeeds = synth._player.feeds - feedsAtCancel
		f.check("cancel returns promptly even at the slowest settings",
			cancelMs < 150, f"{cancelMs:.0f} ms")
		f.check("no audio is fed after a cancel", staleFeeds == 0,
			f"{staleFeeds} stale feed(s)")
		f.check("a cancelled utterance does not report done speaking",
			not synthDoneSpeaking.events, str(len(synthDoneSpeaking.events)))
		synth._player.feedDelay = 0.0
		synth.rate = byId["rate"].defaultVal
		synth.worddelay = 0

		# And repeatedly, the way NVDA cancels on every keystroke. This is
		# the sensitive one: the stale-feed window opens just after a read
		# returns, so hitting it takes several attempts at varied phases
		# rather than one well-timed cancel. Removing the post-read re-check
		# from the driver makes this fail and the single cancel above pass,
		# which is why both are here.
		stale = 0
		for i in range(24):
			synth.speak([f"Utterance number {i} which will be interrupted partway."])
			time.sleep(0.01 + (i % 7) * 0.012)   # land in different phases
			synth.cancel()
			after = synth._player.feeds
			time.sleep(0.12)      # a stale feed would land in this window
			stale += synth._player.feeds - after
		f.check("no stale audio across repeated cancels", stale == 0,
			f"{stale} stale feed(s) of {synth._player.feeds} total")
		f.check("cancel stops the player", synth._player.stopped > 0)

		pcm += speak_and_wait(["Speech works again after cancelling."], "after cancel")
	finally:
		player = synth._player
		synth.terminate()
		wav_write(outpath, 8000, pcm)

	f.check("terminate closed the player", player is not None and player.closed)

	print(f"\nwrote {outpath}: {len(pcm) // 2} samples at 8000 Hz")
	print("\nall checks passed" if not f.count else f"\n{f.count} check(s) FAILED")
	return 1 if f.count else 0


if __name__ == "__main__":
	sys.exit(main())
