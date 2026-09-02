# Test record

What was tested, how, and what came out.  All numbers are from the machine the
wrapper was developed on (Windows 11, 2026-09-02).

## Engine probe (`test/cybertalk_probe.cpp`)

Drives the engine directly through `stl_engine` and writes one WAV per
experiment into `samples/probe` (67 files) with `probe_report.txt`.
`samples/probe/analysis.txt` holds the measurements:

- pitch 70, 90, 110, 130, 150 Hz (male) and the female range: the median
  fundamental of each file follows the setting;
- speed 100, 150, 200, 250, 300: duration of the same sentence falls
  inversely with speed;
- volume 0..255: RMS level follows the setting;
- brightness 0..15: zero-crossing rate rises with the setting;
- every mode on and off, every inline tag, numbers, currency, punctuation,
  Unicode transliteration, backslashes.

The probe also found the engine's failure modes: the Japanese-mode crash, the
query lag, and (with the clock patch) the lost-pulse hang.

## Registration (`test/reg_test.cpp`)

`DllRegisterServer` and `DllUnregisterServer` of both DLLs were run with
`HKEY_LOCAL_MACHINE` and `HKEY_CLASSES_ROOT` redirected into
`HKCU\Software\CTRegTest`.  Both returned S_OK; the keys written were the two
CLSIDs with `InProcServer32` and `ThreadingModel`, and
`Speech\Voices\TokenEnums\CyberTalk`; unregistration removed them all.

## SAPI end to end (`test/sapi_test.cpp`)

The DLLs were registered per user (HKCU class entries in both registry views
plus three plain voice tokens) and driven through `SpVoice` with file output:

| Case | Result |
|---|---|
| CyberTalk Male, default text with bookmarks and a `<pitch>` change | 11.01 s, median f0 97.6 Hz |
| CyberTalk Female | 11.01 s, 208 Hz |
| CyberTalk Custom Voice with settings female / 250 Hz / 150 wpm | 12.16 s, 245 Hz |
| rate +5 | 5.90 s (300 wpm, compression 1.15) |
| rate +10 | 3.40 s (300 wpm, compression 2.0) |
| rate -5 | 18.08 s (115 wpm) |
| volume 50 | same length, half amplitude |
| Unicode text | transliterated, 6.01 s |
| XML: spell, silence, volume, rate, pitch | 7.16 s |
| 32-bit vs 64-bit DLL | all ten renders byte-identical (MD5) |

Events with a live audio output (the screen-reader path):

```
[audio-short] Speak(async) returned after 0 ms
[audio-short] +   47 ms start
[audio-short] + 1766 ms bookmark audio=4620   "a"
[audio-short] + 1766 ms sentence audio=4620
[audio-short] + 1766 ms bookmark audio=35420  "b"
[audio-short] + 2250 ms end      audio=48620
[audio-cancel] + 1547 ms PURGE -> S_OK, call took 47 ms   (20 ms slices: within one slice)
[audio-cancel] + 1578 ms end / start of the replacement utterance
```

Before the slice change the purge took 500 ms (one whole engine chunk).

## Stress and hang recovery

Three consecutive 32-bit runs and one 64-bit run of the full test (40
utterances, including a 340-second text) after the dispatcher rewrite:

- 40 of 40 rendered, no timeouts;
- 5 lost pulses recovered by the poll fallback (each of them would have hung
  the engine before);
- the 11-second default text renders in 47..63 ms (about 175x real time).

Before the rewrite the same test hung the engine on the third 64-bit render
and every later command timed out.

## Configuration utility

- `msaa_dump.exe` lists 24 focusable controls, all with accessible names, in
  a sensible tab order (gender, pitch slider, pitch edit, ... , Save, Reset,
  Close).
- Preview was pressed through `BM_CLICK`: the host log shows the utterance;
  Save wrote the expected `settings.ini`; a subsequent SAPI render of the
  Custom Voice used the new values.

## Installer

Compiled with Inno Setup 6 (`x64compatible` install mode), installed and used
by the author on a real system: voices appear in 32-bit and 64-bit SAPI
applications, the configuration utility works, logs are written.
