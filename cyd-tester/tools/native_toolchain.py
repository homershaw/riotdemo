# PlatformIO pre-script for env:native: put a host GCC on PATH for this build only.
# Set CYD_HOST_GCC to the folder holding g++.exe to override the default.
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

default = r"C:\Users\scada\tools\winlibs-gcc-16.2.0\mingw64\bin"
gcc_dir = os.environ.get("CYD_HOST_GCC", default)
if os.path.isdir(gcc_dir):
    env.PrependENVPath("PATH", gcc_dir)  # noqa: F821
# Link the test program statically so it runs without the GCC runtime DLLs on PATH.
env.Append(LINKFLAGS=["-static"])  # noqa: F821
