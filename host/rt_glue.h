// What the game executable hands the runtime DLL at start (release model of the Set up: SplatoonRaiders_rt.dll holds
// the runtime, built once and shared; SplatoonRaiders.exe holds the code lifted from the player's own game files,
// built on the player's PC by tools/setup_build.py). The executable's main calls rt_main(argc, argv, &glue).
#pragma once
#include <stdint.h>

#define RT_GLUE_VERSION 1

struct RtModule;
struct RtLayoutEntry;
typedef struct {
    int version;   // RT_GLUE_VERSION
    const struct RtLayoutEntry *layout;
    int layout_count;
    int (*linked_modules)(struct RtModule *out, int max);
    const void *exefs_sha256;   // RtFileHash[]
    int exefs_sha256_count;
    const char *default_exefs, *default_romfs, *default_profile;
    int game_version;
    const char *port_version;
} RtGlue;

#ifdef _WIN32
__declspec(dllimport)
#endif
int rt_main(int argc, char **argv, const RtGlue *glue);
