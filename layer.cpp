// Nyushu FrameGen - Vulkan layer yang menyisipkan 1 frame interpolasi di antara 2 frame emulator (x2).
// Menyamar sebagai VK_LAYER_KHRONOS_validation supaya dimuat lewat opsi "Graphics Debugging" emulator.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "NyushuFG", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "NyushuFG", __VA_ARGS__)
#define EXPORT extern "C" __attribute__((visibility("default")))
#define LAYER_NAME "VK_LAYER_KHRONOS_validation"
#define VKAPI VKAPI_ATTR VKAPI_CALL
#define VKCHK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { LOGE("%s gagal: %d (baris %d)", #x, (int)_r, __LINE__); return false; } } while (0)

static const uint32_t k_luma_spv[] = {
#include "luma.inc"
};
static const uint32_t k_motion_spv[] = {
#include "motion.inc"
};
static const uint32_t k_interp_spv[] = {
#include "interp.inc"
};

// ---------------------------------------------------------------- struct loader (vk_layer.h versi ringkas)
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_physGPA)(VkInstance, const char*);
struct LayerInstanceLink { LayerInstanceLink* pNext; PFN_vkGetInstanceProcAddr gipa; PFN_physGPA physGpa; };
struct LayerInstanceCreateInfo { VkStructureType sType; const void* pNext; int function; union { LayerInstanceLink* pLayerInfo; void* cb; } u; };
struct LayerDeviceLink { LayerDeviceLink* pNext; PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa; };
struct LayerDeviceCreateInfo { VkStructureType sType; const void* pNext; int function; union { LayerDeviceLink* pLayerInfo; void* cb; } u; };
struct NegotiateLayerInterface { int sType; void* pNext; uint32_t version; PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa; PFN_physGPA physGpa; };
static const int STYPE_LOADER_INSTANCE = 47, STYPE_LOADER_DEVICE = 48, LAYER_LINK_INFO = 0;

// ---------------------------------------------------------------- data
#define DEV_FUNCS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
 X(AcquireNextImageKHR) X(QueuePresentKHR) X(QueueSubmit) X(CreateImage) X(DestroyImage) \
 X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView) \
 X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBindPipeline) \
 X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CreateFence) X(DestroyFence) \
 X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(DeviceWaitIdle)

struct DevDispatch {
#define X(n) PFN_vk##n n = nullptr;
    DEV_FUNCS(X)
#undef X
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
};

struct InstanceData {
    VkInstance inst = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemProps = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfCaps = nullptr;
};

struct DeviceData {
    VkDevice dev = nullptr;
    VkPhysicalDevice phys = nullptr;
    InstanceData* ii = nullptr;
    DevDispatch d;
    bool usable = false;
    VkPhysicalDeviceMemoryProperties mem{};
    std::unordered_map<VkQueue, uint32_t> qfam;
    // resource per-device (dibuat sekali, malas)
    bool resTried = false, resOk = false;
    VkShaderModule smLuma = 0, smMotion = 0, smInterp = 0;
    VkDescriptorSetLayout dsl = 0;
    VkPipelineLayout pl = 0;
    VkPipeline pLuma = 0, pMotion = 0, pInterp = 0;
};

struct Img { VkImage img = 0; VkDeviceMemory mem = 0; VkImageView view = 0; };
static const int RING = 4;
struct Slot { VkCommandBuffer cb = nullptr; VkFence fence = 0; VkSemaphore semAcq = 0, semMid = 0, semCur = 0; };

struct SwapData {
    DeviceData* dd = nullptr;
    VkSwapchainKHR sc = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkExtent2D ext{};
    bool enabled = false;
    std::mutex mu;
    bool resTried = false, resOk = false;
    uint32_t halfW = 0, halfH = 0, bx = 0, by = 0, family = 0;
    std::vector<VkImage> images;
    Img cur, prev, mid, luma[2];
    VkBuffer mvBuf = 0; VkDeviceMemory mvMem = 0;
    VkDescriptorPool pool = 0;
    VkDescriptorSet setLuma[2]{}, setMotion[2]{}, setInterp = 0;
    VkCommandPool cmdPool = 0;
    Slot slot[RING];
    uint32_t ringIdx = 0;
    bool hasPrev = false, layoutsInit = false;
    int par = 0;
};

static std::mutex g_mu;
static std::unordered_map<void*, InstanceData*> g_inst;
static std::unordered_map<void*, DeviceData*> g_dev;
static std::unordered_map<uint64_t, SwapData*> g_sc;

static inline void* Key(const void* h) { return *(void* const*)h; }

static InstanceData* getInst(const void* handle) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_inst.find(Key(handle));
    if (it != g_inst.end()) return it->second;
    if (g_inst.size() == 1) return g_inst.begin()->second;
    return nullptr;
}
static DeviceData* getDev(const void* handle) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_dev.find(Key(handle));
    return it == g_dev.end() ? nullptr : it->second;
}

// setprop debug.nyushu.fg 0  -> matikan tanpa ganti file. Default: nyala.
static bool fgEnabled() {
    // NYUSHU_FG diisi tombol drawer (FrameGen.smali): "1" nyala, "0" mati. Dicek tiap present.
    const char* e = getenv("NYUSHU_FG");
    if (e && e[0]) return e[0] != '0';
    static std::atomic<int> cnt{0};
    static std::atomic<bool> en{true};
    if ((cnt++ % 240) == 0) {
        char v[PROP_VALUE_MAX] = {0};
        int n = __system_property_get("debug.nyushu.fg", v);
        en = (n == 0 || v[0] != '0');
    }
    return en;
}

static bool supportedFormat(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

// ---------------------------------------------------------------- helper resource
static uint32_t findMem(DeviceData* dd, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < dd->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (dd->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool allocBind(DeviceData* dd, const VkMemoryRequirements& mr, VkDeviceMemory* out) {
    uint32_t mt = findMem(dd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { LOGE("tidak ada memory type device-local"); return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    VKCHK(dd->d.AllocateMemory(dd->dev, &ai, nullptr, out));
    return true;
}

static bool mkImg(DeviceData* dd, Img& o, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    auto& d = dd->d;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHK(d.CreateImage(dd->dev, &ci, nullptr, &o.img));
    VkMemoryRequirements mr; d.GetImageMemoryRequirements(dd->dev, o.img, &mr);
    if (!allocBind(dd, mr, &o.mem)) return false;
    VKCHK(d.BindImageMemory(dd->dev, o.img, o.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHK(d.CreateImageView(dd->dev, &vi, nullptr, &o.view));
    return true;
}
static void rmImg(DeviceData* dd, Img& o) {
    auto& d = dd->d;
    if (o.view) d.DestroyImageView(dd->dev, o.view, nullptr);
    if (o.img) d.DestroyImage(dd->dev, o.img, nullptr);
    if (o.mem) d.FreeMemory(dd->dev, o.mem, nullptr);
    o = Img{};
}

static bool ensureDeviceRes(DeviceData* dd) {
    if (dd->resTried) return dd->resOk;
    dd->resTried = true;
    auto& d = dd->d; VkDevice dev = dd->dev;
    auto mkSm = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) -> bool {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes; ci.pCode = code;
        VKCHK(d.CreateShaderModule(dev, &ci, nullptr, out));
        return true;
    };
    if (!mkSm(k_luma_spv, sizeof(k_luma_spv), &dd->smLuma)) return false;
    if (!mkSm(k_motion_spv, sizeof(k_motion_spv), &dd->smMotion)) return false;
    if (!mkSm(k_interp_spv, sizeof(k_interp_spv), &dd->smInterp)) return false;

    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; i++) {
        b[i].binding = i;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4; dl.pBindings = b;
    VKCHK(d.CreateDescriptorSetLayout(dev, &dl, nullptr, &dd->dsl));

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &dd->dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VKCHK(d.CreatePipelineLayout(dev, &pli, nullptr, &dd->pl));

    auto mkPipe = [&](VkShaderModule sm, VkPipeline* out) -> bool {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = sm; ci.stage.pName = "main";
        ci.layout = dd->pl;
        VKCHK(d.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, out));
        return true;
    };
    if (!mkPipe(dd->smLuma, &dd->pLuma)) return false;
    if (!mkPipe(dd->smMotion, &dd->pMotion)) return false;
    if (!mkPipe(dd->smInterp, &dd->pInterp)) return false;
    dd->resOk = true;
    LOGI("pipeline compute siap");
    return true;
}

static void destroyDeviceRes(DeviceData* dd) {
    auto& d = dd->d; VkDevice dev = dd->dev;
    if (dd->pLuma) d.DestroyPipeline(dev, dd->pLuma, nullptr);
    if (dd->pMotion) d.DestroyPipeline(dev, dd->pMotion, nullptr);
    if (dd->pInterp) d.DestroyPipeline(dev, dd->pInterp, nullptr);
    if (dd->pl) d.DestroyPipelineLayout(dev, dd->pl, nullptr);
    if (dd->dsl) d.DestroyDescriptorSetLayout(dev, dd->dsl, nullptr);
    if (dd->smLuma) d.DestroyShaderModule(dev, dd->smLuma, nullptr);
    if (dd->smMotion) d.DestroyShaderModule(dev, dd->smMotion, nullptr);
    if (dd->smInterp) d.DestroyShaderModule(dev, dd->smInterp, nullptr);
}

static void destroySwap(SwapData* s) {
    DeviceData* dd = s->dd; auto& d = dd->d; VkDevice dev = dd->dev;
    for (int i = 0; i < RING; i++) {
        if (s->slot[i].fence) d.DestroyFence(dev, s->slot[i].fence, nullptr);
        if (s->slot[i].semAcq) d.DestroySemaphore(dev, s->slot[i].semAcq, nullptr);
        if (s->slot[i].semMid) d.DestroySemaphore(dev, s->slot[i].semMid, nullptr);
        if (s->slot[i].semCur) d.DestroySemaphore(dev, s->slot[i].semCur, nullptr);
    }
    if (s->cmdPool) d.DestroyCommandPool(dev, s->cmdPool, nullptr);
    if (s->pool) d.DestroyDescriptorPool(dev, s->pool, nullptr);
    if (s->mvBuf) d.DestroyBuffer(dev, s->mvBuf, nullptr);
    if (s->mvMem) d.FreeMemory(dev, s->mvMem, nullptr);
    rmImg(dd, s->cur); rmImg(dd, s->prev); rmImg(dd, s->mid);
    rmImg(dd, s->luma[0]); rmImg(dd, s->luma[1]);
}

static bool initSwap(SwapData* s, uint32_t family) {
    DeviceData* dd = s->dd; auto& d = dd->d; VkDevice dev = dd->dev;
    if (!ensureDeviceRes(dd)) return false;
    uint32_t w = s->ext.width, h = s->ext.height;
    s->halfW = (w + 1) / 2; s->halfH = (h + 1) / 2;
    s->bx = (s->halfW + 7) / 8; s->by = (s->halfH + 7) / 8;
    s->family = family;

    uint32_t n = 0;
    VKCHK(d.GetSwapchainImagesKHR(dev, s->sc, &n, nullptr));
    s->images.resize(n);
    VKCHK(d.GetSwapchainImagesKHR(dev, s->sc, &n, s->images.data()));

    const VkImageUsageFlags U = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!mkImg(dd, s->cur, VK_FORMAT_R8G8B8A8_UNORM, w, h, U)) return false;
    if (!mkImg(dd, s->prev, VK_FORMAT_R8G8B8A8_UNORM, w, h, U)) return false;
    if (!mkImg(dd, s->mid, VK_FORMAT_R8G8B8A8_UNORM, w, h, U)) return false;
    for (int i = 0; i < 2; i++)
        if (!mkImg(dd, s->luma[i], VK_FORMAT_R32_SFLOAT, s->halfW, s->halfH, VK_IMAGE_USAGE_STORAGE_BIT)) return false;

    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = (VkDeviceSize)s->bx * s->by * 16; bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHK(d.CreateBuffer(dev, &bc, nullptr, &s->mvBuf));
    VkMemoryRequirements mr; d.GetBufferMemoryRequirements(dev, s->mvBuf, &mr);
    if (!allocBind(dd, mr, &s->mvMem)) return false;
    VKCHK(d.BindBufferMemory(dev, s->mvBuf, s->mvMem, 0));

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pc.maxSets = 5; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
    VKCHK(d.CreateDescriptorPool(dev, &pc, nullptr, &s->pool));
    VkDescriptorSetLayout ls[5] = {dd->dsl, dd->dsl, dd->dsl, dd->dsl, dd->dsl};
    VkDescriptorSet sets[5];
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sa.descriptorPool = s->pool; sa.descriptorSetCount = 5; sa.pSetLayouts = ls;
    VKCHK(d.AllocateDescriptorSets(dev, &sa, sets));
    s->setLuma[0] = sets[0]; s->setLuma[1] = sets[1];
    s->setMotion[0] = sets[2]; s->setMotion[1] = sets[3];
    s->setInterp = sets[4];

    VkDescriptorImageInfo ii[16]; VkDescriptorBufferInfo bi[4]; VkWriteDescriptorSet w2[16];
    int ni = 0, nb = 0, nw = 0;
    auto wImg = [&](VkDescriptorSet set, uint32_t b, VkImageView v) {
        ii[ni] = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
        w2[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w2[nw].dstSet = set; w2[nw].dstBinding = b; w2[nw].descriptorCount = 1;
        w2[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w2[nw].pImageInfo = &ii[ni];
        ni++; nw++;
    };
    auto wBuf = [&](VkDescriptorSet set, uint32_t b) {
        bi[nb] = {s->mvBuf, 0, VK_WHOLE_SIZE};
        w2[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w2[nw].dstSet = set; w2[nw].dstBinding = b; w2[nw].descriptorCount = 1;
        w2[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w2[nw].pBufferInfo = &bi[nb];
        nb++; nw++;
    };
    for (int p = 0; p < 2; p++) {
        wImg(s->setLuma[p], 0, s->cur.view);
        wImg(s->setLuma[p], 1, s->luma[p].view);
        wImg(s->setMotion[p], 0, s->luma[p].view);        // frame sekarang
        wImg(s->setMotion[p], 1, s->luma[1 - p].view);    // frame sebelumnya
        wBuf(s->setMotion[p], 3);
    }
    wImg(s->setInterp, 0, s->prev.view);
    wImg(s->setInterp, 1, s->cur.view);
    wImg(s->setInterp, 2, s->mid.view);
    wBuf(s->setInterp, 3);
    d.UpdateDescriptorSets(dev, nw, w2, 0, nullptr);

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex = family;
    VKCHK(d.CreateCommandPool(dev, &cp, nullptr, &s->cmdPool));
    VkCommandBuffer cbs[RING];
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = s->cmdPool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = RING;
    VKCHK(d.AllocateCommandBuffers(dev, &ca, cbs));
    for (int i = 0; i < RING; i++) {
        s->slot[i].cb = cbs[i];
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHK(d.CreateFence(dev, &fi, nullptr, &s->slot[i].fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHK(d.CreateSemaphore(dev, &si, nullptr, &s->slot[i].semAcq));
        VKCHK(d.CreateSemaphore(dev, &si, nullptr, &s->slot[i].semMid));
        VKCHK(d.CreateSemaphore(dev, &si, nullptr, &s->slot[i].semCur));
    }
    LOGI("swapchain %ux%u siap (%u image, blok %ux%u)", w, h, n, s->bx, s->by);
    return true;
}

// ---------------------------------------------------------------- rekam command buffer
static void imgBarrier(DevDispatch& d, VkCommandBuffer cb, VkImage img, VkImageLayout o, VkImageLayout n,
                       VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = o; b.newLayout = n;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.CmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}
static void memBarrier(DevDispatch& d, VkCommandBuffer cb, VkPipelineStageFlags ss, VkAccessFlags sa,
                       VkPipelineStageFlags ds, VkAccessFlags da) {
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = sa; m.dstAccessMask = da;
    d.CmdPipelineBarrier(cb, ss, ds, 0, 1, &m, 0, nullptr, 0, nullptr);
}

static bool record(SwapData* s, Slot& sl, VkImage appImg, VkImage midDst, bool doMid) {
    DeviceData* dd = s->dd; auto& d = dd->d; VkCommandBuffer cb = sl.cb;
    const VkPipelineStageFlags ALL = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags MRW = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    uint32_t w = s->ext.width, h = s->ext.height;
    VkImageCopy rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {w, h, 1};

    d.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHK(d.BeginCommandBuffer(cb, &bi));

    if (!s->layoutsInit) {
        VkImage all[5] = {s->cur.img, s->prev.img, s->mid.img, s->luma[0].img, s->luma[1].img};
        for (VkImage im : all)
            imgBarrier(d, cb, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, MRW,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ALL);
        s->layoutsInit = true;
    }
    memBarrier(d, cb, ALL, MRW, ALL, MRW);   // pisahkan dari frame sebelumnya

    // 1. salin frame emulator -> cur
    imgBarrier(d, cb, appImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               0, VK_ACCESS_TRANSFER_READ_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT);
    d.CmdCopyImage(cb, appImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->cur.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    imgBarrier(d, cb, appImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    memBarrier(d, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // 2. luma setengah-res
    const VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    d.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pLuma);
    d.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pl, 0, 1, &s->setLuma[s->par], 0, nullptr);
    d.CmdDispatch(cb, (s->halfW + 7) / 8, (s->halfH + 7) / 8, 1);
    memBarrier(d, cb, CS, VK_ACCESS_SHADER_WRITE_BIT, CS, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    if (doMid) {
        int32_t blocks[2] = {(int32_t)s->bx, (int32_t)s->by};
        d.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pMotion);
        d.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pl, 0, 1, &s->setMotion[s->par], 0, nullptr);
        d.CmdPushConstants(cb, dd->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, blocks);
        d.CmdDispatch(cb, (s->bx + 7) / 8, (s->by + 7) / 8, 1);
        memBarrier(d, cb, CS, VK_ACCESS_SHADER_WRITE_BIT, CS, VK_ACCESS_SHADER_READ_BIT);

        d.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pInterp);
        d.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dd->pl, 0, 1, &s->setInterp, 0, nullptr);
        d.CmdPushConstants(cb, dd->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, blocks);
        d.CmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        memBarrier(d, cb, CS, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

        // 3. mid -> image swapchain hasil acquire
        imgBarrier(d, cb, midDst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT, ALL, VK_PIPELINE_STAGE_TRANSFER_BIT);
        d.CmdCopyImage(cb, s->mid.img, VK_IMAGE_LAYOUT_GENERAL, midDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        imgBarrier(d, cb, midDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    // 4. prev = cur (untuk frame berikutnya)
    memBarrier(d, cb, ALL, MRW, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    d.CmdCopyImage(cb, s->cur.img, VK_IMAGE_LAYOUT_GENERAL, s->prev.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VKCHK(d.EndCommandBuffer(cb));
    return true;
}

// ---------------------------------------------------------------- hook device
VKAPI VkResult hk_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    DeviceData* dd = nullptr; SwapData* s = nullptr; uint32_t fam = UINT32_MAX;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_dev.find(Key(queue));
        if (it == g_dev.end()) return VK_ERROR_DEVICE_LOST;
        dd = it->second;
        if (pi->swapchainCount == 1) {
            auto is = g_sc.find((uint64_t)pi->pSwapchains[0]);
            if (is != g_sc.end()) s = is->second;
        }
        auto q = dd->qfam.find(queue);
        if (q != dd->qfam.end()) fam = q->second;
    }
    auto& d = dd->d;
    if (!s || !s->enabled || fam == UINT32_MAX || !dd->usable) return d.QueuePresentKHR(queue, pi);
    if (!fgEnabled()) { s->hasPrev = false; return d.QueuePresentKHR(queue, pi); }

    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->resTried) { s->resTried = true; s->resOk = initSwap(s, fam); if (!s->resOk) LOGE("init gagal, framegen dimatikan untuk swapchain ini"); }
    if (!s->resOk || fam != s->family) return d.QueuePresentKHR(queue, pi);

    VkDevice dev = dd->dev; VkSwapchainKHR sc = pi->pSwapchains[0];
    uint32_t idx = pi->pImageIndices[0];
    if (idx >= s->images.size()) return d.QueuePresentKHR(queue, pi);
    Slot& sl = s->slot[s->ringIdx++ % RING];
    d.WaitForFences(dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
    d.ResetFences(dev, 1, &sl.fence);

    uint32_t j = UINT32_MAX; bool doMid = false;
    if (s->hasPrev) {
        VkResult ar = d.AcquireNextImageKHR(dev, sc, 20000000ull, sl.semAcq, VK_NULL_HANDLE, &j);
        doMid = (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) && j < s->images.size();
    }
    if (!record(s, sl, s->images[idx], doMid ? s->images[j] : VK_NULL_HANDLE, doMid)) {
        LOGE("record gagal"); s->resOk = false;
        return d.QueuePresentKHR(queue, pi);
    }

    std::vector<VkSemaphore> waits(pi->pWaitSemaphores, pi->pWaitSemaphores + pi->waitSemaphoreCount);
    if (doMid) waits.push_back(sl.semAcq);
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSemaphore sig[2] = {sl.semCur, sl.semMid};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = (uint32_t)waits.size(); si.pWaitSemaphores = waits.data(); si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &sl.cb;
    si.signalSemaphoreCount = doMid ? 2 : 1; si.pSignalSemaphores = sig;
    VkResult sr = d.QueueSubmit(queue, 1, &si, sl.fence);
    if (sr != VK_SUCCESS) {
        LOGE("QueueSubmit gagal: %d, framegen dimatikan", (int)sr);
        s->resOk = false;
        return d.QueuePresentKHR(queue, pi);
    }

    if (doMid) {
        VkPresentInfoKHR pm{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pm.waitSemaphoreCount = 1; pm.pWaitSemaphores = &sl.semMid;
        pm.swapchainCount = 1; pm.pSwapchains = &sc; pm.pImageIndices = &j;
        d.QueuePresentKHR(queue, &pm);
    }
    VkPresentInfoKHR pc = *pi;
    pc.waitSemaphoreCount = 1; pc.pWaitSemaphores = &sl.semCur;
    s->par ^= 1; s->hasPrev = true;
    return d.QueuePresentKHR(queue, &pc);
}

VKAPI VkResult hk_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCI,
                                     const VkAllocationCallbacks* pA, VkSwapchainKHR* pSc) {
    DeviceData* dd = getDev(device);
    VkSwapchainCreateInfoKHR ci = *pCI;
    bool ok = dd && dd->usable && supportedFormat(ci.imageFormat) && ci.imageArrayLayers == 1 && dd->ii->GetSurfCaps;
    if (ok) {
        VkSurfaceCapabilitiesKHR caps{};
        if (dd->ii->GetSurfCaps(dd->phys, ci.surface, &caps) != VK_SUCCESS) ok = false;
        else {
            const VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if ((caps.supportedUsageFlags & need) != need) ok = false;
            else {
                ci.imageUsage |= need;
                uint32_t want = ci.minImageCount + 2;   // +2 image cadangan untuk frame sisipan
                if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
                ci.minImageCount = std::max(ci.minImageCount, want);
            }
        }
    }
    VkResult r = dd->d.CreateSwapchainKHR(device, &ci, pA, pSc);
    if (r == VK_SUCCESS && ok) {
        SwapData* s = new SwapData();
        s->dd = dd; s->sc = *pSc; s->fmt = ci.imageFormat; s->ext = ci.imageExtent; s->enabled = true;
        std::lock_guard<std::mutex> l(g_mu);
        g_sc[(uint64_t)*pSc] = s;
        LOGI("swapchain dibuat %ux%u fmt=%d minImage=%u", s->ext.width, s->ext.height, (int)s->fmt, ci.minImageCount);
    } else if (r == VK_SUCCESS) {
        LOGI("swapchain dilewati (format/usage tidak didukung): fmt=%d", (int)ci.imageFormat);
    }
    return r;
}

VKAPI void hk_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* pA) {
    DeviceData* dd = getDev(device);
    SwapData* s = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_sc.find((uint64_t)sc);
        if (it != g_sc.end()) { s = it->second; g_sc.erase(it); }
    }
    if (s) { dd->d.DeviceWaitIdle(device); destroySwap(s); delete s; }
    dd->d.DestroySwapchainKHR(device, sc, pA);
}

VKAPI void hk_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* pQ) {
    DeviceData* dd = getDev(device);
    dd->d.GetDeviceQueue(device, family, index, pQ);
    if (pQ && *pQ) { std::lock_guard<std::mutex> l(g_mu); dd->qfam[*pQ] = family; }
}
VKAPI void hk_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* pQ) {
    DeviceData* dd = getDev(device);
    dd->d.GetDeviceQueue2(device, info, pQ);
    if (pQ && *pQ) { std::lock_guard<std::mutex> l(g_mu); dd->qfam[*pQ] = info->queueFamilyIndex; }
}

VKAPI void hk_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pA) {
    DeviceData* dd = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_dev.find(Key(device));
        if (it != g_dev.end()) { dd = it->second; g_dev.erase(it); }
    }
    if (!dd) return;
    destroyDeviceRes(dd);
    PFN_vkDestroyDevice fn = dd->d.DestroyDevice;
    delete dd;
    fn(device, pA);
}

// ---------------------------------------------------------------- hook instance
VKAPI VkResult hk_CreateInstance(const VkInstanceCreateInfo* pCI, const VkAllocationCallbacks* pA, VkInstance* pInst) {
    LayerInstanceCreateInfo* chain = (LayerInstanceCreateInfo*)pCI->pNext;
    while (chain && !(chain->sType == STYPE_LOADER_INSTANCE && chain->function == LAYER_LINK_INFO))
        chain = (LayerInstanceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->gipa;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto createFn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!createFn) return VK_ERROR_INITIALIZATION_FAILED;

    // ekstensi debug (milik validation layer asli) kita tangani sendiri sebagai stub -> jangan diteruskan ke driver
    std::vector<const char*> exts;
    for (uint32_t i = 0; i < pCI->enabledExtensionCount; i++) {
        const char* e = pCI->ppEnabledExtensionNames[i];
        if (!strcmp(e, "VK_EXT_debug_utils") || !strcmp(e, "VK_EXT_debug_report") ||
            !strcmp(e, "VK_EXT_validation_features") || !strcmp(e, "VK_EXT_validation_flags")) continue;
        exts.push_back(e);
    }
    VkInstanceCreateInfo ci = *pCI;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    VkResult r = createFn(&ci, pA, pInst);
    if (r != VK_SUCCESS) return r;

    InstanceData* ii = new InstanceData();
    ii->inst = *pInst; ii->gipa = gipa;
    ii->DestroyInstance = (PFN_vkDestroyInstance)gipa(*pInst, "vkDestroyInstance");
    ii->CreateDevice = (PFN_vkCreateDevice)gipa(*pInst, "vkCreateDevice");
    ii->GetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*pInst, "vkGetPhysicalDeviceMemoryProperties");
    ii->GetSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(*pInst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    std::lock_guard<std::mutex> l(g_mu);
    g_inst[Key(*pInst)] = ii;
    LOGI("layer aktif (instance dibuat)");
    return VK_SUCCESS;
}

VKAPI void hk_DestroyInstance(VkInstance inst, const VkAllocationCallbacks* pA) {
    InstanceData* ii = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_inst.find(Key(inst));
        if (it != g_inst.end()) { ii = it->second; g_inst.erase(it); }
    }
    if (!ii) return;
    PFN_vkDestroyInstance fn = ii->DestroyInstance;
    delete ii;
    fn(inst, pA);
}

VKAPI VkResult hk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* pCI, const VkAllocationCallbacks* pA, VkDevice* pDev) {
    InstanceData* ii = getInst(phys);
    if (!ii) return VK_ERROR_INITIALIZATION_FAILED;
    LayerDeviceCreateInfo* chain = (LayerDeviceCreateInfo*)pCI->pNext;
    while (chain && !(chain->sType == STYPE_LOADER_DEVICE && chain->function == LAYER_LINK_INFO))
        chain = (LayerDeviceCreateInfo*)chain->pNext;
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->gipa;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->gdpa;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto createFn = (PFN_vkCreateDevice)gipa(ii->inst, "vkCreateDevice");
    if (!createFn) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = createFn(phys, pCI, pA, pDev);
    if (r != VK_SUCCESS) return r;

    DeviceData* dd = new DeviceData();
    dd->dev = *pDev; dd->phys = phys; dd->ii = ii; dd->d.gdpa = gdpa;
    bool ok = true;
#define X(n) dd->d.n = (PFN_vk##n)gdpa(*pDev, "vk" #n); if (!dd->d.n) { ok = false; LOGE("fungsi vk" #n " tidak ada"); }
    DEV_FUNCS(X)
#undef X
    dd->d.GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)gdpa(*pDev, "vkGetDeviceQueue2");
    dd->usable = ok && ii->GetMemProps != nullptr;
    if (ii->GetMemProps) ii->GetMemProps(phys, &dd->mem);
    std::lock_guard<std::mutex> l(g_mu);
    g_dev[Key(*pDev)] = dd;
    LOGI("device dibuat (usable=%d)", (int)dd->usable);
    return VK_SUCCESS;
}

// ---------------------------------------------------------------- stub debug (pengganti validation layer asli)
VKAPI VkResult st_CreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* p) { if (p) *p = (VkDebugUtilsMessengerEXT)(uintptr_t)1; return VK_SUCCESS; }
VKAPI void st_DestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*) {}
VKAPI void st_SubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*) {}
VKAPI VkResult st_SetDebugUtilsObjectNameEXT(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
VKAPI VkResult st_SetDebugUtilsObjectTagEXT(VkDevice, const VkDebugUtilsObjectTagInfoEXT*) { return VK_SUCCESS; }
VKAPI void st_QueueBeginDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}
VKAPI void st_QueueEndDebugUtilsLabelEXT(VkQueue) {}
VKAPI void st_QueueInsertDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}
VKAPI void st_CmdBeginDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI void st_CmdEndDebugUtilsLabelEXT(VkCommandBuffer) {}
VKAPI void st_CmdInsertDebugUtilsLabelEXT(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI VkResult st_CreateDebugReportCallbackEXT(VkInstance, const VkDebugReportCallbackCreateInfoEXT*, const VkAllocationCallbacks*, VkDebugReportCallbackEXT* p) { if (p) *p = (VkDebugReportCallbackEXT)(uintptr_t)1; return VK_SUCCESS; }
VKAPI void st_DestroyDebugReportCallbackEXT(VkInstance, VkDebugReportCallbackEXT, const VkAllocationCallbacks*) {}
VKAPI void st_DebugReportMessageEXT(VkInstance, VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t, int32_t, const char*, const char*) {}

#define STUBS(X) X(CreateDebugUtilsMessengerEXT) X(DestroyDebugUtilsMessengerEXT) X(SubmitDebugUtilsMessageEXT) \
 X(SetDebugUtilsObjectNameEXT) X(SetDebugUtilsObjectTagEXT) X(QueueBeginDebugUtilsLabelEXT) X(QueueEndDebugUtilsLabelEXT) \
 X(QueueInsertDebugUtilsLabelEXT) X(CmdBeginDebugUtilsLabelEXT) X(CmdEndDebugUtilsLabelEXT) X(CmdInsertDebugUtilsLabelEXT) \
 X(CreateDebugReportCallbackEXT) X(DestroyDebugReportCallbackEXT) X(DebugReportMessageEXT)

// ---------------------------------------------------------------- proc addr + export loader
EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);
EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);

static PFN_vkVoidFunction hookLookup(const char* name) {
#define HOOK(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)hk_##n;
    HOOK(CreateInstance) HOOK(DestroyInstance) HOOK(CreateDevice) HOOK(DestroyDevice)
    HOOK(GetDeviceQueue) HOOK(GetDeviceQueue2) HOOK(CreateSwapchainKHR) HOOK(DestroySwapchainKHR) HOOK(QueuePresentKHR)
#undef HOOK
#define STUB(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)st_##n;
    STUBS(STUB)
#undef STUB
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    return nullptr;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    DeviceData* dd = device ? getDev(device) : nullptr;
    // GetDeviceQueue2 cuma di-hook kalau driver punya
    if (!strcmp(name, "vkGetDeviceQueue2") && (!dd || !dd->d.GetDeviceQueue2)) return dd ? dd->d.gdpa(device, name) : nullptr;
    PFN_vkVoidFunction f = hookLookup(name);
    if (f) return f;
    return (dd && dd->d.gdpa) ? dd->d.gdpa(device, name) : nullptr;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    InstanceData* ii = instance ? getInst(instance) : nullptr;
    if (!strcmp(name, "vkGetDeviceQueue2") && ii) {
        PFN_vkVoidFunction n = ii->gipa(instance, name);
        return n ? (PFN_vkVoidFunction)hk_GetDeviceQueue2 : nullptr;
    }
    PFN_vkVoidFunction f = hookLookup(name);
    if (f) return f;
    return ii ? ii->gipa(instance, name) : nullptr;
}

// nama versi Android lama: <LayerName>GetInstanceProcAddr
EXPORT PFN_vkVoidFunction VKAPI_CALL VK_LAYER_KHRONOS_validationGetInstanceProcAddr(VkInstance i, const char* n) { return vkGetInstanceProcAddr(i, n); }
EXPORT PFN_vkVoidFunction VKAPI_CALL VK_LAYER_KHRONOS_validationGetDeviceProcAddr(VkDevice d, const char* n) { return vkGetDeviceProcAddr(d, n); }

EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(NegotiateLayerInterface* p) {
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    if (p->version > 2) p->version = 2;
    p->gipa = vkGetInstanceProcAddr; p->gdpa = vkGetDeviceProcAddr; p->physGpa = nullptr;
    return VK_SUCCESS;
}

static void fillLayerProps(VkLayerProperties* p) {
    memset(p, 0, sizeof(*p));
    strncpy(p->layerName, LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
    strncpy(p->description, "Nyushu frame generation layer", VK_MAX_DESCRIPTION_SIZE - 1);
    p->specVersion = VK_MAKE_VERSION(1, 1, 0); p->implementationVersion = 1;
}
EXPORT VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    if (!pCount) return VK_INCOMPLETE;
    if (!pProps) { *pCount = 1; return VK_SUCCESS; }
    if (*pCount < 1) return VK_INCOMPLETE;
    fillLayerProps(&pProps[0]); *pCount = 1; return VK_SUCCESS;
}
EXPORT VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount, VkLayerProperties* pProps) {
    return vkEnumerateInstanceLayerProperties(pCount, pProps);
}
EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* pCount, VkExtensionProperties* pProps) {
    if (!layer || strcmp(layer, LAYER_NAME)) return VK_ERROR_LAYER_NOT_PRESENT;
    static const VkExtensionProperties e[] = {
        {"VK_EXT_debug_utils", 2}, {"VK_EXT_debug_report", 10}, {"VK_EXT_validation_features", 5}};
    const uint32_t n = 3;
    if (!pProps) { *pCount = n; return VK_SUCCESS; }
    uint32_t c = std::min(*pCount, n);
    for (uint32_t i = 0; i < c; i++) pProps[i] = e[i];
    VkResult r = c < n ? VK_INCOMPLETE : VK_SUCCESS; *pCount = c; return r;
}
EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* layer, uint32_t* pCount, VkExtensionProperties*) {
    if (!layer || strcmp(layer, LAYER_NAME)) return VK_ERROR_LAYER_NOT_PRESENT;
    *pCount = 0; return VK_SUCCESS;
}
