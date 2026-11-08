"""The Set up: build SplatoonRaiders.exe on the player's PC from their own game files (run by Launcher.exe).

  1. lift: recomp/gen.py turns the game's exefs (main, subsdk0, sdk) into C, in <work>/gen
  2. glue: <work>/glue.c, the module tables and the exefs hashes for the runtime, and main()
  3. compile: every generated file with the bundled Zig (clang for x86_64-windows-gnu), into <work>/obj
  4. link: <program>/SplatoonRaiders.exe against SplatoonRaiders_rt.dll (the runtime, shipped built)

Every step keeps its results, so a stopped set up continues where it was. Progress on stdout, one line each:
"step <name>", "build <done> <total>", "done", or "error <text>".

Usage: setup_build.py --game DIR --program DIR [--work DIR] [--zig PATH] [--jobs N]
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # the folder with recomp/, tools/ and include/ (or the repository)
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-Wno-unused-variable',
             '-Wno-unused-but-set-variable', '-mcpu=x86_64_v3']
NO_WINDOW = 0x08000000   # CREATE_NO_WINDOW


def say(*a):
    print(*a, flush=True)


def includes():
    # the generated code's headers: runtime/ in the repository, include/ in a release
    for d in (ROOT / 'include', ROOT / 'runtime'):
        if (d / 'cpu.h').exists():
            host = ROOT / 'include' if d.name == 'include' else ROOT / 'host'
            return d, host
    raise SystemExit('error the generated code headers (cpu.h) are missing')


def c_string(s):
    return '"' + str(s).replace('\\', '/').replace('"', '\\"') + '"'


def write_glue(gen, exefs, game, program, path):
    layout = json.loads((gen / 'layout.json').read_text())
    o = ['// The module glue for SplatoonRaiders_rt.dll, written by tools/setup_build.py. Do not edit.',
         '#include "dispatch.h"', '#include "layout.h"', '#include "rt_glue.h"', '',
         'volatile int g_fnprof = 0;   // the lifted code\'s function profiler switch (off in this build)', '',
         'typedef struct { const char *file, *sha256; } FileHash;',
         'static const RtLayoutEntry layout_[] = {']
    o += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    o += ['};']
    for m in layout:
        n = m['name']
        o += [f'extern const uint64_t {n}_fn_addrs[];', f'extern const GuestFn {n}_fn_ptrs[];',
              f'extern const uint64_t {n}_fn_count;']
    o += ['static int linked(RtModule *out, int max) {', '    int n = 0;']
    for m in layout:
        n, u = m['name'], m['name'].upper()
        o.append(f'    if (n < max) out[n++] = (RtModule){{"{n}", MOD_{u}_BASE, MOD_{u}_SIZE, {n}_fn_addrs, {n}_fn_ptrs, '
                 f'{n}_fn_count}};')
    o += ['    return n;', '}', '', 'static const FileHash hashes[] = {']
    files = sorted(f.name for f in exefs.iterdir() if f.is_file())
    for f in files:
        o.append(f'    {{{c_string(f)}, {c_string(hashlib.sha256((exefs / f).read_bytes()).hexdigest())}}},')
    o += ['};', '',
          'int main(int argc, char **argv) {',
          '    static const RtGlue glue = {RT_GLUE_VERSION, layout_, sizeof layout_ / sizeof *layout_, linked, hashes,',
          f'                                {len(files)}, {c_string((game / "exefs").as_posix())}, '
          f'{c_string((game / "romfs").as_posix())},',
          f'                                {c_string((program / "profile").as_posix())}, 0, "setup"}};',
          '    return rt_main(argc, argv, &glue);', '}', '']
    text = '\n'.join(o)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, required=True, help='the folder with exefs/ and romfs/ (update 65536)')
    ap.add_argument('--program', type=Path, required=True, help='where Launcher.exe and SplatoonRaiders_rt.dll are')
    ap.add_argument('--work', type=Path, help='build files (default: <program>/setup_work)')
    ap.add_argument('--zig', type=Path, help='zig.exe (default: <program>/tools/zig/zig.exe)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    game, program = args.game.resolve(), args.program.resolve()
    work = (args.work or program / 'setup_work').resolve()
    zig = (args.zig or program / 'tools' / 'zig' / 'zig.exe').resolve()
    exefs = game / 'exefs'
    if not (exefs / 'main').exists():
        say(f'error no exefs/main in {game}')
        return 2
    if not zig.exists():
        say(f'error {zig} is missing')
        return 2
    implib = program / 'SplatoonRaiders_rt.lib'
    if not implib.exists():
        say(f'error {implib} is missing')
        return 2
    inc, host_inc = includes()
    gen, obj = work / 'gen', work / 'obj'
    obj.mkdir(parents=True, exist_ok=True)

    # 1. lift (again only when the game files changed)
    stamp = work / 'gen.stamp'
    main_sha = hashlib.sha256((exefs / 'main').read_bytes()).hexdigest()
    if not (stamp.exists() and stamp.read_text() == main_sha and (gen / 'layout.json').exists()):
        say('step lift')
        r = subprocess.call([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(exefs), '--out', str(gen)],
                            stdout=sys.stderr)
        if r:
            say(f'error the code lifter failed ({r})')
            return 3
        stamp.write_text(main_sha)

    # 2. glue
    say('step glue')
    glue = work / 'glue.c'
    write_glue(gen, exefs, game, program, glue)

    # 3. compile (what is compiled already is kept)
    srcs = sorted(gen.glob('*/*.c'))
    jobs = []
    for s in srcs:
        o = obj / f'{s.stem}.o'
        if not o.exists() or o.stat().st_mtime < s.stat().st_mtime:
            jobs.append((s, o, ['-I', str(inc), '-I', str(gen)]))
    go = obj / 'glue.o'
    jobs.append((glue, go, ['-I', str(inc), '-I', str(host_inc), '-I', str(gen)]))
    total, done = len(srcs) + 1, len(srcs) + 1 - len(jobs)
    say('step compile')
    say(f'build {done} {total}')

    def cc(job):
        s, o, inc_args = job
        tmp = o.with_suffix('.o.tmp')
        p = subprocess.run([str(zig), 'cc', '-target', 'x86_64-windows-gnu', *GEN_FLAGS, *inc_args, '-c', str(s), '-o', str(tmp)],
                           capture_output=True, text=True, creationflags=NO_WINDOW)
        if p.returncode:
            return s, p.stderr[-2000:]
        os.replace(tmp, o)
        return s, None

    failed = None
    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for s, err in ex.map(cc, jobs):
            if err:
                failed = failed or (s, err)
                sys.stderr.write(f'{s}: {err}\n')
            done += 1
            say(f'build {done} {total}')
    if failed:
        say(f'error compiling {failed[0].name} failed')
        return 4

    # 4. link
    say('step link')
    exe = program / 'SplatoonRaiders.exe'
    tmp = work / 'SplatoonRaiders.exe'
    rsp = work / 'link.rsp'
    objs = [obj / f'{s.stem}.o' for s in srcs] + [go]
    rsp.write_text('\n'.join(f'"{o.as_posix()}"' for o in objs))
    p = subprocess.run([str(zig), 'cc', '-target', 'x86_64-windows-gnu', '-o', str(tmp), f'@{rsp}', str(implib),
                        '-Wl,--stack,16777216'], capture_output=True, text=True, creationflags=NO_WINDOW)
    if p.returncode:
        sys.stderr.write(p.stderr)
        say('error linking failed: ' + p.stderr.strip().splitlines()[-1][:300] if p.stderr.strip() else 'error linking failed')
        return 5
    if exe.exists():   # never deleted: the previous one kept beside it
        old = program / 'SplatoonRaiders.old.exe'
        if old.exists():
            os.replace(old, work / 'SplatoonRaiders.older.exe')
        os.replace(exe, old)
    os.replace(tmp, exe)
    say('done')
    return 0


if __name__ == '__main__':
    sys.exit(main())
