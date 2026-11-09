"""Assemble the source release for GitHub: dist/github/splatoon-raiders-recomp.

Only the project's own code and tools: no game files and nothing derived from them (no work/ (generated C, prelinked
modules), build/, dist/, executables, shader or pipeline caches, saves, logs, screenshots). A check at the end refuses
the package if any of those slipped in. The game is identified by the supported update's main build ID.

Never removes anything: an existing package folder is moved to work/shots_old/github_<time>/ first.

Usage: package_source.py [--out DIR]
"""
import argparse
import hashlib
import os
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIRS = ['host', 'runtime', 'shader', 'recomp', 'tests', 'third_party', 'tools']
FILES = ['COMO_JOGAR.md', 'HOW_TO_PLAY.md', 'ROADMAP.md', 'THIRD_PARTY_NOTICES.md']
SKIP_DIRS = {'streamline_dl', 'build', '__pycache__', '.git', 'shots', 'profile', 'logs'}
SKIP_EXT = {'.exe', '.pdb', '.obj', '.o', '.lib', '.dll', '.bin', '.log', '.png', '.ninja', '.img', '.nso', '.nca',
            '.nsp', '.xci', '.npdm', '.spv', '.code', '.ctrl'}
# the update this port was made from (exefs of update 65536 / version 65536)
SUPPORTED = {
    'title_id': '0400380020056000',
    'version': '65536',
    'main_build_id': '663C2E087A083F05307F229DFC4042D5BCC906B6',
    'main_sha256': '35fe1cbe7ea983e1e6ce8716b98e884a2f6cfb02c9514d51ec2c00daa4addcde',
}
GPL = ROOT.parent / 'projetos' / 'eglantine' / 'LICENSE.txt'

README = """# Splatoon Raiders (Switch 2) — PC recompilation

A static recompilation of Splatoon Raiders for Windows x64 (Vulkan), with a launcher (display, FSR 1 / NVIDIA Image
Scaling, frame rate, controllers, PT/EN), DualSense / DualShock 4 / XInput support with gyro aiming, a performance
overlay and shader preparation before the game.

**This repository contains no game code or data.** You need your own legally obtained copy of the game, dumped from
your console. The build converts *your* copy into the program; nothing from the game is ever distributed here.

## Supported game version

| | |
|---|---|
| Title ID | `{title_id}` |
| Update | version `{version}` (exefs + merged romfs of that update) |
| `exefs/main` build ID | `{main_build_id}` |
| `exefs/main` SHA-256 | `{main_sha256}` |

Other versions are not supported: the program checks the exefs it was built from.

## Requirements

- Windows 10/11 x64, a GPU with Vulkan 1.3 (current drivers)
- LLVM Clang 19+ and Ninja; CMake 3.22+; Python 3.10+ with numpy
- Vulkan SDK 1.3/1.4
- For the shader translator: fmt, sirit (eden-emulator/sirit) and Boost headers in one folder (`NX2_DEPS`)

## Building

```
# 1. your dump: a folder with exefs/ and romfs/ of update {version}
set SPLATOON_GAME_DIR=D:\\dumps\\splatoon_raiders\\update_{version}
set LLVM_DIR=C:\\Program Files\\LLVM
set VULKAN_SDK=C:\\VulkanSDK\\1.4.363.0

# 2. the shader translator (third_party/eglantine, GPL-3.0-or-later)
cmake -S shader -B shader/build -G Ninja -DNX2_DEPS=C:\\deps -DCMAKE_BUILD_TYPE=Release
cmake --build shader/build

# 3. lift the game's code to C, link its modules, compile everything
python recomp/gen.py --exefs %SPLATOON_GAME_DIR%\\exefs --out work/gen
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
"""

GITIGNORE = """# never commit game files or anything built from them
work/
build/
dist/
shader/build/
*.exe
*.pdb
*.bin
*.nso
*.nca
*.nsp
*.xci
exefs/
romfs/
profile/
logs/
splatoon.ini
__pycache__/
"""


def skip(p: Path) -> bool:
    return any(part in SKIP_DIRS for part in p.parts) or p.suffix.lower() in SKIP_EXT


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'dist' / 'github' / 'splatoon-raiders-recomp')
    args = ap.parse_args()
    out = args.out
    if out.exists() and any(f.is_file() for f in out.rglob('*')):
        aside = ROOT / 'work' / 'shots_old' / time.strftime('github_%Y%m%d_%H%M%S')
        aside.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.rename(out, aside)   # a plain rename: never a copy-then-delete
            print(f'previous package moved to {aside}')
        except OSError:   # in use (open in Explorer): the new package goes beside it
            out = out.with_name(out.name + time.strftime('-%Y%m%d-%H%M%S'))
            print(f'previous package in use: writing {out}')
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for d in DIRS:
        for src in sorted((ROOT / d).rglob('*')):
            rel = src.relative_to(ROOT)
            if not src.is_file() or skip(rel):
                continue
            dst = out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            n += 1
    for f in FILES:
        shutil.copy2(ROOT / f, out / f)
        n += 1
    (out / 'README.md').write_text(README.format(**SUPPORTED), encoding='utf-8', newline='\n')
    (out / '.gitignore').write_text(GITIGNORE, encoding='utf-8', newline='\n')
    if GPL.exists():
        shutil.copy2(GPL, out / 'LICENSE')
    # refuse the package if anything game-derived slipped in
    game_main = Path(os.environ.get('SPLATOON_GAME_DIR', r'C:/Program Files (x86)/nintendo_s_2games/splatoon_raiders/updates/65536')) / 'exefs' / 'main'
    bad = []
    big = []
    for f in out.rglob('*'):
        if not f.is_file():
            continue
        rel = f.relative_to(out)
        if skip(rel) or rel.parts[0] in ('work', 'build', 'dist'):
            bad.append(rel)
        if f.stat().st_size > 2_000_000:
            big.append((f.stat().st_size, rel))
    if game_main.exists():   # no file may contain a 4 KiB run of the game's main executable
        probe = game_main.read_bytes()[0x1000:0x2000]
        for f in out.rglob('*'):
            if f.is_file() and probe in f.read_bytes():
                bad.append(f.relative_to(out))
    if bad:
        raise SystemExit('refused, game-derived or binary files: ' + ', '.join(map(str, bad[:20])))
    total = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'{n} files, {total / 1048576:.1f} MiB -> {out}')
    for s, r in sorted(big, reverse=True)[:5]:
        print(f'  large: {r} ({s / 1048576:.1f} MiB)')


if __name__ == '__main__':
    main()
