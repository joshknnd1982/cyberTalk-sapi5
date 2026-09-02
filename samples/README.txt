CyberTalk SAPI5 - sample renders
================================

probe\        67 WAV files rendered straight from the CyberTalk engine by
              cybertalk_probe.exe: both genders, pitch / speed / volume /
              brightness sweeps, every reading mode, every inline tag, numbers,
              punctuation and Unicode transliteration.  probe_report.txt lists
              what each file contains; analysis.txt has the measured pitch,
              duration, loudness and brightness that prove each parameter works.

sapi_x86\     The engine driven through the 32-bit SAPI 5 DLL
              (sapi_test_x86.exe): the three voices, SAPI rate -5/+5/+10,
              volume 50, XML (bookmarks, pitch, silence, spell, volume, rate)
              and Unicode text.  sapi_x86_console.txt is the test's output
              including the word / sentence / bookmark events.

sapi_x64_md5.txt
              MD5 sums of the same renders made through the 64-bit DLL
              (sapi_x64_console.txt is that run's output).  They are
              byte-identical to the sapi_x86 files, so the WAVs themselves are
              not duplicated in the repository; the full set, including the
              64-bit files and the 340-second cancellation test, is in
              CyberTalk-samples.zip on the Releases page.

The "Custom Voice" files in sapi_x86 were rendered with a settings.ini of:
female, pitch 250 Hz, speed 150 words/min, volume 200, brightness 12, zero
mode and recorded numbers on - which is why that voice sounds different from
"CyberTalk Female".
