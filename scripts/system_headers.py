# PlatformIO pre-script: mark the Teensy core and the framework libraries the Audio Library
# pulls in as *system* headers (-isystem), so our strict warnings (-Wextra, -Wdouble-promotion)
# report problems in our code only, not in third-party headers.
# GCC ignores a -I dir that is also given with -isystem, so header resolution is unchanged.
from os.path import isdir, join

Import("env")

fw = env.PioPlatform().get_package_dir("framework-arduinoteensy")
dirs = [join(fw, "cores", "teensy4")]
for lib in ["Audio", "SD", "SdFat", "Wire", "SPI", "SerialFlash"]:
    for sub in ["", "src", "utility"]:
        d = join(fw, "libraries", lib, sub)
        if isdir(d) and (sub or not isdir(join(d, "src"))):
            dirs.append(d)

for d in dirs:
    env.Append(CCFLAGS=["-isystem", d])
