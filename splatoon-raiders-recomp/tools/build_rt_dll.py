"""Build the runtime as a DLL for the Set up release: build/rtdll/SplatoonRaiders_rt.dll and its import library.

The DLL is the project's own code (kernel, memory, loader, graphics, audio, input, the shader translator): it holds no
game code and is shipped with Launcher.exe. The player's PC then builds SplatoonRaiders.exe from their own game files
(tools/setup_build.py) and links it against this DLL. Compiled with -DNX2_RT_DLL: the module glue comes from the
executable at start (host/rt_glue.h, host/rt_dll.c).

Usage: build_rt_dll.py [--gen work/gen] [--jobs N]
"""
import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402  (paths, flags and the runtime's file list)

ROOT = build.ROOT
# what the lifted game code calls (llvm-nm -u of the generated objects): exported by name
EXPORTS = ['rt_main', 'rt_dispatch', 'rt_udf', 'rt_svc', 'rt_ticks', 'fnprof_hit']


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, default=ROOT / 'work' / 'gen', help='gen.py output (layout.h, patches.h)')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'rtdll')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    gen, out = args.gen.resolve(), args.out.resolve()
    (out / 'obj').mkdir(parents=True, exist_ok=True)
    xid = hashlib.sha1(b''.join((ROOT / 'shader' / 'build' / p).read_bytes()
                                for p in ('nx2_xlate.lib', 'shader_recompiler.lib'))).hexdigest()[:16]
    inc = (f'-DNX2_RT_DLL -DNX2_XLATE_ID=0x{xid}ull -I "{ROOT / "host"}" -I "{ROOT / "host" / "compat_sdl"}" '
           f'-I "{build.VULKAN / "Include"}" -I "{ROOT / "shader"}" -I "{gen}" -include "{ROOT / "host" / "sp_compat.h"}"')
    dll, lib = out / 'SplatoonRaiders_rt.dll', out / 'SplatoonRaiders_rt.lib'
    exports = ' '.join(f'-Wl,/EXPORT:{e}' for e in EXPORTS)
    libs = (f'"{build.VULKAN / "Lib" / "vulkan-1.lib"}" -lwinmm -luser32 -ladvapi32 -lbcrypt -lgdi32 -lshell32 -lole32 '
            f'-lsetupapi -lhid "{build.BUILTINS}" {build.XLATE_LIBS}')
    lines = [
        f'rtflags = {build.RT_FLAGS} {inc}',
        f'rule cc_rt\n  command = "{build.CLANG}" $rtflags -MD -MF $out.d -c $in -o $out\n  deps = gcc\n'
        f'  depfile = $out.d\n  description = rt $in',
        f'rule link_dll\n  command = "{build.CLANG}" -shared -fuse-ld=lld -Wl,/DEBUG "-Wl,/IMPLIB:{lib.as_posix()}" {exports} '
        f'-o $out @$out.rsp {libs}\n  rspfile = $out.rsp\n  rspfile_content = $in_newline\n  description = link $out',
    ]
    objs = []
    for src in [ROOT / 'host' / n for n in build.HOST] + [ROOT / 'host' / 'rt_dll.c']:
        o = out / 'obj' / f'{src.stem}.o'
        lines.append(f'build {build.esc(o)}: cc_rt {build.esc(src)}')
        objs.append(o)
    lines.append(f'build {build.esc(dll)}: link_dll ' + ' '.join(build.esc(o) for o in objs))
    lines.append(f'default {build.esc(dll)}')
    (out / 'rtdll.ninja').write_text('\n'.join(lines) + '\n')
    r = subprocess.call([build.NINJA, '-C', str(out), '-f', 'rtdll.ninja', f'-j{args.jobs}'])
    if not r:
        print(f'{dll} ({dll.stat().st_size // 1024} KiB), {lib}')
    return r


if __name__ == '__main__':
    sys.exit(main())
