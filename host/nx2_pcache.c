// Shader preparation: a persistent Vulkan pipeline cache plus the "recipes" of every pipeline the game has used, so
// the launcher can compile them all before the game starts (like an emulator's shader cache loading screen).
//
// pipeline_cache.bin  - vkGetPipelineCacheData of the game's VkPipelineCache (the driver checks its header: another
//                       GPU or driver simply starts over). Loaded at start, saved when new pipelines appear.
// pipeline_recipes.bin - one record per distinct pipeline: SPIR-V of its stages, descriptor bindings, push constant
//                       range and the fixed-function state, enough to call vkCreate*Pipelines again.
//
// nx2_pcache_prebuild (the launcher, before the game) makes its own Vulkan device on the same GPU, compiles every
// recipe through the cache in worker threads and saves it: the game's own pipeline creation then finds them compiled.
// Both files sit next to the program and hold only shaders the player's own game produced. NX2_PCACHE=0: off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#include "host.h"

#define REC_MAGIC 0x50435250u   // "PRCP"
#define FORMAT 1u
#define HSET 65536

static CRITICAL_SECTION lock;
static volatile LONG init_state;
static VkDevice dev;
static VkPipelineCache cache;
static FILE *recipes;
static uint64_t *seen;   // hashes of the recorded recipes (open addressing)
static volatile LONG dirty, nrec;
static int off = -1;

static void path_of(char *out, size_t n, const char *name) {
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!k || k >= sizeof exe) exe[0] = 0;
    char *slash = strrchr(exe, 92);
    if (slash) slash[1] = 0;
    else exe[0] = 0;
    snprintf(out, n, "%s%s", exe, name);
}

static void lock_init(void) {
    if (InterlockedCompareExchange(&init_state, 1, 0) == 0) {
        InitializeCriticalSection(&lock);
        InterlockedExchange(&init_state, 2);
    }
    while (init_state != 2) Sleep(0);
}

static int is_off(void) {
    if (off < 0) off = getenv("NX2_PCACHE") && *getenv("NX2_PCACHE") == '0';
    return off;
}

static uint64_t hash64(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h ? h : 1;
}
static int seen_add(uint64_t h) {   // 1 if new
    if (!seen) seen = calloc(HSET, sizeof *seen);
    if (!seen) return 1;
    for (uint64_t i = 0; i < HSET; i++) {
        uint64_t *s = &seen[(h + i) & (HSET - 1)];
        if (*s == h) return 0;
        if (!*s) { *s = h; return 1; }
    }
    return 0;
}

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
    fclose(f);
    *size = d ? (size_t)n : 0;
    return d;
}

static VkPipelineCache cache_load(VkDevice d) {
    char path[MAX_PATH + 32];
    path_of(path, sizeof path, "pipeline_cache.bin");
    size_t n = 0;
    void *data = read_file(path, &n);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, n, data};
    VkPipelineCache c = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(d, &ci, NULL, &c) != VK_SUCCESS) {   // damaged: start over
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(d, &ci, NULL, &c) != VK_SUCCESS) c = VK_NULL_HANDLE;
    }
    free(data);
    return c;
}

static void cache_save(VkDevice d, VkPipelineCache c) {
    if (!d || !c) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(d, c, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (!data) return;
    if (vkGetPipelineCacheData(d, c, &n, data) == VK_SUCCESS) {
        char path[MAX_PATH + 32], tmp[MAX_PATH + 40];
        path_of(path, sizeof path, "pipeline_cache.bin");
        snprintf(tmp, sizeof tmp, "%s.new", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            size_t w = fwrite(data, 1, n, f);
            fclose(f);
            if (w == n) MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
        }
    }
    free(data);
}

// ---- the game's side
void nx2_pcache_open(VkDevice d) {
    lock_init();
    if (is_off()) return;
    dev = d;
    cache = cache_load(d);
    char path[MAX_PATH + 32];
    path_of(path, sizeof path, "pipeline_recipes.bin");
    size_t n = 0;
    uint8_t *all = read_file(path, &n);
    uint8_t *p = all, *end = all + n;
    while (p && p + 16 <= end) {   // the hashes of the recipes there (not recorded twice)
        uint32_t magic, len;
        uint64_t h;
        memcpy(&magic, p, 4), memcpy(&len, p + 4, 4), memcpy(&h, p + 8, 8);
        if (magic != REC_MAGIC || (size_t)(end - p) < 16 + (size_t)len) break;
        seen_add(h), nrec++;
        p += 16 + len;
    }
    free(all);
    recipes = fopen(path, "ab");
    rt_log("pcache: pipeline cache %s, %ld recipes", cache ? "open" : "unavailable", (long)nrec);
}

VkPipelineCache nx2_pcache(void) { return cache; }

static DWORD WINAPI save_thread(LPVOID a) {
    (void)a;
    cache_save(dev, cache);
    return 0;
}
// Saves the cache when pipelines were added (at most every 30 s; on a worker thread); final: now, on this thread.
void nx2_pcache_save(int final) {
    if (!cache || !dirty) return;
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (!final && now - last < 30000) return;
    last = now;
    InterlockedExchange(&dirty, 0);
    if (final) cache_save(dev, cache);
    else CloseHandle(CreateThread(NULL, 0, save_thread, NULL, 0, NULL));
}

// ---- recipes
typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void put(Buf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 4096;
        while (c < o->n + n) c *= 2;
        uint8_t *nb = realloc(o->b, c);
        if (!nb) return;
        o->b = nb, o->cap = c;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void put32(Buf *o, uint32_t v) { put(o, &v, 4); }

static void put_common(Buf *o, int n, const uint32_t *stage_flags, const uint32_t *const *spv, const size_t *words,
                       const VkDescriptorSetLayoutBinding *b, int nb, const VkPushConstantRange *pcr) {
    put32(o, (uint32_t)n);
    for (int s = 0; s < n; s++) {
        put32(o, stage_flags[s]), put32(o, (uint32_t)words[s]);
        put(o, spv[s], words[s] * 4);
    }
    put32(o, (uint32_t)nb);
    for (int i = 0; i < nb; i++) {
        uint32_t v[4] = {b[i].binding, (uint32_t)b[i].descriptorType, b[i].descriptorCount, b[i].stageFlags};
        put(o, v, sizeof v);
    }
    put(o, pcr, sizeof *pcr);
}

static void record(Buf *o) {
    if (!o->b) return;
    uint64_t h = hash64(o->b, o->n);
    EnterCriticalSection(&lock);
    if (recipes && seen_add(h)) {
        uint32_t hdr[2] = {REC_MAGIC, (uint32_t)o->n};
        fwrite(hdr, 4, 2, recipes), fwrite(&h, 8, 1, recipes), fwrite(o->b, 1, o->n, recipes);
        fflush(recipes);
        nrec++;
    }
    LeaveCriticalSection(&lock);
    InterlockedExchange(&dirty, 1);
    free(o->b);
}

// A graphics pipeline as created (nx2_vkdraw.c): its state structs (no pointers in them but pNext) are stored whole.
void nx2_pcache_record_gfx(const VkGraphicsPipelineCreateInfo *gp, const VkPipelineRenderingCreateInfo *ri,
                           const uint32_t *const *spv, const size_t *words, const VkDescriptorSetLayoutBinding *b,
                           int nb, const VkPushConstantRange *pcr) {
    if (is_off() || !recipes) return;
    Buf o = {0};
    put32(&o, 1);   // graphics
    uint32_t sf[8];
    for (uint32_t s = 0; s < gp->stageCount && s < 8; s++) sf[s] = (uint32_t)gp->pStages[s].stage;
    put_common(&o, (int)gp->stageCount, sf, spv, words, b, nb, pcr);
    const VkPipelineVertexInputStateCreateInfo *vi = gp->pVertexInputState;
    put32(&o, vi->vertexBindingDescriptionCount);
    put(&o, vi->pVertexBindingDescriptions, vi->vertexBindingDescriptionCount * sizeof *vi->pVertexBindingDescriptions);
    put32(&o, vi->vertexAttributeDescriptionCount);
    put(&o, vi->pVertexAttributeDescriptions,
        vi->vertexAttributeDescriptionCount * sizeof *vi->pVertexAttributeDescriptions);
    put(&o, gp->pInputAssemblyState, sizeof *gp->pInputAssemblyState);
    put(&o, gp->pRasterizationState, sizeof *gp->pRasterizationState);
    put(&o, gp->pMultisampleState, sizeof *gp->pMultisampleState);
    put(&o, gp->pDepthStencilState, sizeof *gp->pDepthStencilState);
    const VkPipelineColorBlendStateCreateInfo *cb = gp->pColorBlendState;
    put32(&o, cb->attachmentCount);
    put(&o, cb->pAttachments, cb->attachmentCount * sizeof *cb->pAttachments);
    const VkPipelineDynamicStateCreateInfo *dy = gp->pDynamicState;
    put32(&o, dy->dynamicStateCount);
    put(&o, dy->pDynamicStates, dy->dynamicStateCount * sizeof *dy->pDynamicStates);
    put32(&o, ri->colorAttachmentCount);
    put(&o, ri->pColorAttachmentFormats, ri->colorAttachmentCount * sizeof(VkFormat));
    put32(&o, (uint32_t)ri->depthAttachmentFormat), put32(&o, (uint32_t)ri->stencilAttachmentFormat);
    record(&o);
}

void nx2_pcache_record_compute(const uint32_t *spv, size_t words, const VkDescriptorSetLayoutBinding *b, int nb,
                               const VkPushConstantRange *pcr) {
    if (is_off() || !recipes) return;
    Buf o = {0};
    put32(&o, 2);   // compute
    uint32_t sf = VK_SHADER_STAGE_COMPUTE_BIT;
    put_common(&o, 1, &sf, &spv, &words, b, nb, pcr);
    record(&o);
}

// ---- the launcher's side: compile every recipe before the game
typedef struct { const uint8_t *p, *end; int bad; } Rd;
static const void *take(Rd *r, size_t n) {
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = 1; return NULL; }
    const void *q = r->p;
    r->p += n;
    return q;
}
static uint32_t take32(Rd *r) {
    const void *q = take(r, 4);
    uint32_t v = 0;
    if (q) memcpy(&v, q, 4);
    return v;
}

static VkDevice pdev;
static void build_one(const uint8_t *rec, size_t len) {
    Rd r = {rec, rec + len, 0};
    uint32_t kind = take32(&r), n = take32(&r);
    if (n == 0 || n > 5) return;
    VkShaderModule mods[5] = {0};
    VkPipelineShaderStageCreateInfo st[5];
    for (uint32_t s = 0; s < n; s++) {
        uint32_t flags = take32(&r), words = take32(&r);
        const void *code = take(&r, (size_t)words * 4);
        if (!code) goto out;
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)words * 4, code};
        if (vkCreateShaderModule(pdev, &mi, NULL, &mods[s]) != VK_SUCCESS) goto out;
        st[s] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                  (VkShaderStageFlagBits)flags, mods[s], "main", NULL};
    }
    uint32_t nb = take32(&r);
    if (nb > 256) goto out;
    VkDescriptorSetLayoutBinding b[256];
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t *v = take(&r, 16);
        if (!v) goto out;
        b[i] = (VkDescriptorSetLayoutBinding){v[0], (VkDescriptorType)v[1], v[2], v[3], NULL};
    }
    const VkPushConstantRange *pcr = take(&r, sizeof *pcr);
    if (!pcr) goto out;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, pcr};
    if (vkCreateDescriptorSetLayout(pdev, &dl, NULL, &dsl) != VK_SUCCESS) goto out;
    if (vkCreatePipelineLayout(pdev, &pli, NULL, &pl) != VK_SUCCESS) goto out2;
    VkPipeline p = VK_NULL_HANDLE;
    if (kind == 2) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, st[0], pl};
        vkCreateComputePipelines(pdev, cache, 1, &ci, NULL, &p);
    } else {
        uint32_t nvb = take32(&r);
        const VkVertexInputBindingDescription *vb = take(&r, nvb * sizeof *vb);
        uint32_t nva = take32(&r);
        const VkVertexInputAttributeDescription *va = take(&r, nva * sizeof *va);
        VkPipelineInputAssemblyStateCreateInfo ia;
        VkPipelineRasterizationStateCreateInfo rs;
        VkPipelineMultisampleStateCreateInfo ms;
        VkPipelineDepthStencilStateCreateInfo ds;
        const void *q;
        if ((q = take(&r, sizeof ia))) memcpy(&ia, q, sizeof ia);
        if ((q = take(&r, sizeof rs))) memcpy(&rs, q, sizeof rs);
        if ((q = take(&r, sizeof ms))) memcpy(&ms, q, sizeof ms);
        if ((q = take(&r, sizeof ds))) memcpy(&ds, q, sizeof ds);
        uint32_t ncb = take32(&r);
        const VkPipelineColorBlendAttachmentState *cba = take(&r, ncb * sizeof *cba);
        uint32_t ndy = take32(&r);
        const VkDynamicState *dyn = take(&r, ndy * sizeof *dyn);
        uint32_t ncol = take32(&r);
        const VkFormat *cf = take(&r, ncol * sizeof *cf);
        uint32_t df = take32(&r), sf = take32(&r);
        if (!r.bad && ncb <= 8 && ncol <= 8) {
            ia.pNext = rs.pNext = ms.pNext = ds.pNext = NULL;
            ms.pSampleMask = NULL;
            VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                                        nvb, vb, nva, va};
            VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1,
                                                     NULL, 1, NULL};
            VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cbs.attachmentCount = ncb, cbs.pAttachments = cba;
            VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, ndy, dyn};
            VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, ncol, cf,
                                                (VkFormat)df, (VkFormat)sf};
            VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
            gp.stageCount = n, gp.pStages = st, gp.pVertexInputState = &vis, gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps, gp.pRasterizationState = &rs, gp.pMultisampleState = &ms;
            gp.pDepthStencilState = &ds, gp.pColorBlendState = &cbs, gp.pDynamicState = &dys, gp.layout = pl;
            vkCreateGraphicsPipelines(pdev, cache, 1, &gp, NULL, &p);
        }
    }
    if (p) vkDestroyPipeline(pdev, p, NULL);
    vkDestroyPipelineLayout(pdev, pl, NULL);
out2:
    vkDestroyDescriptorSetLayout(pdev, dsl, NULL);
out:
    for (uint32_t s = 0; s < n; s++)
        if (mods[s]) vkDestroyShaderModule(pdev, mods[s], NULL);
}

typedef struct { const uint8_t **rec; const size_t *len; int n; volatile LONG next, done; } Work;
static DWORD WINAPI worker(LPVOID a) {
    Work *w = a;
    for (;;) {
        LONG i = InterlockedIncrement(&w->next) - 1;
        if (i >= w->n) return 0;
        build_one(w->rec[i], w->len[i]);
        InterlockedIncrement(&w->done);
    }
}

// Compiles every recipe on a Vulkan device of its own (the GPU the game picks), reporting progress; 0 when there is
// nothing to do. Called by the launcher after PLAY, before the game starts.
int nx2_pcache_prebuild(void (*progress)(int done, int total, void *user), void *user) {
    lock_init();
    if (is_off()) return 0;
    char path[MAX_PATH + 32];
    path_of(path, sizeof path, "pipeline_recipes.bin");
    size_t n = 0;
    uint8_t *all = read_file(path, &n);
    if (!all) return 0;
    int cap = 1024, count = 0;
    const uint8_t **rec = malloc(cap * sizeof *rec);
    size_t *len = malloc(cap * sizeof *len);
    for (uint8_t *p = all; rec && len && p + 16 <= all + n;) {
        uint32_t magic, l;
        memcpy(&magic, p, 4), memcpy(&l, p + 4, 4);
        if (magic != REC_MAGIC || (size_t)(all + n - p) < 16 + (size_t)l) break;
        if (count == cap) {
            cap *= 2;
            rec = realloc(rec, cap * sizeof *rec), len = realloc(len, cap * sizeof *len);
            if (!rec || !len) break;
        }
        rec[count] = p + 16, len[count] = l, count++;
        p += 16 + l;
    }
    int built = 0;
    VkInstance inst = VK_NULL_HANDLE;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Splatoon Raiders recomp", 1, "nx2", 1,
                             VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (count && rec && len && vkCreateInstance(&ic, NULL, &inst) == VK_SUCCESS) {
        uint32_t np = 8;
        VkPhysicalDevice pd[8], phys = VK_NULL_HANDLE;
        if (vkEnumeratePhysicalDevices(inst, &np, pd) >= 0 && np) {
            phys = pd[0];
            for (uint32_t i = 0; i < np; i++) {   // as nx2_vk.c picks it
                VkPhysicalDeviceProperties pr;
                vkGetPhysicalDeviceProperties(pd[i], &pr);
                if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd[i]; break; }
            }
        }
        uint32_t nq = 16, qfam = 0;
        VkQueueFamilyProperties qf[16];
        if (phys) vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
        for (uint32_t i = 0; phys && i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
        // the same features the game enables (they can change the compiled code)
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f2.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
        if (phys) {
            vkGetPhysicalDeviceFeatures2(phys, &f2);
            f2.features.robustBufferAccess = VK_FALSE;
            f13.robustImageAccess = VK_FALSE;
            VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qc};
            if (vkCreateDevice(phys, &dc, NULL, &pdev) != VK_SUCCESS) pdev = VK_NULL_HANDLE;
        }
        if (pdev) {
            cache = cache_load(pdev);
            Work w = {rec, len, count, 0, 0};
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            int nt = (int)si.dwNumberOfProcessors - 1;
            if (nt < 1) nt = 1;
            if (nt > 12) nt = 12;
            HANDLE th[12];
            for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, worker, &w, 0, NULL);
            for (;;) {
                DWORD r = WaitForMultipleObjects((DWORD)nt, th, TRUE, 50);
                if (progress) progress((int)w.done, count, user);
                if (r != WAIT_TIMEOUT) break;
            }
            for (int i = 0; i < nt; i++) CloseHandle(th[i]);
            built = (int)w.done;
            cache_save(pdev, cache);
            vkDestroyPipelineCache(pdev, cache, NULL);
            cache = VK_NULL_HANDLE;
            vkDestroyDevice(pdev, NULL);
            pdev = VK_NULL_HANDLE;
        }
        vkDestroyInstance(inst, NULL);
    }
    free(rec), free(len), free(all);
    return built;
}
