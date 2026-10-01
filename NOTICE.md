# Notices

The code written for this project is licensed under the MIT License (see [LICENSE](LICENSE)). The material
below is not covered by that licence and stays under its own terms.

## The CyberTalk engine binaries

This license covers the wrapper source code only.  The CyberTalk engine
binaries (STLTTS.EXE, TTSAPI.DLL, MSAMPLES.DLL, FSAMPLES.DLL, NSAMPLES.DLL)
are the property of their original copyright holders (Panasonic / Speech
Technology Laboratory, 1997) and are not covered by this license.

## Not covered: the SAPI 5 COM skeleton from the BestSpeech SAPI 5 wrapper

The files below are an exception to the statement above that the license covers the wrapper
source code. They were adapted from the SAPI 5 COM server and token enumerator skeleton of
the BestSpeech SAPI 5 wrapper by Gozaltech (<https://github.com/gozaltech/BstSpeech-sapi>),
they are not the work of this project's author, and the MIT License does not cover them.
They stay under their original author's terms.

- `src/com.hpp` and `src/com.cpp`
- `src/registry.hpp` and `src/registry.cpp`
- `src/utils.hpp`
- `src/ISpDataKeyImpl.hpp` and `src/ISpDataKeyImpl.cpp`
- `src/IEnumSpObjectTokensImpl.hpp` and `src/IEnumSpObjectTokensImpl.cpp`
- `src/voice_token.hpp` and `src/voice_token.cpp`
