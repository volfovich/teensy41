# PlatformIO pre-build script: regenerates lib/dsp/src/dsp/drums/DrumKitData.cpp from
# kits/*/ and DrumSongData.cpp from songs/*.txt when they changed (see
# tools/make_kits.py and tools/make_songs.py).
import os
import subprocess

Import("env")  # noqa: F821 (provided by PlatformIO)

# This firmware always runs at 48 kHz (lib/i2s32); the kits must match it.
rate = 48000

tools = os.path.join(env.subst("$PROJECT_DIR"), "tools")  # noqa: F821
subprocess.check_call([env.subst("$PYTHONEXE"), os.path.join(tools, "make_kits.py"), "--rate", str(rate)])  # noqa: F821
subprocess.check_call([env.subst("$PYTHONEXE"), os.path.join(tools, "make_songs.py")])  # noqa: F821
