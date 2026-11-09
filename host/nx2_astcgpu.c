// ASTC textures decoded on the GPU for the Splatoon renderer (nx2_vkdraw.c). Desktop GPUs do not sample ASTC, so
// every ASTC texture was decoded to RGBA8 on the CPU at its first use: the treasure map (11000x8000) cost ~0.2 s with
// all cores and each new item in the Gear menu ~50 ms. Here the raw blocks go into the staging ring and TotK Recomp's
// compute decoder (shaders/astc_decode.comp, the same bytes as the CPU decoder) writes the texels into a device-local
// scratch ring, which is copied into the image. NX2_ASTC=cpu keeps the CPU decoder.
// The decoded texels are then encoded to BC3 on the GPU (TotK Recomp's shaders/bc3_encode.comp), 1 byte a texel
// instead of 4: ASTC textures took most of the video memory (5 GB of an 8 GB card after a few missions in 4K).
// NX2_TEX_COMPRESS: auto (default: on with less than 12 GB of video memory), 1 on, 0 off (RGBA8, sharper normal maps).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astc_decode_spv.h"
#include "bc3_encode_spv.h"
#include "host.h"
#include "nx2_vk.h"

// Unquantization tables of tex_astc.c, as the shader reads them (copied from vk_astc.c)
// tex_astc.c's unquantization tables, as the shader reads them (Tables in astc_decode.comp): 12 + 16 row offsets in
// bytes, then the bytes. Kept in step with tex_astc.c by the oracle (--probe-astc), which compares every mode.
static const uint8_t wunq_data[] = {
    0,64,
    0,32,64,
    0,21,43,64,
    0,16,32,48,64,
    0,64,12,52,25,39,
    0,9,18,27,37,46,55,64,
    0,64,7,57,14,50,21,43,28,36,
    0,64,17,47,5,59,23,41,11,53,28,36,
    0,4,8,12,17,21,25,29,35,39,43,47,52,56,60,64,
    0,64,16,48,3,61,19,45,6,58,23,41,9,55,26,38,13,51,29,35,
    0,64,8,56,16,48,24,40,2,62,11,53,19,45,27,37,5,59,13,51,22,42,30,34,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
};
static const uint16_t wunq_off[12] = {0,2,5,9,14,20,28,38,50,66,86,110};
static const uint8_t cunq_data[] = {
    0,255,51,204,102,153,
    0,36,73,109,146,182,219,255,
    0,255,28,227,56,199,84,171,113,142,
    0,255,69,186,23,232,92,163,46,209,116,139,
    0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255,
    0,255,67,188,13,242,80,175,27,228,94,161,40,215,107,148,54,201,121,134,
    0,255,33,222,66,189,99,156,11,244,44,211,77,178,110,145,22,233,55,200,88,167,121,134,
    0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,
    231,239,247,255,
    0,255,32,223,65,190,97,158,6,249,39,216,71,184,104,151,13,242,45,210,78,177,110,145,19,236,52,203,
    84,171,117,138,26,229,58,197,91,164,123,132,
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,
    103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
    0,255,16,239,32,223,48,207,64,191,80,175,96,159,112,143,3,252,19,236,35,220,51,204,67,188,83,172,
    100,155,116,139,6,249,22,233,38,217,54,201,71,184,87,168,103,152,119,136,9,246,25,230,42,213,58,197,
    74,181,90,165,106,149,122,133,13,242,29,226,45,210,61,194,77,178,93,162,109,146,125,130,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,2,253,10,245,18,237,26,229,35,220,43,212,51,204,59,196,67,188,75,180,83,172,91,164,
    99,156,107,148,115,140,123,132,5,250,13,242,21,234,29,226,37,218,45,210,53,202,61,194,70,185,78,177,
    86,169,94,161,102,153,110,145,118,137,126,129,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,
    126,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,
    177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,
    227,229,231,233,235,237,239,241,243,245,247,249,251,253,255,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,1,254,9,246,17,238,25,230,33,222,41,214,49,206,57,198,65,190,73,182,81,174,89,166,
    97,158,105,150,113,142,121,134,3,252,11,244,19,236,27,228,35,220,43,212,51,204,59,196,67,188,75,180,
    83,172,91,164,99,156,107,148,115,140,123,132,4,251,12,243,20,235,28,227,36,219,44,211,52,203,60,195,
    68,187,76,179,84,171,92,163,100,155,108,147,116,139,124,131,6,249,14,241,22,233,30,225,38,217,46,
    209,54,201,62,193,70,185,78,177,86,169,94,161,102,153,110,145,118,137,126,129,
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};
static const uint16_t cunq_off[16] = {0,6,14,24,36,52,72,96,128,168,216,280,360,456,584,744};
_Static_assert(sizeof wunq_data == 142 && sizeof cunq_data == 936, "unquantization table size");

enum { TAB_BYTES = 28 * 4, TAB_WUNQ = TAB_BYTES, TAB_CUNQ = TAB_BYTES + 144, TAB_SIZE = TAB_CUNQ + 936 };

typedef struct { uint32_t in_base, out_base, width, height, bw, bh, bx, bslice, srgb; } Push;

#define SCRATCH (64ull << 20)   // decoded texels in flight; a band of a texture is at most this

static struct {
    int tried, ok;
    VkDevice dev;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkDescriptorSet set, bc_set;
    VkPipeline bc_pipe;          // 0: images stay RGBA8
    VkBuffer scratch, tab, bc_buf;
    VkDeviceMemory scratch_mem, tab_mem, bc_mem;
    VkDeviceSize pos;            // next free byte of the scratch
    VkCommandBuffer last_cb;
    uint64_t texels, calls;
} G;

static void fill_tables(uint8_t *p) {
    uint32_t off[28];
    for (int i = 0; i < 12; i++) off[i] = TAB_WUNQ + wunq_off[i];
    for (int i = 0; i < 16; i++) off[12 + i] = TAB_CUNQ + cunq_off[i];
    memset(p, 0, TAB_SIZE);
    memcpy(p, off, sizeof off);
    memcpy(p + TAB_WUNQ, wunq_data, sizeof wunq_data);
    memcpy(p + TAB_CUNQ, cunq_data, sizeof cunq_data);
}

static int buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer *b, VkDeviceMemory *m) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage};
    if (vkCreateBuffer(G.dev, &bi, NULL, b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(G.dev, *b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, nx2_vk_memory_type(mr.memoryTypeBits, props)};
    return vkAllocateMemory(G.dev, &ai, NULL, m) == VK_SUCCESS && vkBindBufferMemory(G.dev, *b, *m, 0) == VK_SUCCESS;
}

static int init(VkDevice dev, VkPhysicalDevice phys, VkBuffer src, VkDeviceSize src_size) {
    G.tried = 1, G.dev = dev;
    const char *e = getenv("NX2_ASTC");
    if (e && !strcmp(e, "cpu")) {
        rt_log("astc: decoded on the CPU (NX2_ASTC=cpu)");
        return 0;
    }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    if (pp.limits.maxStorageBufferRange < src_size) {   // the staging ring is bound whole
        rt_log("astc: storage buffers of at most %u bytes, smaller than the staging ring: decoded on the CPU",
               pp.limits.maxStorageBufferRange);
        return 0;
    }
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof astc_decode_spv, astc_decode_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps};
    if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &G.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &G.dsl, 1, &pr};
    if (vkCreatePipelineLayout(dev, &li, NULL, &G.layout) != VK_SUCCESS) goto fail;
    if (vkCreateShaderModule(dev, &mi, NULL, &m) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
    ci.layout = G.layout;
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe);
    vkDestroyShaderModule(dev, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    if (!buffer(SCRATCH, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.scratch, &G.scratch_mem) ||
        !buffer(TAB_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &G.tab, &G.tab_mem))
        goto fail;
    void *p;
    if (vkMapMemory(dev, G.tab_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto fail;
    fill_tables(p);
    vkUnmapMemory(dev, G.tab_mem);
    if (vkCreateDescriptorPool(dev, &pci, NULL, &G.pool) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, G.pool, 1, &G.dsl};
    if (vkAllocateDescriptorSets(dev, &ai, &G.set) != VK_SUCCESS) goto fail;
    VkDescriptorBufferInfo dbi[3] = {{src, 0, src_size}, {G.scratch, 0, SCRATCH}, {G.tab, 0, TAB_SIZE}};
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, G.set, (uint32_t)i, 0, 1,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[i], NULL};
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
    G.ok = 1;
    // BC3 encoding: when asked for (or auto with a smaller card) and the device samples BC3
    const char *tc = getenv("NX2_TEX_COMPRESS");
    int want = tc && !strcmp(tc, "1");
    if (!tc || !strcmp(tc, "auto")) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        VkDeviceSize vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > vram)
                vram = mp.memoryHeaps[i].size;
        want = vram < (12ull << 30);
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_BC3_SRGB_BLOCK, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkShaderModuleCreateInfo bmi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof bc3_encode_spv, bc3_encode_spv};
    VkShaderModule bm = VK_NULL_HANDLE;
    if (want && (fp.optimalTilingFeatures & need) == need && vkCreateShaderModule(dev, &bmi, NULL, &bm) == VK_SUCCESS) {
        ci.stage.module = bm;
        VkDescriptorSetAllocateInfo bai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, G.pool, 1, &G.dsl};
        VkDeviceSize bcs = SCRATCH / 4 + (4ull << 20);
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &G.bc_pipe) == VK_SUCCESS &&
            buffer(bcs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.bc_buf, &G.bc_mem) &&
            vkAllocateDescriptorSets(dev, &bai, &G.bc_set) == VK_SUCCESS) {
            VkDescriptorBufferInfo bdi[3] = {{G.scratch, 0, SCRATCH}, {G.bc_buf, 0, bcs}, {G.tab, 0, TAB_SIZE}};
            for (int i = 0; i < 3; i++) w[i].dstSet = G.bc_set, w[i].pBufferInfo = &bdi[i];
            vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
        } else {
            if (G.bc_pipe) vkDestroyPipeline(dev, G.bc_pipe, NULL);
            G.bc_pipe = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(dev, bm, NULL);
    }
    rt_log("astc: decoded on the GPU, stored as %s", G.bc_pipe ? "BC3 (1 byte a texel)" : "RGBA8");
    return 1;
fail:
    rt_log("astc: the GPU decoder could not be created: decoded on the CPU");
    return 0;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkAccessFlags sa, VkPipelineStageFlags d, VkAccessFlags da) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, sa, da};
    vkCmdPipelineBarrier(cb, s, d, 0, 1, &mb, 0, NULL, 0, NULL);
}

// 1: ASTC images are BC3 (nx2_vkdraw.c creates them so and uploads them only through nx2_astc_gpu_band)
int nx2_astc_gpu_bc(VkDevice dev, VkPhysicalDevice phys, VkBuffer src, VkDeviceSize src_size) {
    if (!G.tried) init(dev, phys, src, src_size);
    return G.ok && G.bc_pipe;
}

// Rows of a w-wide texture one call can take (0: the GPU decoder is off). src is the staging ring, bound whole.
uint32_t nx2_astc_gpu_rows(VkDevice dev, VkPhysicalDevice phys, VkBuffer src, VkDeviceSize src_size, uint32_t w) {
    if (!G.tried) init(dev, phys, src, src_size);
    if (!G.ok || !w) return 0;
    VkDeviceSize rows = (SCRATCH - 256) / ((VkDeviceSize)w * 4);
    return rows > 0xffffffffu ? 0xffffffffu : (uint32_t)rows;
}

// Records the decode of the blocks at src_off in the staging ring (16-byte aligned, rows of whole blocks, tightly
// packed) into the w x h region at (0, y, z) of img's level / layer; img is in GENERAL layout. 0: not done.
int nx2_astc_gpu_band(VkCommandBuffer cb, VkDeviceSize src_off, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb,
                      VkImage img, VkImageAspectFlags aspect, uint32_t level, uint32_t layer, int32_t y, int32_t z) {
    if (!G.ok || (src_off & 15) || bw < 4 || bh < 4 || bw > 12 || bh > 12) return 0;
    VkDeviceSize out = ((VkDeviceSize)w * h * 4 + 255) & ~255ull;
    if (out > SCRATCH) return 0;
    if (G.pos + out > SCRATCH || cb != G.last_cb) {   // the scratch from its start: earlier copies out of it must be done
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
        G.pos = 0, G.last_cb = cb;
    }
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;   // (the CPU's writes to the ring are visible at submit)
    Push pc = {(uint32_t)(src_off / 16), (uint32_t)(G.pos / 4), w, h, bw, bh, bx, bx * by, (uint32_t)(srgb != 0)};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 1, &G.set, 0, NULL);
    vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    uint32_t bpw = bw * bh <= 64 ? 64 / (bw * bh) : 1;   // blocks per workgroup, as astc_decode.comp main
    vkCmdDispatch(cb, (bx + bpw - 1) / bpw, by, 1);
    barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    if (G.bc_pipe) {   // the texels to BC3 blocks (y is on a 4-row boundary: nx2_vkdraw.c sizes the bands so)
        uint32_t bx4 = (w + 3) / 4, by4 = (h + 3) / 4;
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);   // the previous band's copy out of bc_buf
        struct { uint32_t in_base, out_base, width, height, bx, bslice; } bp = {(uint32_t)(G.pos / 4), 0, w, h, bx4, bx4 * by4};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.bc_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 1, &G.bc_set, 0, NULL);
        vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof bp, &bp);
        vkCmdDispatch(cb, (bx4 + 7) / 8, (by4 + 7) / 8, 1);
        barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy cp = {0, bx4 * 4, by4 * 4, {aspect, level, layer, 1}, {0, y, z}, {w, h, 1}};
        vkCmdCopyBufferToImage(cb, G.bc_buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else {
        VkBufferImageCopy cp = {G.pos, 0, 0, {aspect, level, layer, 1}, {0, y, z}, {w, h, 1}};
        vkCmdCopyBufferToImage(cb, G.scratch, img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    }
    G.pos += out;
    G.texels += (uint64_t)w * h;
    uint64_t n = ++G.calls;
    if (!(n & (n - 1)))
        rt_log("astc: %llu GPU decodes, %.1f M texels", (unsigned long long)G.calls, (double)G.texels / 1e6);
    return 1;
}
