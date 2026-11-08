"""Assemble the release with the Set up (as the Pokopia PC launcher ships): dist/github/SplatoonRaiders-win64/ and its zip.

The folder holds no game code or data. The player opens Launcher.exe, chooses their own game files and presses Set up:
SplatoonRaiders.exe is then built on their PC (tools/setup_build.py) from their exefs and linked against
SplatoonRaiders_rt.dll, the runtime (tools/build_rt_dll.py). What is in it:

  Launcher.exe, SplatoonRaiders_rt.dll (+ .lib, its import library), the guides, licences
  setup/recomp, setup/tools   the code lifter (recomp/gen.py ...) and setup_build.py
  setup/include               the headers the lifted code compiles with
  setup/python                a Python 3 with numpy, for the lifter (copied from this PC's Python)
  setup/zig                   Zig (clang + lld + mingw-w64), the compiler (copied from --zig)

Never removes anything: an existing folder is moved to work/shots_old/release_<time>/.

Usage: package_setup.py [--python DIR] [--zig DIR] [--no-zip]
"""
import argparse
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'dist' / 'github' / 'SplatoonRaiders-win64'
ZIP = ROOT / 'dist' / 'github' / 'SplatoonRaiders-win64.zip'
PY_SKIP = {'test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', '__pycache__', 'site-packages', 'venv'}
README = """Splatoon Raiders (recomp) - PC port for Windows x64

PT
1. Abra o Launcher.exe e escolha os arquivos do jogo (a SUA copia, atualizacao 65536: a pasta com exefs e romfs, ou a
   pasta acima dela com updates/65536). O launcher confere a versao.
2. Clique em Configurar: o launcher converte o codigo do jogo e gera o SplatoonRaiders.exe neste PC (uma vez; leva de
   30 a 70 minutos e precisa de uns 5 GB livres; se for interrompido, continua de onde parou). Depois ele traduz os
   shaders do jogo (cerca de 1 minuto).
3. Clique em Jogar.
Este pacote nao contem nenhum codigo ou arquivo do jogo. Se o Windows Defender acusar o Launcher.exe, e um falso
positivo de programas sem assinatura: veja COMO_JOGAR.md.

EN
1. Open Launcher.exe and choose the game files (YOUR own copy, update 65536: the folder with exefs and romfs, or the
   folder above it with updates/65536). The launcher checks the version.
2. Click Set up: the launcher converts the game's code and builds SplatoonRaiders.exe on this PC (once; 30 to 70
   minutes and about 5 GB free; if stopped, it continues where it was). Then it translates the game's shaders (about
   a minute).
3. Click Play.
This package contains no game code or files.
"""


def copy_tree(src, dst, skip=frozenset()):
    for p in src.iterdir():
        if p.name in skip:
            continue
        if p.is_dir():
            copy_tree(p, dst / p.name, skip)
        else:
            (dst).mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst / p.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--python', type=Path, default=Path(sys.executable).parent)
    ap.add_argument('--zig', type=Path, default=Path(r'C:/Program Files (x86)/recomps/PokopiaRecomp-0.3.0/tools/build/zig'))
    ap.add_argument('--no-zip', action='store_true')
    args = ap.parse_args()
    for need in (ROOT / 'build' / 'launcher' / 'Launcher.exe', ROOT / 'build' / 'rtdll' / 'SplatoonRaiders_rt.dll',
                 args.zig / 'zig.exe', args.python / 'python.exe', args.python / 'Lib' / 'site-packages' / 'numpy'):
        if not need.exists():
            raise SystemExit(f'missing {need} (tools/build_launcher.py, tools/build_rt_dll.py, --zig, --python)')
    if OUT.exists():
        aside = ROOT / 'work' / 'shots_old' / time.strftime('release_%Y%m%d_%H%M%S')
        aside.parent.mkdir(parents=True, exist_ok=True)
        os.rename(OUT, aside)
        print(f'previous release folder moved to {aside}')
    OUT.mkdir(parents=True)
    for src, name in [(ROOT / 'build' / 'launcher' / 'Launcher.exe', 'Launcher.exe'),
                      (ROOT / 'build' / 'rtdll' / 'SplatoonRaiders_rt.dll', 'SplatoonRaiders_rt.dll'),
                      (ROOT / 'build' / 'rtdll' / 'SplatoonRaiders_rt.lib', 'SplatoonRaiders_rt.lib'),
                      (ROOT / 'COMO_JOGAR.md', 'COMO_JOGAR.md'), (ROOT / 'HOW_TO_PLAY.md', 'HOW_TO_PLAY.md'),
                      (ROOT / 'THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md')]:
        shutil.copy2(src, OUT / name)
    (OUT / 'LEIA-ME_README.txt').write_text(README, encoding='utf-8')
    lic = ROOT.parent / 'projetos' / 'eglantine' / 'LICENSE.txt'
    if lic.exists():
        shutil.copy2(lic, OUT / 'LICENSE.txt')
    st = OUT / 'setup'
    # the lifter and the Set up script
    (st / 'recomp').mkdir(parents=True)
    for p in (ROOT / 'recomp').glob('*'):
        if p.suffix in ('.py', '.txt') or p.name.startswith('LICENSE'):
            shutil.copy2(p, st / 'recomp' / p.name)
    (st / 'tools').mkdir()
    for n in ('setup_build.py', 'check_content.py', 'hostmem.py'):
        shutil.copy2(ROOT / 'tools' / n, st / 'tools' / n)
    (st / 'include').mkdir()
    for p in (ROOT / 'runtime' / 'cpu.h', ROOT / 'runtime' / 'simd.h', ROOT / 'runtime' / 'simd_fp.h',
              ROOT / 'host' / 'dispatch.h', ROOT / 'host' / 'rt_glue.h'):
        shutil.copy2(p, st / 'include' / p.name)
    # Python: the interpreter, its standard library and numpy
    py = st / 'python'
    py.mkdir()
    for n in ('python.exe', 'python3.dll', f'python{sys.version_info.major}{sys.version_info.minor}.dll', 'vcruntime140.dll',
              'vcruntime140_1.dll', 'LICENSE.txt'):
        if (args.python / n).exists():
            shutil.copy2(args.python / n, py / n)
    copy_tree(args.python / 'DLLs', py / 'DLLs')
    copy_tree(args.python / 'Lib', py / 'Lib', PY_SKIP)
    sp = args.python / 'Lib' / 'site-packages'
    for p in sp.iterdir():
        if p.name == 'numpy' or p.name == 'numpy.libs' or (p.name.startswith('numpy-') and p.name.endswith('.dist-info')):
            copy_tree(p, py / 'Lib' / 'site-packages' / p.name, {'__pycache__', 'tests'})
    # Zig
    copy_tree(args.zig, st / 'zig')
    # nothing from the game: no generated code, no exefs/romfs, no images
    bad = [p for p in OUT.rglob('*') if p.is_file() and (p.suffix in ('.nso', '.nca', '.nsp', '.xci', '.img') or
                                                          p.name in ('main', 'sdk', 'subsdk0', 'main.npdm'))]
    for p in OUT.rglob('*.c'):
        if p.is_file() and b'GENERATED_FROM_GAME_CODE' in p.read_bytes()[:400]:
            bad.append(p)
    if bad:
        raise SystemExit('refused, game files in the release: ' + ', '.join(map(str, bad[:10])))
    total = sum(p.stat().st_size for p in OUT.rglob('*') if p.is_file())
    print(f'{OUT}: {total / 1048576:.0f} MiB')
    if args.no_zip:
        return
    tmp = ZIP.with_suffix('.zip.new')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(OUT.rglob('*')):
            if p.is_file():
                z.write(p, Path('SplatoonRaiders-win64') / p.relative_to(OUT))
    os.replace(tmp, ZIP)
    print(f'{ZIP}: {ZIP.stat().st_size / 1048576:.0f} MiB')


if __name__ == '__main__':
    main()
