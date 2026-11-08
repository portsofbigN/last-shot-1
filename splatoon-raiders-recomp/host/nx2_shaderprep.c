// Shader preparation from the game files, before the first game: every program of the romfs shader archives
// (romfs/Shader/*.bfsha.zs) translated into shader_cache.bin (nx2_xcache.c), so the game does not stop to translate a
// shader the first time it draws with it. Run as SplatoonRaiders.exe with NX2_PREPARE_SHADERS=1 (Launcher.exe does,
// once the game files are chosen); progress lines "prep <done> <total>" go to stdout.
//
// An archive is a BFSHA holding a BNSH: its programs are {stage count, pointer to an array of 0x40-byte stage records},
// a record {0, control, code, sizes...} with pointers relative to the BNSH. The control section starts with NVN's
// magic 0x98761234 and gives the embedded constants' offset (0x40) and end (0x44); the code starts with its 0x80-byte
// header. The game hands the translator these same bytes, so the keys match; what the game only knows when it draws
// is answered the way it nearly always is: textures 2D and not integer (5370 of 5372 answers in a played cache),
// vertex inputs and colour outputs float. A program drawn otherwise is translated again then (the cache checks).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#include "nx2_xlate.h"

#ifndef NX2_XLATE_ID
#define NX2_XLATE_ID 0ull
#endif

void rt_log(const char *fmt, ...);
int nx2_xcache_translate(int n, const void *const *code, const size_t *size, const size_t *len, int compute,
                         const Nx2XOptions *opt, Nx2XStage *out);
void nx2_xcache_stats(uint64_t *h, uint64_t *m, uint64_t *rf, uint64_t *ld);
size_t nx2_zstd_size(const void *src, size_t n);
size_t nx2_zstd_decompress(void *dst, size_t cap, const void *src, size_t n);   // nx2_zstd.c

#define CTRL_MAGIC 0x98761234u
typedef struct { const uint8_t *data[6]; uint32_t size[6]; } Consts;   // per Shader::Stage

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static uint32_t q_cbuf(void *user, int stage, uint32_t index, uint32_t offset) {
    const Consts *c = user;
    if (index == 2) return 0x10000u + offset;   // a texture handle: any value (the cache maps them)
    if (index == 1 && stage >= 0 && stage < 6 && offset + 4 <= c->size[stage]) return rd32(c->data[stage] + offset);
    return 0;
}
static int q_type(void *user, int stage, uint32_t handle) { (void)user, (void)stage, (void)handle; return NX2X_T_2D; }
static int q_int(void *user, int stage, uint32_t handle) { (void)user, (void)stage, (void)handle; return 0; }

// the device's float controls, as the game's device reports them (nx2_vkdraw.c: the same GPU as nx2_vk.c picks)
static int float_controls(Nx2XOptions *o) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "shaderprep", 1, "nx2", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    if (vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pd[16], phys = VK_NULL_HANDLE;
    if (n > 16) n = 16;
    if (n && vkEnumeratePhysicalDevices(inst, &n, pd) >= 0) {
        phys = pd[0];
        for (uint32_t i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd[i], &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd[i]; break; }
        }
    }
    int ok = 0;
    if (phys) {
        VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        o->float_controls_set = 1;
        o->denorm_flush16 = p12.shaderDenormFlushToZeroFloat16;
        o->denorm_flush32 = p12.shaderDenormFlushToZeroFloat32;
        o->denorm_preserve16 = p12.shaderDenormPreserveFloat16;
        o->denorm_preserve32 = p12.shaderDenormPreserveFloat32;
        o->szinf_nan16 = p12.shaderSignedZeroInfNanPreserveFloat16;
        o->szinf_nan32 = p12.shaderSignedZeroInfNanPreserveFloat32;
        o->szinf_nan64 = p12.shaderSignedZeroInfNanPreserveFloat64;
        ok = 1;
    }
    vkDestroyInstance(inst, NULL);
    return ok;
}

// A program found in the BNSH: its stages' code and control
typedef struct { const uint8_t *code[5], *ctrl[5], *bn; size_t bsize; int n; } Prog;

static int stage_record(const uint8_t *bn, size_t bsize, uint64_t rec, const uint8_t **code, const uint8_t **ctrl) {
    if (rec + 0x18 > bsize) return 0;
    uint64_t c = rd64(bn + rec + 8), d = rd64(bn + rec + 16);
    if (!c || !d || c + 0x48 > bsize || d + 0x90 > bsize || rd32(bn + c) != CTRL_MAGIC) return 0;
    uint32_t w = rd32(bn + d);
    if ((w & 0x1f) != 1 && (w & 0x1f) != 2) return 0;
    *code = bn + d, *ctrl = bn + c;
    return 1;
}

static Prog *find_programs(const uint8_t *bn, size_t bsize, int *count) {
    int cap = 1024, n = 0;
    Prog *v = malloc(cap * sizeof *v);
    if (!v) return NULL;
    for (size_t o = 0; o + 16 <= bsize; o += 8) {
        uint64_t cnt = rd64(bn + o), ptr = rd64(bn + o + 8);
        if (cnt < 1 || cnt > 5 || !ptr || ptr + cnt * 0x40 > bsize) continue;
        Prog p = {.n = (int)cnt, .bn = bn, .bsize = bsize};
        int ok = 1;
        for (uint64_t i = 0; i < cnt && ok; i++) ok = stage_record(bn, bsize, ptr + i * 0x40, &p.code[i], &p.ctrl[i]);
        if (!ok) continue;
        int dup = 0;   // the same stages as one found already
        for (int k = n - 1; k >= 0 && k >= n - 64 && !dup; k--)
            if (v[k].n == p.n && !memcmp(v[k].code, p.code, sizeof p.code)) dup = 1;
        if (dup) continue;
        if (n == cap) {
            Prog *nv = realloc(v, (size_t)cap * 2 * sizeof *v);
            if (!nv) break;
            v = nv, cap *= 2;
        }
        v[n++] = p;
    }
    *count = n;
    return v;
}

static uint8_t *read_zs(const wchar_t *path, size_t *out_n) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    uint8_t *z = malloc((size_t)sz.QuadPart);
    DWORD got = 0;
    int ok = z && ReadFile(f, z, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart;
    CloseHandle(f);
    if (!ok) { free(z); return NULL; }
    size_t n = nx2_zstd_size(z, got);
    if (!n || n > (1ull << 31)) { free(z); return NULL; }
    uint8_t *d = malloc(n);
    size_t r = d ? nx2_zstd_decompress(d, n, z, got) : (size_t)-1;
    free(z);
    if (!d || r == (size_t)-1) { free(d); return NULL; }
    *out_n = r;
    return d;
}

// The shader archives: every .zs under these romfs folders that holds a BNSH (uber shaders, effects, layouts)
static const wchar_t *const dirs[] = {L"Shader", L"Effect", L"Layout", L"UI", L"Lib"};
typedef struct { wchar_t **path; int n, cap; } Files;
static void list_files(Files *f, const wchar_t *dir) {
    wchar_t pat[MAX_PATH * 2];
    swprintf(pat, MAX_PATH * 2, L"%ls/*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t full[MAX_PATH * 2];
        swprintf(full, MAX_PATH * 2, L"%ls/%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { list_files(f, full); continue; }
        size_t l = wcslen(fd.cFileName);
        if (l < 3 || wcscmp(fd.cFileName + l - 3, L".zs")) continue;
        if (f->n == f->cap) {
            int nc = f->cap ? f->cap * 2 : 256;
            wchar_t **np = realloc(f->path, (size_t)nc * sizeof *np);
            if (!np) break;
            f->path = np, f->cap = nc;
        }
        f->path[f->n++] = _wcsdup(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The programs of every BNSH in a file's bytes
static Prog *file_programs(const uint8_t *d, size_t n, int *count) {
    Prog *all = NULL;
    int na = 0;
    size_t starts[64];
    int ns = 0;
    for (size_t i = 0; i + 4 <= n && ns < 64; i += 4)
        if (!memcmp(d + i, "BNSH", 4)) starts[ns++] = i;
    for (int k = 0; k < ns; k++) {
        size_t end = k + 1 < ns ? starts[k + 1] : n;
        int c = 0;
        Prog *p = find_programs(d + starts[k], end - starts[k], &c);
        if (p && c) {
            Prog *m = realloc(all, (size_t)(na + c) * sizeof *m);
            if (m) memcpy(m + na, p, (size_t)c * sizeof *p), all = m, na += c;
        }
        free(p);
    }
    *count = na;
    return all;
}

static int translate(const Prog *p, Nx2XOptions *opt) {   // 0 when it failed
    const void *code[5];
    size_t size[5], len[5];
    Consts cs = {0};
    int n = 0;
    for (uint32_t want = 1; want <= 5; want++)   // pipeline order, vertex first (program_stages)
        for (int i = 0; i < p->n; i++) {
            uint32_t w = rd32(p->code[i]);
            if (((w >> 10) & 0xf) != want) continue;
            const uint8_t *ctl = p->ctrl[i];
            size_t avail = (size_t)(p->bn + p->bsize - p->code[i]);
            uint32_t co = rd32(ctl + 0x40), ce = rd32(ctl + 0x44);
            int has = co >= 0x90 && ce >= co && ce <= avail;
            code[n] = p->code[i];
            size[n] = avail;
            len[n] = has ? co : (avail < (1u << 20) ? avail : (1u << 20));
            if (has) cs.data[want - 1] = p->code[i] + co, cs.size[want - 1] = ce - co;
            n++;
        }
    if (!n) return 1;
    Nx2XStage st[5];
    memset(st, 0, sizeof st);
    opt->user = &cs;
    for (int i = 0; i < 32; i++) opt->attrib_types[i] = 0;   // float
    for (int i = 0; i < 8; i++) opt->color_types[i] = 0;
    if (nx2_xcache_translate(n, code, size, len, 0, opt, st)) return 0;
    // the vertex layouts the game also draws these with (seen in play): input 6 unsigned (skinning indices), input 12
    // absent; each a variant under the same key, the cache picks the one that fits
    uint32_t used = st[0].stage == 0 ? st[0].loads_generic : 0;
    nx2_xlate_free(st, n);
    for (int v = 1; v < 4; v++) {
        if (((v & 1) && !(used >> 6 & 1)) || ((v & 2) && !(used >> 12 & 1))) continue;
        opt->attrib_types[6] = v & 1 ? 2 : 0;
        opt->attrib_types[12] = v & 2 ? 5 : 0;
        memset(st, 0, sizeof st);
        if (!nx2_xcache_translate(n, code, size, len, 0, opt, st)) nx2_xlate_free(st, n);
    }
    return 1;
}

int nx2_shaderprep_main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *gd = getenv("NX2_GAME_DIR");
    if (!gd || !*gd) {
        printf("error no game folder\n");
        return 2;
    }
    wchar_t dir[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, gd, -1, dir, MAX_PATH);
    Nx2XOptions opt = {0};
    if (!float_controls(&opt)) {
        printf("error no Vulkan device\n");
        return 3;
    }
    opt.cbuf_value = q_cbuf, opt.texture_type = q_type, opt.texture_integer = q_int;
    opt.convert_depth = 1;   // as nx2_vkdraw.c's build_pipeline
    opt.topology = 3;        // triangles (only geometry stages read it)
    DWORD t0 = GetTickCount();
    // first the files and how many programs each holds, so the progress has a total
    Files fl = {0};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        wchar_t d[MAX_PATH * 2];
        swprintf(d, MAX_PATH * 2, L"%ls/romfs/%ls", dir, dirs[i]);
        list_files(&fl, d);
    }
    int *counts = calloc((size_t)fl.n + 1, sizeof *counts), total = 0;
    printf("scan 0 %d\n", fl.n);
    for (int i = 0; i < fl.n; i++) {
        size_t n = 0;
        uint8_t *d = read_zs(fl.path[i], &n);
        if (d && n >= 4) {
            Prog *p = file_programs(d, n, &counts[i]);
            free(p);
            total += counts[i];
        }
        free(d);
        if (!(i % 32)) printf("scan %d %d\n", i, fl.n);
    }
    if (!total) {
        printf("error no shaders in %s/romfs\n", gd);
        return 4;
    }
    rt_log("shaderprep: %d programs in %d files (%.1f s to find)", total, fl.n, (GetTickCount() - t0) / 1000.0);
    printf("prep 0 %d\n", total);
    uint64_t h0, m0, r0, l0, h1, m1, r1, l1;
    nx2_xcache_stats(&h0, &m0, &r0, &l0);
    int done = 0, failed = 0;
    for (int i = 0; i < fl.n; i++) {
        if (!counts[i]) continue;
        size_t n = 0;
        uint8_t *d = read_zs(fl.path[i], &n);
        int c = 0;
        Prog *p = d ? file_programs(d, n, &c) : NULL;
        for (int k = 0; k < c; k++) {
            if (!translate(&p[k], &opt)) failed++;
            done++;
            if (!(done % 16)) printf("prep %d %d\n", done, total);
        }
        free(p), free(d);
    }
    printf("prep %d %d\n", total, total);
    nx2_xcache_stats(&h1, &m1, &r1, &l1);
    rt_log("shaderprep: %d programs in %.1f s: %llu translated, %llu were in the cache, %d failed", done,
           (GetTickCount() - t0) / 1000.0, (unsigned long long)(m1 - m0), (unsigned long long)(h1 - h0), failed);
    printf("done %d %llu %llu %d\n", done, (unsigned long long)(m1 - m0), (unsigned long long)(h1 - h0), failed);
    {   // shader_cache.prepared next to the program: Launcher.exe's sign that this was done for this translator
        // (nx2_xcache.c sets it aside with the cache when the translator changes)
        char exe[MAX_PATH], path[MAX_PATH + 32];
        DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *sl = k ? strrchr(exe, 92) : NULL;
        if (sl) {
            sl[1] = 0;
            snprintf(path, sizeof path, "%sshader_cache.prepared", exe);
            FILE *m = fopen(path, "w");
            if (m) fprintf(m, "translator %016llx%cprograms %d%cfailed %d%c", (unsigned long long)NX2_XLATE_ID, 10, done, 10, failed, 10), fclose(m);
        }
    }
    for (int i = 0; i < fl.n; i++) free(fl.path[i]);
    free(fl.path), free(counts);
    return 0;
}
