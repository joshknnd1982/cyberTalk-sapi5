# Credits

## CyberTalk text-to-speech engine

CyberTalk (engine version 1.0.225, 1997) was developed by Panasonic's Speech
Technology Laboratory (STL) in Santa Barbara, California.  The engine files
shipped in the `engine` folder (STLTTS.EXE, TTSAPI.DLL, MSAMPLES.DLL,
FSAMPLES.DLL and NSAMPLES.DLL) remain the property of their copyright holders.
They are redistributed here unmodified except for TTSAPI.DLL, in which two
start-up message boxes that required an obsolete registry key were disabled
(see `tools/patch_ttsapi.py`; the original file is kept in `bin/original`).

The engine files were preserved and supplied by **@rommix0** (see
`bin/readme_first.txt`); without that copy this project would not exist.

## SAPI5 wrapper

The SAPI5 wrapper, the CyberTalk host, the configuration utility, the
reverse-engineering notes and the installer were written for this project by
joshknnd1982 with help from Claude.  The COM plumbing (`com.hpp`,
`registry.hpp`, `ISpDataKeyImpl`) was adapted from the BestSpeech SAPI5
wrapper in the same repository family.

## Third-party code

- [libsonic](https://github.com/waywardgeek/sonic) by Bill Cox, Apache License
  2.0 - used for time compression of speech beyond the engine's 300 words per
  minute ceiling.
- [Inno Setup](https://jrsoftware.org/isinfo.php) by Jordan Russell and Martijn
  Laan - installer.
