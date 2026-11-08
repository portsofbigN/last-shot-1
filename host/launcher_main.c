// Launcher.exe: the launcher window (launcher_ui.c) and the shader preparation (nx2_pcache.c), with no game code, so it
// can be shared. Play starts SplatoonRaiders.exe from the same folder with NX2_LAUNCHED=1 (the game then skips its own
// menu); the settings travel in splatoon.ini.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

// the few host functions nx2_settings.c / nx2_pcache.c use
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc(10, stderr);
}

// no built-in game folder: the Game files page picks it (the game's own build has the developer's default)
const char rt_default_exefs[] = "", rt_default_romfs[] = "", rt_default_profile[] = "";

int launcher_ui_run(void);

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)inst, (void)prev, (void)cmd, (void)show;
    return launcher_ui_run();
}
