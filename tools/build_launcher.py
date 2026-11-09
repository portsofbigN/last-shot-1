"""Build Launcher.exe (the settings menu and shader preparation on their own: no game code) and its release zip.

Usage: build_launcher.py [--zip]   ->  build/launcher/Launcher.exe (and with --zip dist/github/SplatoonRaiders-Launcher-win64.zip)
"""
import os
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LLVM = Path(os.environ.get('LLVM_DIR', r'C:\Program Files\LLVM'))
VULKAN = Path(os.environ.get('VULKAN_SDK', r'C:/VulkanSDK/1.4.363.0'))
OUT = ROOT / 'build' / 'launcher' / 'Launcher.exe'
ZIP = ROOT / 'dist' / 'github' / 'SplatoonRaiders-Launcher-win64.zip'
README = """Splatoon Raiders (recomp) - Launcher for Windows x64

PT: Coloque o Launcher.exe na mesma pasta do SplatoonRaiders.exe que voce gerou a partir da SUA copia do jogo
(atualizacao 65536; veja o README do repositorio). Abra o Launcher.exe: escolha as configuracoes (tela, FSR/NIS,
FPS, controles, idioma, pasta do jogo) e clique em JOGAR!. Os shaders ja vistos sao preparados antes do jogo.
Este pacote nao contem nenhum codigo ou arquivo do jogo.

EN: Put Launcher.exe in the same folder as the SplatoonRaiders.exe you built from YOUR own copy of the game
(update 65536; see the repository README). Open Launcher.exe, choose the settings and click PLAY!. Shaders seen
before are prepared before the game starts. This package contains no game code or files.
"""


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    # version information and manifest: an exe without them is more often flagged by antivirus heuristics
    res = OUT.with_suffix('.res')
    subprocess.run([str(LLVM / 'bin' / 'llvm-rc.exe'), '/nologo', '/fo', str(res), str(ROOT / 'host' / 'launcher.rc')],
                   check=True, cwd=ROOT / 'host')
    cmd = [str(LLVM / 'bin' / 'clang.exe'), '-O2', '-march=x86-64-v2', '-D_CRT_SECURE_NO_WARNINGS',
           '-I', str(ROOT / 'host'), '-I', str(ROOT / 'host' / 'compat_sdl'), '-I', str(VULKAN / 'Include'),
           '-include', str(ROOT / 'host' / 'sp_compat.h'),
           str(ROOT / 'host' / 'launcher_main.c'), str(ROOT / 'host' / 'launcher_ui.c'), str(ROOT / 'host' / 'nx2_settings.c'), str(ROOT / 'host' / 'nx2_pcache.c'),
           '-o', str(OUT), str(res), str(VULKAN / 'Lib' / 'vulkan-1.lib'),
           '-luser32', '-lgdi32', '-lgdiplus', '-lbcrypt', '-lshcore', '-lshell32', '-lole32', '-ladvapi32', '-ldxgi', '-ldxguid', '-lwinhttp', '-Wl,/SUBSYSTEM:WINDOWS']
    subprocess.run(cmd, check=True)
    if '--zip' not in sys.argv:   # the launcher alone (no Set up): only on request; the release is tools/package_setup.py
        print(f'{OUT} ({OUT.stat().st_size // 1024} KiB)')
        return
    ZIP.parent.mkdir(parents=True, exist_ok=True)
    tmp = ZIP.with_suffix('.zip.new')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(OUT, 'Launcher.exe')
        z.writestr('LEIA-ME_README.txt', README)
        for f in ('COMO_JOGAR.md', 'HOW_TO_PLAY.md'):
            z.write(ROOT / f, f)
        lic = ROOT.parent / 'projetos' / 'eglantine' / 'LICENSE.txt'
        if lic.exists():
            z.write(lic, 'LICENSE.txt')
    os.replace(tmp, ZIP)
    print(f'{OUT} ({OUT.stat().st_size // 1024} KiB)\n{ZIP} ({ZIP.stat().st_size // 1024} KiB)')


if __name__ == '__main__':
    main()
