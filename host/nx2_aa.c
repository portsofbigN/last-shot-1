// Anti-aliasing of the game's final frame before FSR / NIS scale it up (nx2_vkdraw.c, where the game's neural upscaler
// ran: that upscaler did the game's anti-aliasing, and the port replaces it). NX2_AA = off | fxaa | smaa.
//   fxaa: one compute pass (shaders/fxaa.comp).
//   smaa: SMAA 1x, high preset (shaders/smaa_*.comp over the official third_party/smaa, MIT): luma edges, blending
//         weights from its area and search tables, neighbourhood blending.
// The frame is first copied into a scratch image of the same format; the passes read the copy and the last one writes
// the frame in place (a storage view of it). NX2_AA_TEST=copy (debug): the copy only.
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "host.h"
#include "fxaa_spv.h"
#include "smaa_blend_spv.h"
#include "smaa_edge_spv.h"
#include "smaa_weight_spv.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

uint32_t nx2_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

#define SETS 96

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Tex;

static struct {
    int tried, ok, smaa_ok, first;
    VkDevice dev;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline fxaa, edge, weight, blend;
    VkSampler linear, nearest;
    VkDescriptorPool pool;
    VkDescriptorSet sets[SETS];
    int next;
    Tex copy, edges, weights, area, search;   // copy / edges / weights: the frame's size and coordinates
    VkFormat copy_fmt;
    uint32_t w, h;
    VkBuffer upload;
    VkDeviceMemory upload_mem;
    int tables_ready;
} A;

int nx2_aa_mode(void) {   // 0 off, 1 FXAA, 2 SMAA
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("NX2_AA");
        m = !e ? 1 : !strcmp(e, "off") || !strcmp(e, "0") ? 0 : !strcmp(e, "smaa") ? 2 : 1;
        rt_log("aa: %s", m == 0 ? "off" : m == 1 ? "FXAA" : "SMAA");
    }
    return m;
}

static VkPipeline pipeline(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m;
    if (vkCreateShaderModule(A.dev, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
    ci.layout = A.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(A.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(A.dev, m, NULL);
    return p;
}

static void tex_free(Tex *t) {
    if (t->view) vkDestroyImageView(A.dev, t->view, NULL);
    if (t->img) vkDestroyImage(A.dev, t->img, NULL);
    if (t->mem) vkFreeMemory(A.dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

static int tex_make(Tex *t, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D, ii.format = fmt, ii.extent = (VkExtent3D){w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1, ii.samples = VK_SAMPLE_COUNT_1_BIT, ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(A.dev, &ii, NULL, &t->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(A.dev, t->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(A.dev, &ai, NULL, &t->mem) != VK_SUCCESS) return 0;
    vkBindImageMemory(A.dev, t->img, t->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, t->img, VK_IMAGE_VIEW_TYPE_2D, fmt,
                                {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(A.dev, &vi, NULL, &t->view) == VK_SUCCESS;
}

static int init(VkDevice dev) {
    A.tried = 1, A.dev = dev;
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS * 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, 2, ps};
    if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &A.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &A.dsl, 1, &pr};
    if (vkCreatePipelineLayout(dev, &li, NULL, &A.layout) != VK_SUCCESS || vkCreateSampler(dev, &si, NULL, &A.linear) != VK_SUCCESS)
        goto fail;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev, &si, NULL, &A.nearest) != VK_SUCCESS || vkCreateDescriptorPool(dev, &pci, NULL, &A.pool) != VK_SUCCESS)
        goto fail;
    for (int i = 0; i < SETS; i++) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, A.pool, 1, &A.dsl};
        if (vkAllocateDescriptorSets(dev, &ai, &A.sets[i]) != VK_SUCCESS) goto fail;
    }
    if (!(A.fxaa = pipeline(fxaa_spv, sizeof fxaa_spv))) goto fail;
    A.ok = 1;
    // SMAA: its pipelines, its two tables (uploaded at the first use) and a host-visible buffer to upload them
    A.edge = pipeline(smaa_edge_spv, sizeof smaa_edge_spv);
    A.weight = pipeline(smaa_weight_spv, sizeof smaa_weight_spv);
    A.blend = pipeline(smaa_blend_spv, sizeof smaa_blend_spv);
    VkImageUsageFlags tu = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (A.edge && A.weight && A.blend && tex_make(&A.area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, tu) &&
        tex_make(&A.search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, tu)) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, AREATEX_SIZE + SEARCHTEX_SIZE,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VkMemoryRequirements mr;
        void *p = NULL;
        if (vkCreateBuffer(dev, &bi, NULL, &A.upload) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(dev, A.upload, &mr);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                       nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            if (vkAllocateMemory(dev, &ai, NULL, &A.upload_mem) == VK_SUCCESS &&
                vkBindBufferMemory(dev, A.upload, A.upload_mem, 0) == VK_SUCCESS &&
                vkMapMemory(dev, A.upload_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                memcpy(p, areaTexBytes, AREATEX_SIZE);
                memcpy((char *)p + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
                vkUnmapMemory(dev, A.upload_mem);
                A.smaa_ok = 1;
            }
        }
    }
    rt_log("aa: FXAA ready; SMAA %s", A.smaa_ok ? "ready" : "unavailable (FXAA instead)");
    return 1;
fail:
    rt_log("aa: off (Vulkan objects could not be created)");
    return 0;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void to_general(VkCommandBuffer cb, VkImage img) {   // from UNDEFINED, at an image's first use
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

// The frame-sized images: at least w x h, the frame's format (made again when either changes; sizes are rounded up
// to 256, so the game's dynamic resolution rarely needs a new set and the GPU idle for it).
static int scratch(VkCommandBuffer cb, VkFormat fmt, uint32_t w, uint32_t h) {
    if (A.copy.img && A.copy_fmt == fmt && A.w >= w && A.h >= h) return 1;
    vkDeviceWaitIdle(A.dev);
    tex_free(&A.copy), tex_free(&A.edges), tex_free(&A.weights);
    w = (w + 255) & ~255u, h = (h + 255) & ~255u;
    if (!tex_make(&A.copy, fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 0;
    VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (A.smaa_ok && (!tex_make(&A.edges, VK_FORMAT_R8G8_UNORM, w, h, su) || !tex_make(&A.weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, su)))
        A.smaa_ok = 0;
    A.copy_fmt = fmt, A.w = w, A.h = h;
    to_general(cb, A.copy.img);
    if (A.smaa_ok) to_general(cb, A.edges.img), to_general(cb, A.weights.img);
    return 1;
}

static void upload_tables(VkCommandBuffer cb) {
    to_general(cb, A.area.img), to_general(cb, A.search.img);
    VkBufferImageCopy c[2] = {{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {AREATEX_WIDTH, AREATEX_HEIGHT, 1}},
                              {AREATEX_SIZE, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                               {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}}};
    vkCmdCopyBufferToImage(cb, A.upload, A.area.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c[0]);
    vkCmdCopyBufferToImage(cb, A.upload, A.search.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c[1]);
    A.tables_ready = 1;
}

// One pass: binding 0 (sampled, linear), 1 (storage), 2 (sampled, linear) and 3 (sampled, nearest).
static void pass(VkCommandBuffer cb, VkPipeline p, VkImageView in0, VkImageView out1, VkImageView in2, VkImageView in3,
                 const float pc[8], const int r[4]) {
    VkDescriptorSet set = A.sets[A.next];
    A.next = (A.next + 1) % SETS;
    VkDescriptorImageInfo di[4] = {{A.linear, in0, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, out1, VK_IMAGE_LAYOUT_GENERAL},
                                   {A.linear, in2, VK_IMAGE_LAYOUT_GENERAL}, {A.nearest, in3, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)i, 0, 1,
                                      i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      &di[i], NULL, NULL};
    vkUpdateDescriptorSets(A.dev, 4, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, A.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, A.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vkCmdDispatch(cb, ((uint32_t)r[2] + 15) / 16, ((uint32_t)r[3] + 15) / 16, 1);
    barrier(cb);
}

// 1 when the rectangle r (x, y, w, h) of img (GENERAL layout, w x h texels, format fmt) was anti-aliased in place.
int nx2_aa_apply(VkDevice dev, VkCommandBuffer cb, VkImage img, VkFormat fmt, VkImageView storage_view, uint32_t tw,
                 uint32_t th, const int r[4]) {
    int mode = nx2_aa_mode();
    if (!mode || !storage_view || r[2] <= 0 || r[3] <= 0) return 0;
    if (!A.tried) init(dev);
    if (!A.ok || !scratch(cb, fmt, tw, th)) return 0;
    int smaa = mode == 2 && A.smaa_ok;
    if (smaa && !A.tables_ready) upload_tables(cb);
    barrier(cb);
    // the frame into the copy: the rectangle and a border (FXAA reads neighbours; SMAA searches along edges)
    int pad = smaa ? 64 : 8;
    int x0 = r[0] - pad, y0 = r[1] - pad, x1 = r[0] + r[2] + pad, y1 = r[1] + r[3] + pad;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)tw) x1 = (int)tw;
    if (y1 > (int)th) y1 = (int)th;
    VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {x0, y0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {x0, y0, 0},
                      {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1}};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, A.copy.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    static int test = -1;
    if (test < 0) test = getenv("NX2_AA_TEST") && !strcmp(getenv("NX2_AA_TEST"), "copy");
    if (test) return 1;
    if (smaa) {   // no edges or weights outside what the passes write
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, A.edges.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr);
        vkCmdClearColorImage(cb, A.weights.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr);
    }
    barrier(cb);
    float pc[8] = {(float)r[0], (float)r[1], (float)r[2], (float)r[3], 1.0f / (float)A.w, 1.0f / (float)A.h, (float)A.w,
                   (float)A.h};
    if (!smaa) {
        float f[8] = {(float)r[0], (float)r[1], (float)r[2], (float)r[3], (float)A.w, (float)A.h, 0, 0};   // fxaa.comp
        pass(cb, A.fxaa, A.copy.view, storage_view, A.copy.view, A.copy.view, f, r);
        return 1;
    }
    pass(cb, A.edge, A.copy.view, A.edges.view, A.copy.view, A.copy.view, pc, r);
    pass(cb, A.weight, A.edges.view, A.weights.view, A.area.view, A.search.view, pc, r);
    pass(cb, A.blend, A.copy.view, storage_view, A.weights.view, A.copy.view, pc, r);
    return 1;
}
