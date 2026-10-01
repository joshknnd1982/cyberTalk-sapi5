# CyberTalk SAPI5

Panasonic **CyberTalk** (Speech Technology Laboratory, 1997) as SAPI 5 voices
for 32-bit and 64-bit Windows applications: screen readers (NVDA, JAWS,
Narrator), Balabolka, Bookworm, anything that speaks through SAPI 5.

CyberTalk is abandonware: a formant/diphone text-to-speech engine that
Panasonic's Speech Technology Laboratory in Santa Barbara sold in the late
1990s as a SAPI 4 engine and as a Macromedia Director Xtra.  It has not been
available for decades.  This repository keeps the complete original engine
(the files in `bin`), the reverse-engineered protocol documentation, a modern
SAPI 5 wrapper, a configuration utility and an installer, so that a `git clone`
gives you everything.

The wrapper talks to the engine process directly through its shared-memory
protocol.  **No SAPI 4 component is installed or used.**  The engine's SAPI 4
glue (TTSAPI.DLL) is shipped only because STLTTS.EXE imports it; none of its
code is called by the wrapper, and the original SAPI 4 registration is not
performed.

## Contents

| Folder | What it holds |
|---|---|
| `bin` | the original CyberTalk engine files (see [Engine files](#engine-files)) |
| `src` | the SAPI 5 engine DLL, the host, the pipe protocol, the direct engine client and the configuration utility |
| `installer` | the Inno Setup script |
| `prebuilt` | ready-built binaries from the release, for those who do not want to compile |
| `samples` | WAV renders proving every parameter, voice and tag ([samples/README.txt](samples/README.txt)) |
| `docs` | the documentation, see below |
| `test` | engine probe, SAPI end-to-end test, registration test, accessibility dump |
| `tools` | the TTSAPI.DLL patch script |

## Documentation

CyberTalk had no public documentation.  These files are the first:

- [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md): the engine
  itself - files and exports, process model, every named kernel object, the
  control block field by field, all 41 commands, the three call channels,
  speaking and cancelling, parameter limits, modes, tags, pacing, the two
  binary patches, known bugs, code addresses.
- [docs/WRAPPER.md](docs/WRAPPER.md): the SAPI 5 wrapper - registration,
  the pipe protocol, the host, the SAPI parameter and event mapping, the
  settings file, logging, the accessibility rules of the utility.
- [docs/TESTING.md](docs/TESTING.md): what was tested and the results.
- [samples/README.txt](samples/README.txt): the WAV renders and measurements.
- This file: user-level parameter reference.

## Installing

Download `CyberTalkSAPI_Setup.exe` from the
[Releases](../../releases) page and run it (administrator rights are required
to register SAPI voices).  It installs the host, both engine DLLs, the engine
files, the configuration utility and the test tools, registers the three
voices for 32-bit and 64-bit applications, writes
`HKLM\SOFTWARE\CyberTalkSAPI\InstallDir`, and offers a desktop icon for the
configuration utility.  Uninstalling stops the host and unregisters everything.

## Voices

| Voice | What it is |
|---|---|
| CyberTalk Male | the engine's male sample set with the engine defaults (90 Hz, 200 words/min, brightness 8) |
| CyberTalk Female | the engine's female sample set with the engine defaults (195 Hz, 200 words/min, brightness 6) |
| CyberTalk Custom Voice | every engine parameter under your control through the configuration utility |

The engine speaks US English only.  Audio is 11025 Hz, 16-bit mono.  The
engine has exactly one voice per gender; there are no other languages, dialects
or speakers hidden in it (the sample DLLs are the only voice data, and the
recorded-number set is not a voice but an alternative way of reading digits).

## Parameter reference

Everything below was discovered by reverse-engineering STLTTS.EXE and
TTSAPI.DLL and then proven by rendering WAV files (`samples/probe`, with
measurements in `samples/probe/analysis.txt`).  The engine validates every
value: out-of-range values are rejected with 0x8000FFFF and the previous value
stays in force.

### Gender

| | |
|---|---|
| Values | male, female |
| Engine default | male |
| Engine command | `MAL` / `FEM` (query `GEND` returns 2 for male, 1 for female) |
| What it does | selects the sample set (MSAMPLES.DLL or FSAMPLES.DLL).  Switching gender resets pitch, speed and brightness to that gender's defaults, so the wrapper re-applies its values after every switch. |
| Configuration utility | "Voice gender" combo box |
| SAPI | fixed per voice; the Custom Voice takes it from the settings file |

### Pitch

| | |
|---|---|
| Range | male 70 - 150 Hz, female 145 - 300 Hz |
| Engine default | male 90 Hz, female 195 Hz |
| Engine command | `PIT` (query `GPIT`) |
| Inline tag | `\Pit=Hz\` |
| What it does | baseline fundamental frequency of the voice.  Intonation still moves around it. |
| Configuration utility | slider and spin edit, 0 % = 70 Hz (male) / 145 Hz (female), 100 % = 150 Hz / 300 Hz; the label shows the resulting Hz |
| SAPI | `<pitch absmiddle="-10..10">` and `<pitch middle="...">` scale the voice's pitch by 2^(n/24) (one octave over the full range), clamped to the gender's range |

### Speed

| | |
|---|---|
| Range | 100 - 300 words per minute |
| Engine default | 200 |
| Engine command | `SPD` (query `GSPD`) |
| Inline tag | `\Spd=wpm\` |
| What it does | speaking rate.  The engine synthesises at any speed in the range; speeds outside it are rejected. |
| Configuration utility | slider and spin edit, 0 % = 100 wpm, 100 % = 300 wpm |
| SAPI | rate -10..+10 is mapped exponentially (rate +10 = 9x, -10 = 1/9 of the base speed) and clamped to 100..300 wpm.  When "rate boost" is on, anything above 300 wpm is produced by time-compressing the 300 wpm audio with libsonic, so rate +10 really is faster than the engine's ceiling.  `<rate absspeed="...">` and `<rate speed="...">` are honoured per fragment. |

### Volume

| | |
|---|---|
| Range | 0 - 255 |
| Engine default | 255 |
| Engine command | `VOL` (query `GVOL`) |
| Inline tag | `\Vol=0..65535\` (the tag uses a 16-bit scale; the wrapper multiplies the 0-255 value by 257) |
| Configuration utility | slider and spin edit, 0 % = 0, 100 % = 255 |
| SAPI | `ISpVoice::SetVolume` (0-100) and `<volume level="...">` multiply the voice's base volume |

### Brightness

| | |
|---|---|
| Range | 0 - 15 |
| Engine default | male 8, female 6 |
| Engine command | `BRGHT` (query `GBRGHT`) |
| What it does | timbre: higher values emphasise the high frequencies (measurable as a rising zero-crossing rate), lower values sound muffled |
| Configuration utility | slider and spin edit, 0 % = 0, 100 % = 15 |
| SAPI | no SAPI equivalent; fixed per voice, the Custom Voice takes it from the settings file |

### Reading modes

Set with `MDE(index, value)` / queried with `GMDE(index)`; an unknown index is
rejected with 0x80070057.  The wrapper applies them before every utterance.

| Index | Mode | Default | What it does |
|---|---|---|---|
| 0 | reset | | restores all modes to their defaults |
| 1 | spreadsheet / table | off | reads cell-by-cell, with a pause between fields |
| 2 | list | off | pauses after each line |
| 3 | dollar | **on** | "$19.99" is read as "nineteen dollars and ninety-nine cents"; off reads the symbol and the number |
| 4 | zero | off | say "zero" instead of "oh" for the digit 0.  Note: in our tests this produced no audible difference on the test sentences. |
| 5 | number samples | off | reads numbers from the recorded-number set NSAMPLES.DLL (`SNMB` / `GNMB` set and query the same thing) |
| 6 | Japanese digit grouping | off | groups digits by four.  **Crashes the engine on ordinary text; the wrapper never enables it.** |

Modes can also be switched inside the text with
`\Eng:SET:NUMBER|DOLLAR|ZERO|JAPANESE|DICTIONARY|DEFAULT\` and
`\Eng:RST:...\`.

### Inline tags

The engine parses backslash tags in the text.  Those the wrapper generates are
marked with *.

| Tag | Meaning |
|---|---|
| `\Mrk=N\` * | bookmark: the engine reports the audio position when it is reached (used for SAPI bookmarks and sentence events).  Two adjacent marks produce a single report, so the wrapper folds them. |
| `\Pau=ms\` * | silence, up to 30000 ms |
| `\Spd=wpm\` * | speed from here on |
| `\Pit=Hz\` * | pitch from here on |
| `\Vol=0..65535\` * | volume from here on |
| `\Vce=Gender=Male|Female|Neutral\` | switch the voice |
| `\Emp\`, `\Emp=n\` | emphasis on the next word |
| `\Eng:PHO=phonemes\` | speak a phoneme string |
| `\Eng:PRN=word=pronunciation,PRT=part\` | add a pronunciation |
| `\Eng:SET:...\`, `\Eng:RST:...\` | reading modes, see above |
| `\Chr`, `\Com`, `\Ctx=...`, `\Prt=...`, `\Rst` | character, comment, context, part of speech, reset |
| `\Pro` | accepted and ignored; unknown tags are ignored |

Because a backslash starts a tag, a backslash in ordinary text cannot be
spoken; the wrapper turns it into a space.

### Text handling

The engine accepts 8-bit text.  The wrapper transliterates Unicode: accented
letters lose their accents, smart quotes, dashes and ellipses become their
ASCII forms, symbols such as ° and © become words.  Word positions reported by
the engine are byte offsets into this text; the wrapper maps them back to the
original characters for SAPI word events.

### Audio

11025 Hz, 16-bit, mono, delivered in 11000-byte (half-second) chunks.  The
engine reports the format through `WaveFormatSet` when it starts.

## SAPI 5 features

- Rate, pitch and volume as described above.
- `<bookmark>`, `<silence>`, `<spell>`, `<pitch>`, `<rate>`, `<volume>` XML.
- Word, sentence and bookmark events with sample-exact audio positions.
- Fast cancel: a purge is honoured within one 20 ms audio slice.
- The engine hangs occasionally by design flaw (a lost event pulse); the host
  recovers a lost pulse within 2 ms and, should the engine ever stop
  responding, restarts it within a few seconds.  Engine crashes are also
  restarted automatically.

## Configuration utility

*Start menu > CyberTalk SAPI5 > CyberTalk Configuration* (or the optional
desktop icon) adjusts the **CyberTalk Custom Voice**:

- Voice gender.
- Pitch, speed, volume and brightness: a slider paired with a spin edit, both
  0 - 100 %; **0 % is the engine's minimum and 100 % its maximum**, and the
  edit's label shows the real engine value ("Pitch % (90 Hz)").
- The five reading modes.
- General: rate boost, word boundary events, detailed logging, "Open log folder".
- Test text with Preview and Stop; Save, Reset to defaults, Close.

Every control has a text label and is in the tab order; verified with the
MSAA dump tool in `test/msaa_dump.cpp`.  Settings live in
`%APPDATA%\CyberTalkSAPI\settings.ini` and are picked up on the next
utterance, no restart of the screen reader needed.

```ini
[CustomVoice]
Gender=2          ; 2 = male, 1 = female
Pitch=90          ; Hz, within the gender's range
Speed=200         ; words per minute, 100..300
Volume=255        ; 0..255
Brightness=8      ; 0..15
DollarMode=1
ZeroMode=0
NumberSamples=0
SpreadsheetMode=0
ListMode=0
[General]
RateBoost=1       ; time compression above 300 wpm
Logging=1
WordEvents=1
```

## Log files

`%LOCALAPPDATA%\CyberTalkSAPI\logs\`:

| File | Written by |
|---|---|
| `sapi_x86.log`, `sapi_x64.log` | the SAPI engine DLL: every Speak call, parameters, event counts, cancels, errors |
| `host.log` | CyberTalkHost.exe: engine start, commands, cancels, lost pulses, restarts |
| `config.log` | the configuration utility |
| `install.log` | copied there by the installer (full Inno Setup log); a second copy is written to `<install folder>\logs\install.log` |

Logging is on by default and can be switched off in the configuration
utility.  Files roll over to `.old` at 4 MB.

## How it works

```
SAPI application (x86 or x64)
  -> CyberTalkSAPI.dll            (ISpTTSEngine, x86 and x64 builds)
     -> \\.\pipe\CyberTalkTTS     (byte-stream named pipe, src/pipe_protocol.h)
        -> CyberTalkHost.exe      (32-bit, owns the engine, libsonic rate boost)
           -> STLTTS.EXE          (the engine: shared memory + registered window messages)
```

The host starts on demand, keeps the engine warm, restarts it if it dies or
hangs, and exits after ten idle minutes.  Three things make the 1997 engine
usable today:

- STLTTS.EXE paces itself with `timeGetTime` to simulate real-time playback
  (busy-looping at 100 % CPU).  The host redirects its `timeGetTime` import to
  a stub that scales the clock 200x, so the engine renders about 175x faster
  than real time with exact byte positions for marks.
- The engine signals its audio-object calls with SetEvent immediately followed
  by ResetEvent, which loses the wakeup unless the client is already waiting.
  The host's dispatcher polls the command word as a fallback.
- TTSAPI.DLL shows message boxes at start-up when its SAPI 4 registry key is
  missing.  Two 14-byte `AfxMessageBox` calls are NOP-ed in the shipped copy
  (`tools/patch_ttsapi.py`; the untouched original is `bin/original/TTSAPI.DLL`).

## Engine files

| File | Date | Role |
|---|---|---|
| `bin/STLTTS.EXE` | 1997-07-09 | the synthesiser (version 1.0.225) |
| `bin/TTSAPI.DLL` | 1997-07-09 | SAPI 4 glue, patched as described above |
| `bin/original/TTSAPI.DLL` | 1997-07-09 | the unpatched original |
| `bin/MSAMPLES.DLL`, `bin/FSAMPLES.DLL` | 1997-07-09 | male and female sample sets |
| `bin/NSAMPLES.DLL` | 1997-07-09 | recorded numbers |
| `bin/SPEECH.X32`, `bin/SPEECHNE.X32`, `bin/SPEECHOL.X32` | 1997 | the Macromedia Director Xtras CyberTalk shipped with; kept for completeness, not used |
| `bin/TTSAPI.REG` | 1997 | the original SAPI 4 registration; not used |

## Building from source

Requirements: Visual Studio 2022 Build Tools (C++ for x86 and x64), CMake
3.20+, Inno Setup 6 (optional, for the installer).

```bat
build_all.bat
```

builds `build_x86` and `build_x64`, stages `output\` and compiles
`output\CyberTalkSAPI_Setup.exe`.

Test tools (also installed under `tools\` and available in `prebuilt\tools`):

- `cybertalk_probe.exe <dir> [quick|acktest|modes]` drives the engine directly
  and writes WAV files for every parameter, mode and tag.
- `sapi_test_x86.exe` / `sapi_test_x64.exe <dir> [--audio] [token id...]`
  renders the installed CyberTalk voices through SAPI and prints the events;
  `--audio` speaks through the sound card and measures cancel latency.
- `reg_test_x86.exe` / `reg_test_x64.exe <dll>` runs `DllRegisterServer`
  against a redirected `HKEY_LOCAL_MACHINE` so registration can be checked
  without admin rights.
- `msaa_dump.exe [--press id] [--shot file.png]` prints what a screen reader
  sees in the configuration dialog.

## Known limitations

- US English only; 11 kHz audio.
- The Japanese digit-grouping mode is disabled (engine crash).
- Zero mode produced no audible difference on the test sentences.
- The engine's "get" commands lag one command behind when parameters are
  changed in rapid succession; the wrapper caches what it set instead.

## Credits and license

See [CREDITS.md](CREDITS.md) and [LICENSE](LICENSE).  The wrapper is MIT
licensed, except the SAPI 5 COM files that were adapted from Gozaltech's
BestSpeech wrapper; the CyberTalk engine binaries remain the property of their
copyright holders and are preserved here because the software is no longer
obtainable anywhere else.  [NOTICE.md](NOTICE.md) lists what the MIT license
does not cover.
