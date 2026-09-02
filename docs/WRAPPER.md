# The SAPI 5 wrapper: how it is built

```
SAPI application (x86 or x64)
  -> CyberTalkSAPI.dll            ISpTTSEngine, one build per architecture
     -> \\.\pipe\CyberTalkTTS     byte-stream named pipe, src/pipe_protocol.h
        -> CyberTalkHost.exe      32-bit, single instance, owns the engine
           -> STLTTS.EXE          driven as described in REVERSE_ENGINEERING.md
```

The engine is a 32-bit process with a shared-memory protocol, so a 64-bit
application cannot host it in-process.  A small 32-bit host owns the engine
and serves any number of clients over a named pipe; both the x86 and the x64
SAPI DLL and the configuration utility are pipe clients.

## Source map

| File | Role |
|---|---|
| `src/stl_engine.h/.cpp` | the direct engine client: kernel objects, launch, dispatcher thread, commands, `speak()`, cancel, hang detection, clock patch |
| `src/stl_text.h/.cpp` | Unicode to engine-text transliteration with a byte-to-character map |
| `src/cybertalk_host.cpp` | the host: pipe server, parameter cache, libsonic rate boost, engine restart, idle exit |
| `src/pipe_protocol.h` | the pipe protocol (below) |
| `src/pipe_client.h/.cpp` | pipe client used by the DLL and the utility; finds or launches the host |
| `src/ISpTTSEngineImpl.hpp/.cpp` | the SAPI engine object: text composition, rate/pitch/volume mapping, events, cancel |
| `src/IEnumSpObjectTokensImpl.*`, `src/voice_token.*`, `src/voice_attributes.hpp`, `src/ISpDataKeyImpl.*` | the voice token enumerator SAPI calls to list the three voices |
| `src/sapi_main.cpp`, `src/cybertalk_sapi.def`, `src/com.*`, `src/registry.*` | COM plumbing and registration |
| `src/ct_settings.h` | settings file (`%APPDATA%\CyberTalkSAPI\settings.ini`) |
| `src/ct_log.h` | logger |
| `src/config/*` | the configuration utility |

## Registration

`DllRegisterServer` (run by the installer with `regsvr32` for each
architecture) writes:

- `HKLM\Software\Classes\CLSID\{6F0D1C3E-8A52-4D7B-9C1E-2B5E7A9F4C01}` - the
  engine (`ISpTTSEngine`), `InProcServer32` = the DLL, `ThreadingModel` = Both
- `HKLM\Software\Classes\CLSID\{6F0D1C3E-8A52-4D7B-9C1E-2B5E7A9F4C02}` - the
  token enumerator
- `HKLM\Software\Microsoft\Speech\Voices\TokenEnums\CyberTalk` with
  `CLSID` = the enumerator

The 32-bit DLL registers under the WOW6432 view and the 64-bit DLL under the
native view, so 32-bit and 64-bit applications each find their own engine.
SAPI asks the enumerator for the voices, which are generated in code
(`voice_attributes.hpp`): `CyberTalk Male`, `CyberTalk Female`, `CyberTalk
Custom Voice`, vendor Panasonic, language 409, gender from the settings for
the custom voice.  Token ids look like
`HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\TokenEnums\CyberTalk\CyberTalk Male`.

The installer also writes `HKLM\SOFTWARE\CyberTalkSAPI\InstallDir` (32-bit
view), which the pipe client uses to find the host when the DLL lives
elsewhere.

## The pipe protocol (version 3)

Byte-stream pipe `\\.\pipe\CyberTalkTTS`, one connection per client, every
message a packed header `{uint32 type, uint32 size}` followed by `size` bytes.

Client to host:

| Type | Payload | Answer |
|---|---|---|
| `CMD_PING` (1) | | `RESP_PONG` |
| `CMD_GET_INFO` (2) | | `RESP_INFO`: `InfoResponse` with ranges, defaults, engine and host pids, protocol version |
| `CMD_SPEAK` (3) | `SpeakCommand` + text | the stream below, ending with `RESP_SPEAK_END` |
| `CMD_STOP` (4) | | `RESP_OK`; the running speak ends with `RESP_SPEAK_END` status 1 |
| `CMD_SHUTDOWN` (5) | | `RESP_OK`, then the host exits (used by uninstall) |
| `CMD_GET_STATE` (6) | | `RESP_STATE`: `StateResponse` with the current engine parameters |

`SpeakCommand`: `gender` (1 female, 2 male), `pitch` (Hz), `speed` (wpm),
`volume` (0..255), `bright` (0..15), `modes` (bit mask: 1 spreadsheet, 2
list, 4 dollar, 8 zero, 16 number samples, 32 Japanese - masked by the host),
`sonic_speed` (float, 1.0 = none), `flags`, `text_length`.  The text is
engine text (8-bit, tags included), composed by the client.

Host to client during a speak:

| Type | Payload | Meaning |
|---|---|---|
| `RESP_AUDIO` (104) | PCM bytes | audio, in the order produced, after time compression if any |
| `RESP_MARK` (105) | `MarkResponse {id, byte_offset}` | a `\Mrk=id\` was reached at this byte offset of the (compressed) audio |
| `RESP_WORD` (106) | `MarkResponse {text_pos, byte_offset}` | a word starts at engine text byte `text_pos` |
| `RESP_SPEAK_END` (107) | `SpeakEndResponse {status, hresult, total_bytes}` | 0 done, 1 cancelled, 2 error |
| `RESP_ERROR` (101) | `ErrorResponse {hresult, message}` | any command can fail this way |

A client cancels by sending `CMD_STOP` while the host is streaming; the host
checks for it (`PeekNamedPipe`) every time the engine hands over a chunk, and
the client keeps reading until `RESP_SPEAK_END` so the host never blocks on a
full pipe.  Mark and word offsets are divided by the time-compression factor
and rounded to a sample boundary.

## The host

- Single instance (`Local\CyberTalkHostMutex`); a client that finds no pipe
  launches it (`Local\CyberTalkHostLaunchMutex` prevents two launches) from
  the DLL's folder, its parent, or the `InstallDir` registry value.
- Finds the engine in `engine\`, `.`, `bin\`, `..\bin` or `..\engine` next to
  itself.
- Starts the engine on first use, kills a stale `STLTTS.EXE` from an earlier
  crash, applies the clock patch, logs the version.
- Keeps a cache of the parameters it last set and only sends what changed.  A
  gender change invalidates pitch, speed and brightness (the engine resets
  them); text that carried tags invalidates everything (tags may change
  engine state persistently).
- Rate boost: when `sonic_speed` is not 1.0 the audio goes through libsonic
  (speed change without pitch change) before it is sent.
- Watches for the engine's message boxes (there should be none with the
  patched DLL) and dismisses them; restarts the engine if it exits; declares
  it hung when no handshake happens for 3 s beyond the scaled playback time
  it may still owe, or a command is not acknowledged within 3 s, then kills
  and restarts it.
- Exits after ten minutes without clients.  A screen reader keeps its voice
  object, hence its pipe, open, so the host stays up while the screen reader
  runs.

## The SAPI engine DLL

`Speak()` walks SAPI's fragment list and composes one engine text:

- `SPVA_Speak` text is transliterated; a byte-to-character map (`refs`) is
  kept for every engine byte.
- `SPVA_SpellOut` spaces the characters out.
- `SPVA_Silence` becomes `\Pau=ms\`.
- `SPVA_Bookmark` becomes `\Mrk=N\` and so does every sentence start when the
  application asked for sentence events.  Two marks at the same position are
  folded into one, because the engine reports only one.
- Per-fragment rate, pitch and volume changes become `\Spd=\`, `\Pit=\` and
  `\Vol=\` tags.

Parameter mapping:

| SAPI | Engine |
|---|---|
| rate r (-10..10) | speed = base x 3^(r/10), clamped to 100..300; if the target is above 300 and rate boost is on, `sonic_speed` = target / 300 |
| pitch p (-10..10, `absmiddle`) | pitch = base x 2^(p/24), clamped to the gender's range |
| volume v (0..100, `SetVolume` and `<volume>`) | volume = base x v/100 x fragment volume/100 |

Events: `SPEI_TTS_BOOKMARK` (string and, when numeric, the number in wParam),
`SPEI_SENTENCE_BOUNDARY`, `SPEI_WORD_BOUNDARY` (from the engine's word
positions mapped back through `refs`; only alphanumeric word starts are
reported).  Audio is written to the SAPI site in 20 ms slices with
`GetActions()` checked between them so a purge is honoured within one slice.
`SPVES_ABORT` and `SPVES_SKIP` cancel through the pipe.

The base parameters come from the voice: the male and female voices use the
engine defaults, the custom voice reads `settings.ini` (re-read whenever the
file's timestamp changes, so the configuration utility's Save takes effect on
the next utterance).

## Settings file

`%APPDATA%\CyberTalkSAPI\settings.ini`, written by the configuration utility:

```ini
[CustomVoice]
Gender=2          ; 2 male, 1 female
Pitch=90
Speed=200
Volume=255
Brightness=8
DollarMode=1
ZeroMode=0
NumberSamples=0
SpreadsheetMode=0
ListMode=0
[General]
RateBoost=1
Logging=1
WordEvents=1
```

Values are clamped to the engine's ranges when read.

## Logging

`%LOCALAPPDATA%\CyberTalkSAPI\logs\{sapi_x86,sapi_x64,host,config}.log`,
each line stamped with time, pid and thread id.  Files are opened per write
with `_SH_DENYNO` so several processes can append, and roll over to `.old` at
4 MB.  `Logging=0` in the settings file switches the DLL and the utility off;
the host follows the DLL.

## The configuration utility

A plain Win32 dialog (`src/config/CyberTalkConfig.rc`).  Accessibility rules
that were applied and verified with `test/msaa_dump.cpp`:

- every focusable control has a text label placed directly before it in the
  resource order, because MSAA names an edit or slider from the static that
  precedes it in Z-order and stops at the first non-static window;
- the sliders run 0..100 so the percentage a screen reader announces equals
  the value; the label of the paired edit carries the engine value
  ("Pitch % (90 Hz):");
- the up-down buttons are not tab stops (the edit takes the arrow keys);
- Save confirms with a message box, which screen readers announce.
