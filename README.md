# Splatoon Raiders (Switch 2) — PC recompilation

A static recompilation of Splatoon Raiders for Windows x64 (Vulkan), with a launcher (display, FSR 1 / NVIDIA Image
Scaling, frame rate, controllers, PT/EN), DualSense / DualShock 4 / XInput support with gyro aiming, a performance
overlay and shader preparation before the game.

**This repository contains no game code or data.** You need your own legally obtained copy of the game, dumped from
your console. The build converts *your* copy into the program; nothing from the game is ever distributed here.

## Supported game version

| | |
|---|---|
| Title ID | `0400380020056000` |
| Update | version `65536` (exefs + merged romfs of that update) |
| `exefs/main` build ID | `663C2E087A083F05307F229DFC4042D5BCC906B6` |
| `exefs/main` SHA-256 | `35fe1cbe7ea983e1e6ce8716b98e884a2f6cfb02c9514d51ec2c00daa4addcde` |

Other versions are not supported: the program checks the exefs it was built from.

## Requirements

- Windows 10/11 x64, a GPU with Vulkan 1.3 (current drivers)
- LLVM Clang 19+ and Ninja; CMake 3.22+; Python 3.10+ with numpy
- Vulkan SDK 1.3/1.4
- For the shader translator: fmt, sirit (eden-emulator/sirit) and Boost headers in one folder (`NX2_DEPS`)

## Building

```
# 1. your dump: a folder with exefs/ and romfs/ of update 65536
set SPLATOON_GAME_DIR=D:\dumps\splatoon_raiders\update_65536
set LLVM_DIR=C:\Program Files\LLVM
set VULKAN_SDK=C:\VulkanSDK\1.4.363.0

# 2. the shader translator (third_party/eglantine, GPL-3.0-or-later)
cmake -S shader -B shader/build -G Ninja -DNX2_DEPS=C:\deps -DCMAKE_BUILD_TYPE=Release
cmake --build shader/build

# 3. lift the game's code to C, link its modules, compile everything
python recomp/gen.py --exefs %SPLATOON_GAME_DIR%\exefs --out work/gen
python tools/prelink.py
python tools/build.py --host
python tools/build_launcher.py
python tools/package.py
```

`tools/build_launcher.py` also makes `Launcher.exe` alone (the settings menu and shader preparation, no game code)
and its release zip; it starts the `SplatoonRaiders.exe` next to it.

### The player release (Set up)

Players do not build anything by hand: the release zip has the launcher, the runtime as a DLL and the tools to build
the game on their PC, as the Pokopia PC launcher does.

```
python tools/build_rt_dll.py      # build/rtdll/SplatoonRaiders_rt.dll (+ .lib): the runtime, no game code
python tools/build_launcher.py    # build/launcher/Launcher.exe
python tools/package_setup.py     # dist/github/SplatoonRaiders-win64.zip: + a portable Python with numpy and Zig
```

In the launcher, Set up runs `tools/setup_build.py` with the bundled Python and Zig: `recomp/gen.py` lifts the
player's own exefs to C, Zig compiles it and links `SplatoonRaiders.exe` against `SplatoonRaiders_rt.dll`
(`host/rt_glue.h`). Then the launcher translates the game's shaders from the romfs (`host/nx2_shaderprep.c`).

The playable folder is then `dist/Splatoon Raiders Recomp` (`SplatoonRaiders.exe`, guides). See
[COMO_JOGAR.md](COMO_JOGAR.md) / [HOW_TO_PLAY.md](HOW_TO_PLAY.md) for the settings and controls, and
[ROADMAP.md](ROADMAP.md) for the technical history.

## License

GPL-3.0-or-later (the shader translator derives from yuzu/Eden). Third-party code and licenses:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Splatoon is a trademark of Nintendo; this project is not affiliated
with Nintendo.
