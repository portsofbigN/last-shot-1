// The runtime as a DLL (SplatoonRaiders_rt.dll, built with NX2_RT_DLL): the module glue that a single executable
// gets from modules.c comes from the game executable instead (rt_glue.h), then sp_main.c's main runs.
#include <stdio.h>
#include <windows.h>

#include "dispatch.h"
#include "host.h"

#define rt_main rt_main_imported_name   // rt_glue.h declares it as an import; here it is the export
#include "rt_glue.h"
#undef rt_main

const RtLayoutEntry *rt_layout;
int rt_layout_count;
const RtFileHash *rt_exefs_sha256;
int rt_exefs_sha256_count;
const char *rt_default_exefs = "", *rt_default_romfs = "", *rt_default_profile = "";
int rt_game_version;
const char *rt_port_version = "";

static int (*linked)(RtModule *out, int max);
int rt_linked_modules(RtModule *out, int max) { return linked ? linked(out, max) : 0; }

int nx2_sp_main(int argc, char **argv);

__declspec(dllexport) int rt_main(int argc, char **argv, const RtGlue *g) {
    if (!g || g->version != RT_GLUE_VERSION) {
        MessageBoxW(NULL,
                    L"SplatoonRaiders.exe e SplatoonRaiders_rt.dll são de versões diferentes. Abra o Launcher e use "
                    L"\"Configurar\" para gerar o jogo de novo.\n\nSplatoonRaiders.exe and SplatoonRaiders_rt.dll do "
                    L"not match. Open the Launcher and run Set up again.",
                    L"Splatoon Raiders", MB_ICONERROR);
        return 1;
    }
    rt_layout = g->layout, rt_layout_count = g->layout_count;
    linked = g->linked_modules;
    rt_exefs_sha256 = g->exefs_sha256, rt_exefs_sha256_count = g->exefs_sha256_count;
    rt_default_exefs = g->default_exefs ? g->default_exefs : "";
    rt_default_romfs = g->default_romfs ? g->default_romfs : "";
    rt_default_profile = g->default_profile ? g->default_profile : "";
    rt_game_version = g->game_version;
    rt_port_version = g->port_version ? g->port_version : "";
    return nx2_sp_main(argc, argv);
}
