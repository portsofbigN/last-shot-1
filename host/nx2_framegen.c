// Frame interpolation: frames made between the game's from its last two, shown evenly in between by a display thread,
// so the game's 60 fps are shown at 120 (NX2_FRAMEGEN=2), 180 (3) or 240 (4). Adapted from Hookshot's
// gpu/vk/vk_framegen.cpp (github.com/NotRexed/Hookshot, GPL-3.0-or-later, NotRexed and contributors):
//   1. each game frame is copied (the game draws over it next frame) and reduced to quarter-size luma;
//   2. block matching: a vector per 16x16 pixels within +-32 pixels, then a 3x3 median of them;
//   3. each in-between frame at time t blends the last frame moved along t and the new one back along 1 - t, and keeps
//      what does not move (the HUD, walls) as it is.
// The display thread shows the made frames and then the game's, evenly over the game's recent frame time: about one
// frame behind the game. Loading hitches (frames over 100 ms apart) are not interpolated. The frames are RGB10A2, so
// the HDR10 frame goes through as the SDR one does. Here descriptor sets come from a pool per set of made frames
// (the port's device has no push descriptors), each reset once that set's recording has finished.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "fg_copy_spv.h"
#include "fg_flow_spv.h"
#include "fg_luma_spv.h"
#include "fg_median_spv.h"
#include "fg_synth_spv.h"
#include "host.h"
#include "nx2_vk.h"

#define MAX_MULT 4
#define NSETS 3   // one shown, one waiting its turn, one being made: the display never holds the game back

typedef struct { VkImage image; VkDeviceMemory mem; VkImageView view; } Img;
typedef struct { VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline pipe; } Pipe;
typedef struct {
    Img frame[MAX_MULT];
    int made;   // frame[0..made) are made frames, frame[made] the game's
    int busy;   // queued or being shown
    VkCommandBuffer cmd;
    VkFence fence;
    VkDescriptorPool pool;
    uint64_t game_frame;
} Set;
typedef struct { int set; int64_t at; } Job;

static struct {
    int init, failed, mult;
    VkDevice dev;
    VkQueue queue;
    uint32_t w, h;
    Pipe copy, luma, flow, median, synth;
    VkSampler nearest, linear;
    VkCommandPool cpool;
    Img frames[2], lumas[2], flows[2];
    Set sets[NSETS];
    int cur, next_set, have_prev;
    int64_t last_at;
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE cv;
    Job jobs[8];
    int njobs;
    int thread_started;
} F;

static volatile LONG shown_fps;   // frames shown in the last second (display thread)
int nx2_fg_mult(void);
int nx2_fg_shown_fps(void) { return nx2_fg_mult() > 1 && !F.failed ? (int)shown_fps : 0; }

int nx2_fg_mult(void) {   // NX2_FRAMEGEN: 1 (or unset) off, 2..4 the multiplier
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("NX2_FRAMEGEN");
        m = e ? atoi(e) : 1;
        if (m < 1) m = 1;
        if (m > MAX_MULT) m = MAX_MULT;
    }
    return m;
}

static int64_t now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static int make_pipe(Pipe *p, const uint32_t *code, size_t bytes, const VkDescriptorType *types, int n, uint32_t push,
                     const VkSpecializationInfo *spec) {
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < n; i++) b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)n, b};
    if (vkCreateDescriptorSetLayout(F.dev, &li, NULL, &p->dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push ? push : 4};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &p->dsl, push ? 1u : 0u, &pr};
    if (vkCreatePipelineLayout(F.dev, &pi, NULL, &p->layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code};
    VkShaderModule m;
    if (vkCreateShaderModule(F.dev, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main", spec};
    ci.layout = p->layout;
    VkResult r = vkCreateComputePipelines(F.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipe);
    vkDestroyShaderModule(F.dev, m, NULL);
    return r == VK_SUCCESS;
}

static int init(VkDevice dev, VkQueue queue, uint32_t qfamily) {
    if (F.init) return !F.failed;
    F.init = 1, F.failed = 1, F.dev = dev, F.queue = queue;
    InitializeCriticalSection(&F.mu);
    InitializeConditionVariable(&F.cv);
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    int radius = getenv("NX2_FRAMEGEN_RADIUS") ? atoi(getenv("NX2_FRAMEGEN_RADIUS")) : 8;
    radius = radius < 1 ? 1 : radius > 16 ? 16 : radius;
    VkSpecializationMapEntry me = {0, 0, 4};
    VkSpecializationInfo spec = {1, &me, 4, &radius};
    const VkDescriptorType t_si[] = {S, I}, t_ssi[] = {S, S, I}, t_sssi[] = {S, S, S, I};
    if (!make_pipe(&F.copy, fg_copy_spv, sizeof fg_copy_spv, t_si, 2, 16, NULL) ||
        !make_pipe(&F.luma, fg_luma_spv, sizeof fg_luma_spv, t_si, 2, 0, NULL) ||
        !make_pipe(&F.flow, fg_flow_spv, sizeof fg_flow_spv, t_ssi, 3, 0, &spec) ||
        !make_pipe(&F.median, fg_median_spv, sizeof fg_median_spv, t_si, 2, 0, NULL) ||
        !make_pipe(&F.synth, fg_synth_spv, sizeof fg_synth_spv, t_sssi, 4, 4, NULL)) {
        rt_log("framegen: pipelines could not be made: off");
        return 0;
    }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    vkCreateSampler(dev, &si, NULL, &F.nearest);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(dev, &si, NULL, &F.linear);
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfamily};
    if (vkCreateCommandPool(dev, &pc, NULL, &F.cpool) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = {{S, 4 * (MAX_MULT + 4)}, {I, 2 * (MAX_MULT + 4)}};
    for (int i = 0; i < NSETS; i++) {
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, F.cpool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, MAX_MULT + 4, 2, ps};
        if (vkAllocateCommandBuffers(dev, &ca, &F.sets[i].cmd) != VK_SUCCESS ||
            vkCreateFence(dev, &fc, NULL, &F.sets[i].fence) != VK_SUCCESS ||
            vkCreateDescriptorPool(dev, &dp, NULL, &F.sets[i].pool) != VK_SUCCESS)
            return 0;
    }
    F.failed = 0;
    rt_log("framegen: ready, %dx the game's frames (made from its last two, shown evenly; after Hookshot)", nx2_fg_mult());
    return 1;
}

static int make_img(Img *i, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(F.dev, &ci, NULL, &i->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(F.dev, i->image, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(F.dev, &ai, NULL, &i->mem) != VK_SUCCESS || vkBindImageMemory(F.dev, i->image, i->mem, 0) != VK_SUCCESS)
        return 0;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i->image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = f;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(F.dev, &vi, NULL, &i->view) == VK_SUCCESS;
}
static void drop_img(Img *i) {
    if (i->view) vkDestroyImageView(F.dev, i->view, NULL);
    if (i->image) vkDestroyImage(F.dev, i->image, NULL);
    if (i->mem) vkFreeMemory(F.dev, i->mem, NULL);
    memset(i, 0, sizeof *i);
}

static void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_general(VkCommandBuffer cmd, VkImage image) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// The images for frames of w x h; called with every set idle (the display thread done with them)
static int ensure_size(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (F.w == w && F.h == h && F.mult == nx2_fg_mult()) return 1;
    nx2_vk_queue_lock();
    vkQueueWaitIdle(F.queue);
    nx2_vk_queue_unlock();
    for (int i = 0; i < 2; i++) drop_img(&F.frames[i]), drop_img(&F.lumas[i]), drop_img(&F.flows[i]);
    for (int s = 0; s < NSETS; s++)
        for (int k = 0; k < MAX_MULT; k++) drop_img(&F.sets[s].frame[k]);
    F.w = w, F.h = h, F.mult = nx2_fg_mult();
    uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4, cw = (qw + 3) / 4, ch = (qh + 3) / 4;
    VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        ok = ok && make_img(&F.frames[i], w, h, VK_FORMAT_A2B10G10R10_UNORM_PACK32, rw | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        ok = ok && make_img(&F.lumas[i], qw, qh, VK_FORMAT_R16_SFLOAT, rw);
        ok = ok && make_img(&F.flows[i], cw, ch, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    }
    for (int s = 0; s < NSETS; s++)
        for (int k = 0; k < F.mult; k++)
            ok = ok && make_img(&F.sets[s].frame[k], w, h, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                rw | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!ok) {
        F.failed = 1;
        rt_log("framegen: its images (%ux%u) could not be made: off", w, h);
        return 0;
    }
    for (int i = 0; i < 2; i++) to_general(cmd, F.frames[i].image), to_general(cmd, F.lumas[i].image), to_general(cmd, F.flows[i].image);
    for (int s = 0; s < NSETS; s++)
        for (int k = 0; k < F.mult; k++) to_general(cmd, F.sets[s].frame[k].image);
    F.have_prev = 0;
    rt_log("framegen: frames of %ux%u, %d sets", w, h, NSETS);
    return 1;
}

// One pass over w x h in 8x8 groups; bindings: views with a sampler (sampled) or without (storage)
static void pass(VkCommandBuffer cmd, VkDescriptorPool pool, const Pipe *p, const VkImageView *views,
                 const VkSampler *samplers, int n, uint32_t w, uint32_t h, const void *push, uint32_t push_bytes) {
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &p->dsl};
    if (vkAllocateDescriptorSets(F.dev, &ai, &set) != VK_SUCCESS) return;
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet ws[4];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){samplers[i], views[i], VK_IMAGE_LAYOUT_GENERAL};
        ws[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)i, 0, 1,
                                       samplers[i] ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       &ii[i], NULL, NULL};
    }
    vkUpdateDescriptorSets(F.dev, (uint32_t)n, ws, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    if (push_bytes) vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cmd);
}

// ---- the display thread
void nx2_vk_show_frame(VkImage img, VkImageView view, uint32_t w, uint32_t h);   // nx2_vk.c: one present to the window

static void wait_until_near(int64_t ns) {   // a high-resolution timer: Sleep keeps to the 1 ms (or 15.6 ms) tick
    static HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    LARGE_INTEGER d;
    d.QuadPart = -(ns / 100 > 1 ? ns / 100 : 1);
    if (timer && SetWaitableTimerEx(timer, &d, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)(ns / 1000000));
}

static DWORD WINAPI display_main(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    int64_t interval = 16666667, last_at = 0, next_show = 0, shown_at = 0, since = 0;
    int on_time = 0, late = 0, hist[6] = {0};
    for (;;) {
        Job j;
        EnterCriticalSection(&F.mu);
        while (!F.njobs) SleepConditionVariableCS(&F.cv, &F.mu, INFINITE);
        j = F.jobs[0];
        memmove(F.jobs, F.jobs + 1, (size_t)--F.njobs * sizeof *F.jobs);
        LeaveCriticalSection(&F.mu);
        if (last_at) {
            int64_t d = j.at - last_at;
            if (d > 0 && d < 100000000) {
                if (d < 4000000) d = 4000000;
                if (d > 50000000) d = 50000000;
                interval = (interval * 7 + d) / 8;
            }
        }
        last_at = j.at;
        if (!since) since = j.at;
        if (j.at - since > 5000000000LL) {
            rt_log("framegen: %d frames shown on time, %d late (over 2 ms); apart: <7 ms %d, 7-9 %d, 9-12 %d, 12-17 %d, "
                   "17-25 %d, over 25 %d", on_time, late, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
            memset(hist, 0, sizeof hist);
            on_time = late = 0, since = j.at;
        }
        Set *set = &F.sets[j.set];
        int shown = set->made + 1;
        int64_t step = interval / shown;
        for (int k = 0; k <= set->made; k++) {   // each frame on an even line, never more than a game frame behind
            int64_t due = next_show, now = now_ns();
            if (due < now) due = now;
            if (due > j.at + interval + step) due = j.at + interval + step;
            if (due - now > 1000000) wait_until_near(due - now - 1000000);
            while (now_ns() < due) SwitchToThread();
            int64_t t = now_ns();
            nx2_vk_show_frame(set->frame[k].image, set->frame[k].view, F.w, F.h);
            {   // frames shown per second (the overlay's "on screen" figure)
                static int64_t sec_at;
                static int sec_n;
                sec_n++;
                if (!sec_at) sec_at = t;
                if (t - sec_at >= 1000000000LL) {
                    InterlockedExchange(&shown_fps, (LONG)((double)sec_n * 1e9 / (double)(t - sec_at) + 0.5));
                    sec_at = t, sec_n = 0;
                }
            }
            {   // NX2_FG_SHOT=<game frame> (tests): the first made frame after that present, as fg_shot.png
                static int64_t want = -2;
                if (want == -2) want = getenv("NX2_FG_SHOT") ? atoll(getenv("NX2_FG_SHOT")) : -1;
                if (want >= 0 && k == 0 && set->made && set->game_frame >= (uint64_t)want) {
                    void nx2_vk_shot_image(VkImage, uint32_t, uint32_t, const char *);
                    nx2_vk_shot_image(set->frame[0].image, F.w, F.h, "fg_shot.png");
                    nx2_vk_shot_image(set->frame[set->made].image, F.w, F.h, "fg_shot_game.png");
                    want = -1;
                }
            }
            if (shown_at) {
                int64_t d = t - shown_at;
                hist[d < 7000000 ? 0 : d < 9000000 ? 1 : d < 12000000 ? 2 : d < 17000000 ? 3 : d < 25000000 ? 4 : 5]++;
            }
            shown_at = t;
            next_show = t + step;
            if (t - due > 2000000) late++;
            else on_time++;
        }
        EnterCriticalSection(&F.mu);
        set->busy = 0;
        WakeAllConditionVariable(&F.cv);
        LeaveCriticalSection(&F.mu);
    }
    return 0;
}

// The game's frame (src: a sampled view of image, sw x sh, in *layout), scaled to w x h, interpolated with the last one
// and handed to the display thread. 1: taken (the display thread presents); 0: off or failed (present it directly).
int nx2_fg_submit(VkDevice dev, VkQueue queue, uint32_t qfamily, VkImage image, VkImageLayout *layout, VkImageView src,
                  uint32_t sw, uint32_t sh, uint32_t w, uint32_t h, uint64_t game_frame) {
    (void)sw, (void)sh;
    if (nx2_fg_mult() < 2 || !src || !w || !h || !init(dev, queue, qfamily)) return 0;
    if (!F.thread_started) {
        F.thread_started = 1;
        CloseHandle(CreateThread(NULL, 0, display_main, NULL, 0, NULL));
    }
    if (F.w != w || F.h != h) {   // new images: the display thread done with the old ones first
        EnterCriticalSection(&F.mu);
        for (;;) {
            int busy = F.njobs;
            for (int s = 0; s < NSETS; s++) busy |= F.sets[s].busy;
            if (!busy) break;
            SleepConditionVariableCS(&F.cv, &F.mu, INFINITE);
        }
        LeaveCriticalSection(&F.mu);
    }
    int64_t now = now_ns();
    int gap = F.last_at && now - F.last_at > 100000000;   // a loading hitch: nothing to blend
    F.last_at = now;
    int si;
    EnterCriticalSection(&F.mu);
    while (F.sets[F.next_set].busy) SleepConditionVariableCS(&F.cv, &F.mu, INFINITE);
    si = F.next_set;
    F.sets[si].busy = 1;
    F.next_set = (F.next_set + 1) % NSETS;
    LeaveCriticalSection(&F.mu);
    Set *set = &F.sets[si];
    vkWaitForFences(dev, 1, &set->fence, VK_TRUE, UINT64_MAX);   // its last recording done: pool and buffer free
    vkResetFences(dev, 1, &set->fence);
    vkResetDescriptorPool(dev, set->pool, 0);
    VkCommandBuffer cmd = set->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    int ok = ensure_size(cmd, w, h);
    if (ok) {
        if (*layout != VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT, *layout, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            *layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        full_barrier(cmd);   // the game's work on its frame, submitted before
        int cur = F.cur, prev = cur ^ 1, m = F.mult;
        int blend = F.have_prev && !gap;
        uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4, cw = (qw + 3) / 4, ch = (qh + 3) / 4;
        const int32_t pc[4] = {0, 0, (int32_t)w, (int32_t)h};
        {
            VkImageView v[2] = {src, F.frames[cur].view};
            VkSampler s[2] = {F.linear, VK_NULL_HANDLE};
            pass(cmd, set->pool, &F.copy, v, s, 2, w, h, pc, sizeof pc);
        }
        {
            VkImageView v[2] = {F.frames[cur].view, F.lumas[cur].view};
            VkSampler s[2] = {F.nearest, VK_NULL_HANDLE};
            pass(cmd, set->pool, &F.luma, v, s, 2, qw, qh, NULL, 0);
        }
        set->made = 0;
        if (blend && m > 1) {
            {
                VkImageView v[3] = {F.lumas[prev].view, F.lumas[cur].view, F.flows[0].view};
                VkSampler s[3] = {F.nearest, F.nearest, VK_NULL_HANDLE};
                pass(cmd, set->pool, &F.flow, v, s, 3, cw, ch, NULL, 0);
            }
            {
                VkImageView v[2] = {F.flows[0].view, F.flows[1].view};
                VkSampler s[2] = {F.nearest, VK_NULL_HANDLE};
                pass(cmd, set->pool, &F.median, v, s, 2, cw, ch, NULL, 0);
            }
            for (int k = 1; k < m; k++) {
                float t = (float)k / (float)m;
                VkImageView v[4] = {F.frames[prev].view, F.frames[cur].view, F.flows[1].view, set->frame[k - 1].view};
                VkSampler s[4] = {F.linear, F.linear, F.nearest, VK_NULL_HANDLE};
                pass(cmd, set->pool, &F.synth, v, s, 4, w, h, &t, 4);
            }
            set->made = m - 1;
        }
        VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(cmd, F.frames[cur].image, VK_IMAGE_LAYOUT_GENERAL, set->frame[set->made].image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        full_barrier(cmd);
        F.have_prev = 1;
        F.cur = prev;
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    nx2_vk_queue_lock();
    VkResult r = vkQueueSubmit(queue, 1, &sub, set->fence);
    nx2_vk_queue_unlock();
    EnterCriticalSection(&F.mu);
    if (!ok || r != VK_SUCCESS) {
        set->busy = 0;
        WakeAllConditionVariable(&F.cv);
        LeaveCriticalSection(&F.mu);
        if (r != VK_SUCCESS) rt_log("framegen: submit failed (%d)", (int)r);
        return 0;
    }
    set->game_frame = game_frame;
    if (F.njobs < 8) F.jobs[F.njobs++] = (Job){si, now};
    else set->busy = 0;
    WakeAllConditionVariable(&F.cv);
    LeaveCriticalSection(&F.mu);
    return 1;
}
