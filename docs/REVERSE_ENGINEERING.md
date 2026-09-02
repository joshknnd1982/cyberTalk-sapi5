# CyberTalk engine: reverse-engineering reference

Panasonic CyberTalk was never publicly documented.  Everything in this file
was recovered from the raw disassembly of `TTSAPI.DLL` and `STLTTS.EXE`
(capstone + pefile, no source, no symbols beyond the DLL's export names) and
then confirmed by driving the engine and measuring the audio it produced.
Where something was observed but not fully understood it is marked as such.

Contents

1. [Files](#1-files)
2. [Process model](#2-process-model)
3. [Kernel objects](#3-kernel-objects)
4. [The control block](#4-the-control-block-stlttsmem)
5. [Start-up sequence](#5-start-up-sequence)
6. [Commands (client to engine)](#6-commands-client-to-engine)
7. [Calls from the engine to the client](#7-calls-from-the-engine-to-the-client)
8. [Speaking text](#8-speaking-text)
9. [Cancelling](#9-cancelling)
10. [Parameters and their limits](#10-parameters-and-their-limits)
11. [Reading modes](#11-reading-modes)
12. [Inline tags](#12-inline-tags)
13. [Text encoding](#13-text-encoding)
14. [Pacing and the clock patch](#14-pacing-and-the-clock-patch)
15. [The start-up message boxes and the TTSAPI.DLL patch](#15-the-start-up-message-boxes-and-the-ttsapidll-patch)
16. [Engine bugs and quirks](#16-engine-bugs-and-quirks)
17. [Code landmarks](#17-code-landmarks)
18. [Method](#18-method)

## 1. Files

| File | Size | Built | Role |
|---|---|---|---|
| `STLTTS.EXE` | 824,832 | 1997-07-09 | the synthesiser.  Windows GUI subsystem, message-loop driven, fixed image base 0x400000, no exports.  Imports 40 functions from TTSAPI.DLL, 10 from WINMM (timeGetTime, timeBeginPeriod, waveOut*), 8 from USER32. |
| `TTSAPI.DLL` | 156,672 | 1997-07-09 | MFC 4.2 DLL with two halves: the SAPI 4 `ITTSCentral` implementation (client half, loaded by SAPI 4 applications) and the glue the engine calls (server half, loaded by STLTTS.EXE).  44 exports, listed below. |
| `MSAMPLES.DLL` | 504,832 | 1997-07-09 | male diphone samples; one export, `Get_from_male` |
| `FSAMPLES.DLL` | 336,384 | 1997-07-09 | female diphone samples; one export, `Get_from_female` |
| `NSAMPLES.DLL` | 2,335,744 | 1997-07-09 | recorded numbers ("number samples" mode); one export, `Get_from_number` |
| `SPEECH.X32`, `SPEECHOL.X32` | 32,256 each | 1997 | Macromedia Director Xtras (MOA interface: `DllGetClassInfo`, `DllGetInterface`, ...).  They wrap the same engine for Director; not used here. |
| `SPEECHNE.X32` | 48,611 | 1997 | damaged: the file was transferred in text mode at some point (every 0x0A byte became 0D 0D 0A) and is not a loadable image.  Kept as found. |
| `TTSAPI.REG` | 362 | 1997 | the original SAPI 4 registration: engine CLSID {77798141-2B46-11d0-8C76-00A024405356}, `EngineLocation` = `c:\stlsdk\bin32\` |

Engine version, as returned by `GTVE`: 1.0.225.

### TTSAPI.DLL exports

Server half (called by STLTTS.EXE):

| Export | Purpose |
|---|---|
| `InitGlueServer` | opens the mappings, semaphores and events by name (see below), registers the window messages |
| `GetTtsData` | fetches the next queued text buffer (maps the handle the client duplicated in) |
| `TtsFreeData` | releases a consumed text buffer |
| `TtsBuffersWaiting` | number of queued text buffers |
| `ReturnInitResult`, `ReturnSafeToExit` | acknowledge start-up and shutdown |
| `ReturnError`, `SetResultReturn` | acknowledge a command with a result code |
| `ReturnPitch`, `ReturnSpeed`, `ReturnVolume`, `ReturnBright`, `ReturnGender`, `ReturnNumber`, `ReturnMode`, `ReturnIsLoaded`, `ReturnVersion`, `ReturnFFRWSize`, `ReturnCheckPhoneme`, `ReturnPhoneme` | answer queries by writing into the control block and releasing the sentinel |
| `CallTextdataStarted`, `CallTextdataDone`, `ReturnWordpos`, `ReturnBookMark` | buffer notifications |
| `ReturnStartMessages`, `ReturnStopMessages`, `PitchChanged`, `SpeedChanged`, `VolumeChanged`, `BrightChanged` | audio start/stop and attribute-changed notifications |
| `GetAudioObject` | the engine's proxy for the client's audio destination; every audio call goes through it |
| `ReturnPeakToClient` | audio level meter |
| `TtsGetSpelling`, `TtsGetPhonemes`, `TtsGetPhonemeBuffers`, `TtsGetPos` | spelling / phoneme / lexicon exchanges |
| `APIDoReset` | called after a cancel; releases `BufferSemaphore` |
| `SYSTRACE`, `GetWinExecErrorTxt` | diagnostics |

Client half (used by SAPI 4 applications, not by this wrapper):
`InitGlueClient`, `ClientSentinel`, `DllGetClassObject`, `DllCanUnloadNow`,
`simuDllGetClassObject`.

## 2. Process model

The engine is a separate process.  A client creates the shared objects,
launches `STLTTS.EXE` (with inheritable handles and the engine directory as
current directory), waits for the engine to acknowledge, then drives it with
window messages posted to the engine's main thread.  Everything the engine
sends back goes through the control block plus an event handshake.

The engine is effectively single-threaded from the client's point of view:
in more than a hundred thousand handshakes no two calls were ever in flight at
the same time.  (The glue creates one extra thread, but it never issues
calls.)

## 3. Kernel objects

All are named, created by the client before the engine starts, and opened by
the engine with `OpenFileMappingA` / `OpenSemaphoreA` / `OpenEventA`.

| Name | Kind | Size / initial state | Purpose |
|---|---|---|---|
| `stlttsmem` | file mapping | 0x218 bytes | the control block (section 4) |
| `audioBuf` | file mapping | 0x6590 bytes (26,000) | audio chunks from the engine |
| `spellingBuf` | file mapping | 0x28 | spelling exchange |
| `phonemeBuf` | file mapping | 0x38 | phoneme exchange |
| `SentinelSemaphore` | semaphore | 0, max 1 | released by the engine to acknowledge a command or query |
| `BufferSemaphore` | semaphore | 0 | released by `APIDoReset` once a cancel has been processed |
| `bufNotifyEvent` / `bufNotifyReturn` | events | auto-reset | buffer notifications (text started / done, word position, bookmark) |
| `NotifyEvent` / `notifyReturn` | events | auto-reset | audio start / stop and attribute-changed notifications |
| `audioMeterEvent` | event | auto-reset | audio level meter; unused |
| `audioObjectCall` / `audioObjectReturn` | events | call: manual-reset, return: auto-reset | the engine's calls to the audio destination |

The engine expects the event names exactly as spelt above (note the mixed
case).  The reset mode of the events is the client's choice because the client
creates them; the engine's own code is written for manual-reset events (see
section 16 for the consequence).

## 4. The control block (`stlttsmem`)

Offsets in bytes; all fields little-endian.

| Offset | Size | Name | Written by | Meaning |
|---|---|---|---|---|
| 0x000 | 4 | write_idx | client | next text slot the client fills |
| 0x004 | 4 | read_idx | engine | next slot the engine consumes |
| 0x008 | 4 | pending | client (+1), engine (-1) | queued text buffers |
| 0x00c | 4 | txtdd_flag | client | "text data direct" mode |
| 0x010 | 4 | notify_enabled | client | client wants audio start/stop notifications |
| 0x014 | 50 x 4 | text_handles | client | mapping handles, valid in the engine process, one per slot |
| 0x0dc | 4 | init_result | engine | `ReturnInitResult`: 1 = engine ready |
| 0x0e0 | 4 | safe_to_exit | engine | `ReturnSafeToExit` |
| 0x0e4 | 2 | pitch | engine | `ReturnPitch` |
| 0x0e8 | 4 | speed | engine | `ReturnSpeed` |
| 0x0ec | 4 | volume | engine | `ReturnVolume` |
| 0x0f0 | 2 | bright | engine | `ReturnBright` |
| 0x0f4 | 4 | gender | engine | `ReturnGender`: 2 male, 1 female |
| 0x0f8 | 4 | number | engine | `ReturnNumber`: recorded-number mode |
| 0x0fc | 4 | mode_value | engine | `ReturnMode` |
| 0x100 | 4 | is_loaded | engine | `ReturnIsLoaded` |
| 0x104 | 4 | check_phoneme | engine | `ReturnCheckPhoneme` |
| 0x108 | 4 | ffrw_size | engine | `ReturnFFRWSize` |
| 0x10c | 3 x 4 | version | engine | `ReturnVersion`: major, minor, build |
| 0x118 | 4 | pos | client | `TtsGetPos`: part of speech for lexicon additions |
| 0x11c | 4 | phoneme_handle | | phoneme exchange |
| 0x120 | 4 | phoneme_offset | | |
| 0x124 | 4 | phoneme_result | | |
| 0x128 | 4 | phoneme_size | | |
| 0x12c | 4 | client_busy | client | `ClientSentinel` |
| 0x130 | 4 | command | engine | code of the call in progress (sections 7); never read by the engine |
| 0x138 | 8 | timestamp | engine | cumulative audio byte position for notifications |
| 0x140 | 4 | param | both | flags / mark id / word position / audio chunk size / free space |
| 0x144 | 2 | peak | engine | `ReturnPeakToClient` |
| 0x148 | 4 | eof_flag | client | `FreeSpace` end-of-file answer |
| 0x14c | 16 | iid | engine | the IID asked for in `QueryInterface` |
| 0x15c | 4 | result | both | result of a command (`ReturnError` / `SetResultReturn`) or of an audio call |
| 0x160 | 18 | waveformat | engine | `WAVEFORMATEX` from `WaveFormatSet` |
| 0x174 | 4 | client_count | | |
| 0x178 | 4 x 4 | clients | | |
| 0x190 | 4 | dialog_hwnd | | |

## 5. Start-up sequence

1. Create every object in section 3 (inheritable), zero the control block.
2. `CreateProcess("STLTTS.EXE")` with `bInheritHandles = TRUE`, the engine
   directory as current directory, `CREATE_NO_WINDOW`.
3. The engine's `InitGlueServer` opens the objects and registers the 41 window
   messages by name.  The engine then calls `WaveFormatSet` through the audio
   object (the client learns the format: 11025 Hz, 16-bit, mono) and releases
   `SentinelSemaphore` with `init_result = 1`.  This takes about 50 ms.
4. Optionally patch the clock (section 14) and read the version with `GTVE`.

The engine's main thread id (from `CreateProcess`) is the target of every
`PostThreadMessage`.

## 6. Commands (client to engine)

Each command is a window message registered with `RegisterWindowMessageA`
under the name shown, posted with `PostThreadMessageA(engine_thread, msg,
wParam, lParam)`.  "Ack" means the engine releases `SentinelSemaphore` when
done and, for commands marked "result", writes the outcome into `result`
(S_OK, 0x8000FFFF for an out-of-range value, 0x80070057 for a bad index).
Queries write their answer into the field named and do **not** touch
`result`.

| Message | wParam, lParam | Ack | Meaning |
|---|---|---|---|
| `TXTDTA` | size of the text buffer | ack + result | speak the text in slot `write_idx` (section 8) |
| `TXTDD` | | | "text data direct" mode; not used |
| `AUDRES` | | no (see section 9) | audio reset: stop speaking and discard the queue |
| `AUDPAU`, `AUDRSM` | | ack | pause / resume audio |
| `LEVEL` | | | audio level; not exercised |
| `MAL`, `FEM` | | ack + result | select the male / female sample set.  Resets pitch, speed and brightness to that gender's defaults. |
| `GEND` | | ack -> `gender` | query the gender (2 male, 1 female) |
| `PIT` | Hz | ack + result | pitch, 70..150 male, 145..300 female |
| `GPIT` | | ack -> `pitch` | query |
| `SPD` | words/min | ack + result | speed, 100..300 |
| `GSPD` | | ack -> `speed` | query |
| `VOL` | 0..255 | ack + result | volume |
| `GVOL` | | ack -> `volume` | query |
| `BRGHT` | 0..15 | **no ack** | brightness |
| `GBRGHT` | | ack -> `bright` | query |
| `METR` | | | audio meter on/off; not used |
| `MDE` | index, value | ack + result | set a reading mode (section 11) |
| `GMDE` | index | ack -> `mode_value` | query a mode |
| `SNMB` | 0/1 | **no ack** | recorded-number mode (same thing as mode 5) |
| `GNMB` | | ack -> `number` | query |
| `LMLE`, `LFML`, `LNUM` | | **no ack** | load the male / female / number sample DLL |
| `ULML`, `ULFM`, `ULNU` | | **no ack** | unload them |
| `IMLO`, `IFLO`, `INLO` | | ack -> `is_loaded` | is the set loaded? |
| `FFRW`, `FRWN`, `GFRM`, `SFRM` | | | fast-forward / rewind and frame position for the SAPI 4 `ITTSCentral::AudioFastForward/Rewind` and position calls; not exercised |
| `ADEN`, `DLEN`, `RSDT`, `CKLT` | | | add / delete lexicon entries, reset the dictionary, check a word (`TtsGetSpelling`, `TtsGetPos`, `spellingBuf`); not exercised |
| `PHON` | | ack -> `phonemeBuf` | phoneme conversion of a word (`TtsGetPhonemes`); not exercised |
| `GTVE` | | ack -> `version` | engine version |
| `WM_QUIT` (0x12) | | `ReturnSafeToExit` releases the sentinel | exit the engine |

Observed behaviour of queries: when a set command and its query are posted
back to back, the query's answer is the value *before* the set about one time
in two; with about 100 ms between them it is always current.  The wrapper
therefore trusts what it set (the `set` results are reliable) and uses queries
only for diagnostics.

## 7. Calls from the engine to the client

The engine makes three kinds of calls.  All use the same pattern: write
`command` (and the arguments) into the control block, signal the channel's
call event, block on the channel's return event, then read `result` if the
call has one.

### Audio object (`audioObjectCall` / `audioObjectReturn`)

The engine believes it is talking to a SAPI 4 `IAudioDest`.  Every call is
answered by writing `result` and setting `audioObjectReturn`.

| `command` | Call | Arguments | Expected answer |
|---|---|---|---|
| 0x4ce | QueryInterface | IID at 0x14c | S_OK for `IID_IAudio` {F546B340-C743-11CD-80E5-00AA003E4B50}, `IID_IAudioDest` {2EC34DA0-C743-11CD-80E5-00AA003E4B50} and IUnknown; E_NOINTERFACE for the STL-specific {74B26EA1-5830-101D-9152-040224007802} |
| 0x4e2 | Claim | | S_OK |
| 0x4e3 | Flush | | S_OK |
| 0x4e8 | Start | | S_OK |
| 0x4e9 | Stop | | S_OK |
| 0x4ec | UnClaim | | S_OK |
| 0x4ee | WaveFormatSet | 18-byte `WAVEFORMATEX` at 0x160 | S_OK |
| 0x4f0 | DataSet | chunk size in `param`, samples in `audioBuf` | S_OK; the client must copy the data before answering |
| 0x4f1 | FreeSpace | | client writes the free byte count to `param` (a large number keeps the engine flowing) and 0 to `eof_flag` |

The engine hands audio over in 11,000-byte chunks (half a second) and asks
`FreeSpace` before every one.

**Important:** for this channel the engine does `SetEvent(audioObjectCall)`
immediately followed by `ResetEvent(audioObjectCall)` and only then waits on
`audioObjectReturn`.  See section 16.

### Buffer notifications (`bufNotifyEvent` / `bufNotifyReturn`)

| `command` | Notification | `param` | `timestamp` |
|---|---|---|---|
| 0x4b5 | TextDataStarted | | audio position at the start of this text |
| 0x4b6 | TextDataDone | flags | audio position at its end |
| 0x4ba | WordPosition | byte offset of the word in the text (tags included) | audio position of the word |
| 0x4bb | BookMark | the N of `\Mrk=N\` | audio position of the mark |

The engine issues these while it "plays" (section 14), so their timestamps are
exact byte positions in the audio stream; the client computes offsets within
an utterance as `timestamp - timestamp(TextDataStarted)`.  Every notification
precedes `AudioStop` for the same text.  The engine does not do SetEvent +
ResetEvent on this channel.

### Audio notifications (`NotifyEvent` / `notifyReturn`)

| `command` | Notification |
|---|---|
| 0x4b0 | AudioStart |
| 0x4b1 | AudioStop: the engine's playback clock passed the end of the audio |
| 0x4b3 | AttribChanged: pitch / speed / volume / brightness changed, `param` says which |

## 8. Speaking text

1. Put the text (8-bit, NUL-terminated, tags allowed) into a pagefile-backed
   file mapping of at least `size + 16` bytes.
2. `DuplicateHandle` the mapping into the engine process and store the
   duplicated handle in `text_handles[write_idx]`; increment `pending`.
3. Post `TXTDTA` with `wParam = size`, then advance `write_idx` modulo 50.
4. Wait for the sentinel; `result` is the engine's acceptance (S_OK).
5. The engine calls `TextDataStarted`, then a stream of `FreeSpace` /
   `DataSet` pairs interleaved with `WordPosition` / `BookMark`, then
   `TextDataDone`, and finally `AudioStop` once its playback clock reaches the
   end.

Up to 50 texts can be queued, but the engine plays them strictly in sequence
and does not start the next one before `AudioStop` of the previous.  The
wrapper never queues more than one.

## 9. Cancelling

Post `AUDRES`.  The engine stops delivering audio, calls `TextDataDone` with a
flag for the current text, discards the queue, and its glue calls
`APIDoReset`, which releases `BufferSemaphore`.  A client must take
`BufferSemaphore` once after a cancel before speaking again, otherwise the
next `TXTDTA` may be swallowed.  Measured from `AUDRES` to `BufferSemaphore`:
about 2 ms.

## 10. Parameters and their limits

| Parameter | Range | Default | Handler in STLTTS.EXE |
|---|---|---|---|
| speed | 100..300 words/min | 200 | 0x4138e0 |
| pitch | male 70..150 Hz, female 145..300 Hz | 90 / 195 | 0x413920 |
| volume | 0..255 | 255 | 0x413a20 |
| brightness | 0..15 | male 8, female 6 | 0x4138a0 |
| gender | MAL / FEM | male | 0x4137e0 / 0x413820 |

Out-of-range values are rejected with 0x8000FFFF and leave the current value
unchanged.  Changing gender resets pitch, speed and brightness to the new
gender's defaults.

Acoustic verification (see `samples/probe/analysis.txt`): the measured
fundamental follows the pitch setting across the whole range for both genders,
duration is inversely proportional to speed, RMS level follows volume, and the
zero-crossing rate rises monotonically with brightness.

## 11. Reading modes

`MDE(index, value)` and `GMDE(index)`, handlers at 0x413a60..0x413b70.

| Index | Mode | Default |
|---|---|---|
| 0 | reset all modes | |
| 1 | spreadsheet / table | off |
| 2 | list | off |
| 3 | dollar | on |
| 4 | zero (say "zero" for 0) | off |
| 5 | number samples (NSAMPLES.DLL) | off |
| 6 | Japanese digit grouping | off; **crashes the engine** on text containing words |

`SNMB`/`GNMB` are an older way of setting mode 5.  Mode 4 was toggled on and
off in the probe without an audible difference on the test sentences; whether
it needs a particular context is unknown.

## 12. Inline tags

Parsed at 0x404fe0..0x4059e5, actions applied at 0x404c20.  A tag starts with
a backslash and ends at the next backslash; unknown tags are skipped
silently.

| Tag | Effect |
|---|---|
| `\Mrk=N\` | bookmark N; reported by `BookMark`.  Two marks with nothing speakable between them produce one report. |
| `\Pau=ms\` | pause; up to 30000 ms |
| `\Pit=Hz\` | pitch from here on (same limits as `PIT`) |
| `\Spd=wpm\` | speed from here on |
| `\Vol=0..65535\` | volume from here on, 16-bit scale |
| `\Vce=Gender=Male|Female|Neutral\` | switch the sample set mid-text |
| `\Emp\`, `\Emp=n\` | emphasis |
| `\Eng:PHO=...\` | phoneme string |
| `\Eng:PRN=word=pron,PRT=part\` | add a pronunciation |
| `\Eng:SET:NUMBER|DOLLAR|ZERO|JAPANESE|DICTIONARY|DEFAULT\` | set a mode |
| `\Eng:RST:...\` | reset a mode |
| `\Chr\`, `\Com\`, `\Ctx=...\`, `\Prt=...\`, `\Rst\` | character mode, comment, context, part of speech, reset |
| `\Pro\` | accepted, no effect |

`WordPosition` offsets count the tag bytes, so a client that inserts tags must
keep a map from engine byte to source character.

## 13. Text encoding

The engine reads single-byte text and knows the printable ASCII range plus a
few symbols.  Anything else is best transliterated by the client (the wrapper
maps accented Latin letters, typographic quotes and dashes, ellipsis, degree
and currency symbols to ASCII words or letters).  A backslash always starts a
tag, so it cannot be spoken.

## 14. Pacing and the clock patch

STLTTS.EXE simulates a real-time audio device: after handing a chunk to the
audio object it spins on `timeGetTime` (playback tick at 0x425500, set-up at
0x424f30) until the chunk's play time has elapsed, and only then issues the
next chunk, the notifications and finally `AudioStop`.  Consequences without
intervention: 100 % CPU while speaking, a 6-second utterance takes 6 seconds to
render, marks arrive in real time, and the next text cannot start earlier.

The wrapper's host patches the engine in memory right after start-up: the
import slot for `WINMM!timeGetTime` (RVA 0x0ea3bc) is pointed at a 12-byte
stub allocated in the engine with `VirtualAllocEx`:

```
B8 <real timeGetTime>   mov  eax, real
FF D0                   call eax
69 C0 <scale>           imul eax, eax, scale
C3                      ret
```

With scale 200 the engine believes time runs 200 times faster; it renders
about 175x real time (the synthesis itself becomes the limit), and the audio
is byte-identical to the unpatched engine's.  All timestamps stay exact
because they are byte positions, not times.

## 15. The start-up message boxes and the TTSAPI.DLL patch

A `CWinApp`-derived static initialiser in TTSAPI.DLL looks up the SAPI 4
`EngineLocation` registry value (`HKCR\CLSID\{77798141-...}\InProcServer32`)
and calls `AfxMessageBox` twice when it is missing.  Since the DLL is loaded
by STLTTS.EXE, that would pop two message boxes in the engine process every
time a registry-free client starts it.

`tools/patch_ttsapi.py` replaces the two 14-byte call sequences at file
offsets 0x3bc4 and 0x3bf2 (`6a 00 6a 00 68 24 ee 01 10 e8 fb 1e 01 00` and
`6a 00 6a 00 68 dc ed 01 10 e8 cd 1e 01 00`) with NOPs.  Nothing else in the
DLL changes; the original is kept in `bin/original`.

## 16. Engine bugs and quirks

**Lost audio-object pulses (hang).**  Because the engine sets and immediately
resets `audioObjectCall` before waiting for the answer, the signal is lost
whenever the client thread is not already blocked on the event - for example
while it is still finishing the previous call.  The engine then waits forever
on `audioObjectReturn`: no more audio, no acknowledgement of any command, no
crash.  At real-time pacing this practically never happened; at 200x it
happened about once per 40 utterances.  The wrapper's dispatcher therefore
waits on all call events with a 1 ms timeout and, when a wait times out with
a non-zero `command` that is unchanged on the next poll, serves it anyway
(clearing `command` after each call so nothing is served twice).  Lost pulses
are then recovered within about 2 ms; in a stress run of 40 utterances five
were recovered and none hung.

**Japanese mode crash.**  Mode 6 makes the engine fault on text containing
words.  The wrapper masks it.

**Query lag.**  See section 6.

**Zero mode.**  No audible effect was found; see section 11.

**Get/set after a gender change.**  `MAL` / `FEM` silently reset pitch, speed
and brightness; a client that caches parameters must re-send them.

**Brightness, sample loading and `SNMB` are not acknowledged.**  A client must
not wait for the sentinel after them (the wrapper posts them and continues;
the next acknowledged command orders them).

**Adjacent marks.**  `\Mrk=1\\Mrk=2\` yields one `BookMark` report (for the
first mark).

## 17. Code landmarks

STLTTS.EXE (image base 0x400000):

| Address | What |
|---|---|
| 0x40c7e0 / 0x40c8e0 | main loop and message pump |
| 0x429d70 | command dispatcher (window messages to handlers) |
| 0x4137e0, 0x413820 | gender (MAL, FEM) |
| 0x4138a0 | brightness |
| 0x4138e0 | speed |
| 0x413920 | pitch |
| 0x413a20 | volume |
| 0x413a60 .. 0x413b70 | modes |
| 0x404fe0 .. 0x4059e5 | tag parser |
| 0x404c20 | action processor (applies parsed tags) |
| 0x424f30 | audio set-up (`WaveFormatSet`, `Claim`, `Start`) |
| 0x425500 | playback tick (the `timeGetTime` loop) |
| import slot RVA 0x0ea3bc | `WINMM!timeGetTime` |

TTSAPI.DLL (image base 0x10000000):

| Address | What |
|---|---|
| 0x10001970 | audio-object `QueryInterface` call (SetEvent / ResetEvent / Wait pattern) |
| 0x10001c40 | `DataSet` call (copies the chunk into `audioBuf`, size to 0x140) |
| 0x10003fa0 / 0x10004000 | `CallTextdataDone` / `CallTextdataStarted` |
| 0x100043e0 / 0x10004440 | `ReturnBookMark` / `ReturnWordpos` |
| 0x10002546 .. 0x1000271c | `InitGlueServer`: opens the named objects, creates the glue thread |
| 0x10003400, 0x10003590, 0x10003660, 0x100036b0 | the client half's notification and audio threads (SAPI 4 side; not used by the wrapper) |
| file offsets 0x3bc4, 0x3bf2 | the patched `AfxMessageBox` calls |

## 18. Method

- Disassembly: capstone with `skipdata`, driven by pefile for sections,
  imports and exports; annotated with import names, string references and
  the DLL's export names (`scratch tools, not shipped`).
- GUIDs were located by scanning for the SAPI 4 IIDs from `speech.h`.
- Every hypothesis about a message or a field was tested by sending it from
  the probe (`test/cybertalk_probe.cpp`) and inspecting the control block and
  the audio.  The probe writes a WAV per experiment; `samples/probe` is its
  output and `samples/probe/analysis.txt` the acoustic measurements
  (fundamental frequency by autocorrelation, duration, RMS, zero-crossing
  rate).
- Throughput, cancel latency and hang behaviour were measured with the SAPI
  test client (`test/sapi_test.cpp`) against the finished wrapper.
