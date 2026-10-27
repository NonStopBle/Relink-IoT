# Links src/<mcu>/libRelinkIoT.a into the firmware (PlatformIO).
Import("env")
from os.path import isdir, join, realpath
mcu = env.BoardConfig().get("build.mcu")
path = realpath(join("src", mcu))
if not isdir(path):
    raise SystemExit("RelinkIoT: no precompiled build for '%s'" % mcu)
DefaultEnvironment().Append(LIBPATH=[path], LIBS=["RelinkIoT"])
