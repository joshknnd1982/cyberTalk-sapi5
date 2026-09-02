#!/usr/bin/env python
"""Neutralise the two start-up message boxes in CyberTalk's TTSAPI.DLL.

STLTTS.EXE (the CyberTalk engine process) statically imports TTSAPI.DLL for its
half of the shared-memory glue protocol.  When TTSAPI.DLL loads it runs an MFC
CWinApp constructor that reads HKCR\\CLSID\\{77798141-...}\\InProcServer32 and,
if the key is absent, shows two modal AfxMessageBox dialogs *inside the engine
process*.  The value it reads (EngineLocation) is only used by the SAPI 4
client half of the DLL, which this project does not use.

This script replaces the two "push/push/push/call AfxMessageBox" sequences
(14 bytes each, file offsets 0x3bc4 and 0x3bf2) with NOPs so the engine runs
without any registry entry.  The original file is preserved as
bin/original/TTSAPI.DLL.  Running the script twice is harmless.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "bin", "TTSAPI.DLL")
ORIG = os.path.join(HERE, "..", "bin", "original", "TTSAPI.DLL")

EXPECTED = {
    0x3BC4: bytes.fromhex("6a006a006824ee0110e8fb1e0100"),
    0x3BF2: bytes.fromhex("6a006a0068dced0110e8cd1e0100"),
}


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    data = bytearray(open(src, "rb").read())
    if all(data[o:o + 14] == b"\x90" * 14 for o in EXPECTED):
        print("already patched:", src)
        return 0
    for off, expected in EXPECTED.items():
        if bytes(data[off:off + 14]) != expected:
            print("unexpected bytes at 0x%06x: %s" % (off, data[off:off + 14].hex()))
            return 1
    os.makedirs(os.path.dirname(ORIG), exist_ok=True)
    if not os.path.exists(ORIG):
        shutil.copy2(src, ORIG)
    for off in EXPECTED:
        data[off:off + 14] = b"\x90" * 14
    open(src, "wb").write(data)
    print("patched:", src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
