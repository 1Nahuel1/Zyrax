// Zyrax Vulkan implicit layer (Linux)
// ------------------------------------
// Intercepts the swapchain and applies FSR 1.0 (EASU upscaling + RCAS
// sharpening) to every presented frame.
//
// When upscaling is on, the surface capabilities are reported at a reduced
// resolution so the application renders a smaller frame. vkCreateSwapchainKHR
// then creates the real (full-size) swapchain, and every present is copied into
// a scratch image and reconstructed to full resolution with EASU + RCAS before
// the normal present call. No second swapchain or extra acquire is required.
//
// The implicit layer is loaded by every Vulkan process once installed, but it
// only activates for the PID named in the control file written by the Zyrax
// app (or for every process when ZYRAX_FSR_ENABLE=1 is set manually).
//
// Environment variables:
//   ZYRAX_FSR_ENABLE      force-enable for this process (manual mode)
//   ZYRAX_FSR_SCALE       render scale (default 1.5, 1.0 disables upscaling)
//   ZYRAX_FSR_SHARPNESS   0.0 .. 1.0   (default 0.5)
//   ZYRAX_FSR_DISABLE     set to 1 to hard-disable the hooks
//   ZYRAX_FSR_CONTROL     control file path
//                          (default /tmp/zyrax_fsr_control)
//   ZYRAX_FSR_DEBUG       set to 1 for stderr logging
//   ZYRAX_FSR_STATS       stats output path
//                          (default /tmp/zyrax_fsr_stats.json)

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include "fsr_easu_frag.h"
#include "fsr_rcas_frag.h"
#include "rcas_vert.h"

// ---------------------------------------------------------------------------
// Loader interface plumbing
// ---------------------------------------------------------------------------

static PFN_vkGetInstanceProcAddr g_nextGIPA = nullptr;
static VkInstance g_instance = VK_NULL_HANDLE;
static PFN_vkGetDeviceProcAddr g_nextGDPA = nullptr;
static PFN_GetPhysicalDeviceProcAddr g_nextPDPA = nullptr;
static std::mutex g_mutex;

static PFN_vkGetInstanceProcAddr loaderGIPA() {
    static PFN_vkGetInstanceProcAddr fp = nullptr;
    if (!fp) {
        void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (h) fp = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    }
    return fp;
}

static PFN_vkGetDeviceProcAddr loaderGDPA() {
    static PFN_vkGetDeviceProcAddr fp = nullptr;
    if (!fp) {
        void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (h) fp = (PFN_vkGetDeviceProcAddr)dlsym(h, "vkGetDeviceProcAddr");
    }
    return fp;
}

static PFN_vkGetInstanceProcAddr chainGIPA() { return g_nextGIPA ? g_nextGIPA : loaderGIPA(); }
static PFN_vkGetDeviceProcAddr chainGDPA() { return g_nextGDPA ? g_nextGDPA : loaderGDPA(); }

static bool hooksEnabled() {
    const char* dis = getenv("ZYRAX_FSR_DISABLE");
    return !(dis && dis[0] == '1');
}

static bool debugEnabled() {
    const char* d = getenv("ZYRAX_FSR_DEBUG");
    return d && d[0] == '1';
}

#define ZLOG(...)                            \
    do {                                     \
        if (debugEnabled()) {                \
            fprintf(stderr, "[zyrax-fsr] "); \
            fprintf(stderr, __VA_ARGS__);    \
            fprintf(stderr, "\n");           \
        }                                    \
    } while (0)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct PushConstants {
    uint32_t con0[4];
    uint32_t con1[4];
    uint32_t con2[4];
    uint32_t con3[4];
    uint32_t rcas[4];
    uint32_t ui[4];
};

struct QueueState {
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

// Wayland reports currentExtent == UINT32_MAX, so the layer cannot know the
// window size until the application creates its first swapchain. We remember
// that size, report a reduced extent afterwards and ask the app to recreate
// once (VK_ERROR_OUT_OF_DATE_KHR) so it starts rendering at the lower size.
struct SurfaceState {
    bool learned = false;
    bool disabled = false;
    bool restore = false;
    VkExtent2D outputExtent{0, 0};
    uint32_t recreateAttempts = 0;
    uint32_t recreateBurst = 0;
    double lastRecreate = 0.0;
};
static std::unordered_map<VkSurfaceKHR, SurfaceState> g_surfaces;

struct SwapchainState {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkExtent2D extent{0, 0};
    VkExtent2D renderExtent{0, 0};
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool upscale = false;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
    bool enabled = false;
    bool failed = false;
    bool resourcesReady = false;
    bool forceRecreate = false;

    VkImage scratch = VK_NULL_HANDLE;
    VkDeviceMemory scratchMem = VK_NULL_HANDLE;
    VkImageView scratchView = VK_NULL_HANDLE;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkImage intermediate = VK_NULL_HANDLE;
    VkDeviceMemory intermediateMem = VK_NULL_HANDLE;
    VkImageView intermediateView = VK_NULL_HANDLE;
    VkFramebuffer easuFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet easuDesc = VK_NULL_HANDLE;
    VkPipeline easuPipeline = VK_NULL_HANDLE;
    PushConstants pc{};
};

struct DeviceState {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint64_t controlSerial = 0;
    bool activeLast = false;

    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;

    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdSetViewport CmdSetViewport = nullptr;
    PFN_vkCmdSetScissor CmdSetScissor = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets FreeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;

    bool commonReady = false;
    bool commonFailed = false;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkShaderModule easuModule = VK_NULL_HANDLE;

    float sharpness = 0.5f;
    uint64_t presents = 0;
    uint32_t fpsFrames = 0;
    float fps = 0.0f;
    double fpsStart = 0.0;
    double lastStatsWrite = 0.0;

    std::unordered_map<VkQueue, QueueState> queues;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
};

static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_nextSurfaceCaps = nullptr;
static PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR g_nextSurfaceCaps2 = nullptr;
static PFN_vkGetPhysicalDeviceFormatProperties g_formatProps = nullptr;
static PFN_vkDestroySurfaceKHR g_nextDestroySurface = nullptr;

static std::unordered_map<VkDevice, DeviceState*> g_devices;
static std::unordered_map<VkQueue, DeviceState*> g_queueDevice;
static std::unordered_map<VkQueue, uint32_t> g_queueFamily;

static DeviceState* deviceFor(VkDevice device) {
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

static double nowSeconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct ControlState {
    bool enabled = false;
    long pid = -1;
    float scale = 1.5f;
    float sharpness = 0.5f;
    bool overlay = false;
    bool frameGen = false;
    uint64_t serial = 0;
    double lastRead = -1.0;
};
static ControlState g_control;
static std::mutex g_controlMutex;

static const char* controlPath() {
    const char* p = getenv("ZYRAX_FSR_CONTROL");
    return p && p[0] ? p : "/tmp/zyrax_fsr_control";
}

static const char* statsPath() {
    const char* p = getenv("ZYRAX_FSR_STATS");
    return p && p[0] ? p : "/tmp/zyrax_fsr_stats.json";
}

static char* trimInPlace(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    return s;
}

static void refreshControl() {
    std::lock_guard<std::mutex> lk(g_controlMutex);
    double now = nowSeconds();
    if (now - g_control.lastRead < 0.25) return;

    ControlState c;
    c.serial = g_control.serial;
    c.lastRead = now;
    FILE* f = fopen(controlPath(), "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* hash = strchr(line, '#');
            if (hash) *hash = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* key = trimInPlace(line);
            char* value = trimInPlace(eq + 1);
            if (!strcmp(key, "enabled")) c.enabled = atoi(value) != 0;
            else if (!strcmp(key, "pid")) c.pid = atol(value);
            else if (!strcmp(key, "scale")) c.scale = (float)atof(value);
            else if (!strcmp(key, "sharpness")) c.sharpness = (float)atof(value);
            else if (!strcmp(key, "overlay")) c.overlay = atoi(value) != 0;
            else if (!strcmp(key, "frame_gen")) c.frameGen = atoi(value) != 0;
        }
        fclose(f);
    }
    if (!std::isfinite(c.scale)) c.scale = 1.5f;
    c.scale = std::clamp(c.scale, 1.0f, 4.0f);
    if (!std::isfinite(c.sharpness)) c.sharpness = 0.5f;
    c.sharpness = std::clamp(c.sharpness, 0.0f, 1.0f);

    bool pipelineChanged = c.enabled != g_control.enabled || c.pid != g_control.pid ||
                           fabsf(c.scale - g_control.scale) > 1e-4f ||
                           fabsf(c.sharpness - g_control.sharpness) > 1e-4f;
    bool anyChanged = pipelineChanged || c.overlay != g_control.overlay ||
                      c.frameGen != g_control.frameGen;
    if (pipelineChanged) ++c.serial;
    if (anyChanged) {
        g_control = c;
    } else {
        g_control.lastRead = now;
    }
}

static ControlState snapshotControl() {
    refreshControl();
    std::lock_guard<std::mutex> lk(g_controlMutex);
    return g_control;
}

static bool processActiveFor(const ControlState& c) {
    if (!hooksEnabled()) return false;
    const char* en = getenv("ZYRAX_FSR_ENABLE");
    if (en && en[0] == '1') return true;
    return c.enabled && c.pid > 0 && (long)getpid() == c.pid;
}

static bool processActive() { return processActiveFor(snapshotControl()); }

static float readScale() {
    const char* s = getenv("ZYRAX_FSR_SCALE");
    if (s && s[0]) {
        float v = (float)atof(s);
        if (!std::isfinite(v)) v = 1.5f;
        return std::clamp(v, 1.0f, 4.0f);
    }
    return std::clamp(snapshotControl().scale, 1.0f, 4.0f);
}

static float readSharpness() {
    const char* s = getenv("ZYRAX_FSR_SHARPNESS");
    if (s && s[0]) {
        float v = (float)atof(s);
        if (!std::isfinite(v)) v = 0.5f;
        return std::clamp(v, 0.0f, 1.0f);
    }
    return std::clamp(snapshotControl().sharpness, 0.0f, 1.0f);
}

static bool upscaleEnabled() { return processActive() && readScale() > 1.001f; }

static bool overlayEnabled() {
    const char* e = getenv("ZYRAX_FSR_OVERLAY");
    if (e && e[0] == '1') return true;
    return snapshotControl().overlay;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static VkLayerInstanceCreateInfo* getInstanceChainInfo(const VkInstanceCreateInfo* ci, VkLayerFunction fn) {
    auto* info = (VkLayerInstanceCreateInfo*)ci->pNext;
    while (info && !(info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && info->function == fn))
        info = (VkLayerInstanceCreateInfo*)info->pNext;
    return info;
}

static VkLayerDeviceCreateInfo* getDeviceChainInfo(const VkDeviceCreateInfo* ci, VkLayerFunction fn) {
    auto* info = (VkLayerDeviceCreateInfo*)ci->pNext;
    while (info && !(info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && info->function == fn))
        info = (VkLayerDeviceCreateInfo*)info->pNext;
    return info;
}

static uint32_t findMemoryType(DeviceState* ds, uint32_t bits, VkMemoryPropertyFlags props) {
    if (!ds->GetPhysicalDeviceMemoryProperties) return UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mp;
    ds->GetPhysicalDeviceMemoryProperties(ds->gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

static VkImage createImage(DeviceState* ds, VkFormat fmt, VkExtent2D ext, VkImageUsageFlags usage,
                           VkDeviceMemory* outMem) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {ext.width, ext.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img = VK_NULL_HANDLE;
    if (ds->CreateImage(ds->device, &ci, nullptr, &img) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkMemoryRequirements req;
    ds->GetImageMemoryRequirements(ds->device, img, &req);
    uint32_t type = findMemoryType(ds, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        ds->DestroyImage(ds->device, img, nullptr);
        return VK_NULL_HANDLE;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (ds->AllocateMemory(ds->device, &ai, nullptr, &mem) != VK_SUCCESS) {
        ds->DestroyImage(ds->device, img, nullptr);
        return VK_NULL_HANDLE;
    }
    if (ds->BindImageMemory(ds->device, img, mem, 0) != VK_SUCCESS) {
        ds->FreeMemory(ds->device, mem, nullptr);
        ds->DestroyImage(ds->device, img, nullptr);
        return VK_NULL_HANDLE;
    }
    *outMem = mem;
    return img;
}

static VkImageView createView(DeviceState* ds, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (ds->CreateImageView(ds->device, &ci, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static void imageBarrier(DeviceState* ds, VkCommandBuffer cb, VkImage img, VkImageLayout oldL, VkImageLayout newL) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ds->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
}

static void copyImage(DeviceState* ds, VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                      VkImageLayout dstL, VkExtent2D ext) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {ext.width, ext.height, 1};
    ds->CmdCopyImage(cb, src, srcL, dst, dstL, 1, &region);
}

static VkShaderModule makeModule(DeviceState* ds, const unsigned char* spv, unsigned int len) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = len;
    ci.pCode = reinterpret_cast<const uint32_t*>(spv);
    VkShaderModule mod = VK_NULL_HANDLE;
    if (ds->CreateShaderModule(ds->device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
    return mod;
}

static bool ensureCommon(DeviceState* ds) {
    if (ds->commonReady) return true;
    if (ds->commonFailed) return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslCi.bindingCount = 1;
    dslCi.pBindings = &binding;
    if (ds->CreateDescriptorSetLayout(ds->device, &dslCi, nullptr, &ds->dsl) != VK_SUCCESS) {
        ds->commonFailed = true;
        return false;
    }

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCi.maxSets = 64;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &size;
    if (ds->CreateDescriptorPool(ds->device, &poolCi, nullptr, &ds->descPool) != VK_SUCCESS) {
        ds->commonFailed = true;
        return false;
    }

    VkSamplerCreateInfo sCi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sCi.magFilter = VK_FILTER_LINEAR;
    sCi.minFilter = VK_FILTER_LINEAR;
    sCi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sCi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sCi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sCi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (ds->CreateSampler(ds->device, &sCi, nullptr, &ds->sampler) != VK_SUCCESS) {
        ds->commonFailed = true;
        return false;
    }

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &ds->dsl;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges = &pc;
    if (ds->CreatePipelineLayout(ds->device, &plCi, nullptr, &ds->pipeLayout) != VK_SUCCESS) {
        ds->commonFailed = true;
        return false;
    }

    ds->vertModule = makeModule(ds, rcas_vert_spv, rcas_vert_spv_len);
    ds->fragModule = makeModule(ds, fsr_rcas_frag_spv, fsr_rcas_frag_spv_len);
    ds->easuModule = makeModule(ds, fsr_easu_frag_spv, fsr_easu_frag_spv_len);
    if (!ds->vertModule || !ds->fragModule || !ds->easuModule) {
        ds->commonFailed = true;
        return false;
    }

    ds->sharpness = readSharpness();
    ds->commonReady = true;
    ZLOG("common resources ready (sharpness=%.2f)", ds->sharpness);
    return true;
}

static bool createGraphicsPipeline(DeviceState* ds, SwapchainState* sc, VkShaderModule fragModule,
                                   VkPipeline* outPipeline) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ds->vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ias;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dyn;
    gp.layout = ds->pipeLayout;
    gp.renderPass = sc->renderPass;
    gp.subpass = 0;

    return ds->CreateGraphicsPipelines(ds->device, VK_NULL_HANDLE, 1, &gp, nullptr, outPipeline) == VK_SUCCESS;
}

static bool buildSwapchainPipeline(DeviceState* ds, SwapchainState* sc) {
    VkAttachmentDescription color{};
    color.format = sc->format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpCi.attachmentCount = 1;
    rpCi.pAttachments = &color;
    rpCi.subpassCount = 1;
    rpCi.pSubpasses = &subpass;
    rpCi.dependencyCount = 1;
    rpCi.pDependencies = &dep;
    if (ds->CreateRenderPass(ds->device, &rpCi, nullptr, &sc->renderPass) != VK_SUCCESS) return false;

    if (!createGraphicsPipeline(ds, sc, ds->fragModule, &sc->pipeline)) return false;
    if (sc->upscale && !createGraphicsPipeline(ds, sc, ds->easuModule, &sc->easuPipeline)) return false;
    return true;
}

static bool allocateDescriptor(DeviceState* ds, VkDescriptorSet* set) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = ds->descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &ds->dsl;
    return ds->AllocateDescriptorSets(ds->device, &ai, set) == VK_SUCCESS;
}

static void writeDescriptorImage(DeviceState* ds, VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = ds->sampler;
    srcInfo.imageView = view;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &srcInfo;
    ds->UpdateDescriptorSets(ds->device, 1, &write, 0, nullptr);
}

static bool ensureSwapchainResources(DeviceState* ds, SwapchainState* sc) {
    if (sc->resourcesReady) return true;
    if (sc->failed || !sc->enabled) return false;
    if (!ensureCommon(ds)) return false;

    if (sc->renderExtent.width == 0 || sc->renderExtent.height == 0) sc->renderExtent = sc->extent;

    sc->scratch = createImage(ds, sc->format, sc->renderExtent,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &sc->scratchMem);
    if (!sc->scratch) {
        sc->failed = true;
        return false;
    }
    sc->scratchView = createView(ds, sc->scratch, sc->format);
    if (!sc->scratchView) {
        sc->failed = true;
        return false;
    }

    if (sc->upscale) {
        sc->intermediate =
            createImage(ds, sc->format, sc->extent,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &sc->intermediateMem);
        if (!sc->intermediate) {
            sc->failed = true;
            return false;
        }
        sc->intermediateView = createView(ds, sc->intermediate, sc->format);
        if (!sc->intermediateView) {
            sc->failed = true;
            return false;
        }
    }

    if (!buildSwapchainPipeline(ds, sc)) {
        sc->failed = true;
        return false;
    }

    sc->imageViews.resize(sc->images.size());
    sc->framebuffers.resize(sc->images.size());
    for (size_t i = 0; i < sc->images.size(); ++i) {
        sc->imageViews[i] = createView(ds, sc->images[i], sc->format);
        if (!sc->imageViews[i]) {
            sc->failed = true;
            return false;
        }
        VkFramebufferCreateInfo fbCi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCi.renderPass = sc->renderPass;
        fbCi.attachmentCount = 1;
        fbCi.pAttachments = &sc->imageViews[i];
        fbCi.width = sc->extent.width;
        fbCi.height = sc->extent.height;
        fbCi.layers = 1;
        if (ds->CreateFramebuffer(ds->device, &fbCi, nullptr, &sc->framebuffers[i]) != VK_SUCCESS) {
            sc->failed = true;
            return false;
        }
    }

    if (sc->upscale) {
        VkFramebufferCreateInfo fbCi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCi.renderPass = sc->renderPass;
        fbCi.attachmentCount = 1;
        fbCi.pAttachments = &sc->intermediateView;
        fbCi.width = sc->extent.width;
        fbCi.height = sc->extent.height;
        fbCi.layers = 1;
        if (ds->CreateFramebuffer(ds->device, &fbCi, nullptr, &sc->easuFramebuffer) != VK_SUCCESS) {
            sc->failed = true;
            return false;
        }
    }

    if (!allocateDescriptor(ds, &sc->desc)) {
        sc->failed = true;
        return false;
    }
    writeDescriptorImage(ds, sc->desc, sc->upscale ? sc->intermediateView : sc->scratchView);
    if (sc->upscale) {
        if (!allocateDescriptor(ds, &sc->easuDesc)) {
            sc->failed = true;
            return false;
        }
        writeDescriptorImage(ds, sc->easuDesc, sc->scratchView);
    }

    AF1 inW = (AF1)sc->renderExtent.width;
    AF1 inH = (AF1)sc->renderExtent.height;
    FsrEasuCon(sc->pc.con0, sc->pc.con1, sc->pc.con2, sc->pc.con3, inW, inH, inW, inH,
               (AF1)sc->extent.width, (AF1)sc->extent.height);
    AF1 stops = (AF1)((1.0f - ds->sharpness) * 2.0f);
    FsrRcasCon(sc->pc.rcas, stops);

    sc->resourcesReady = true;
    ZLOG("swapchain resources ready %ux%u <- %ux%u upscale=%d format=%d", sc->extent.width,
         sc->extent.height, sc->renderExtent.width, sc->renderExtent.height, (int)sc->upscale,
         (int)sc->format);
    return true;
}

static QueueState* ensureQueue(DeviceState* ds, VkQueue queue) {
    auto it = ds->queues.find(queue);
    if (it != ds->queues.end() && it->second.pool != VK_NULL_HANDLE) return &it->second;

    auto fit = g_queueFamily.find(queue);
    if (fit == g_queueFamily.end()) return nullptr;

    QueueState qs;
    qs.family = fit->second;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = qs.family;
    if (ds->CreateCommandPool(ds->device, &pc, nullptr, &qs.pool) != VK_SUCCESS) return nullptr;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = qs.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (ds->AllocateCommandBuffers(ds->device, &ai, &qs.cmd) != VK_SUCCESS) {
        ds->DestroyCommandPool(ds->device, qs.pool, nullptr);
        return nullptr;
    }

    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (ds->CreateFence(ds->device, &fc, nullptr, &qs.fence) != VK_SUCCESS) {
        ds->DestroyCommandPool(ds->device, qs.pool, nullptr);
        return nullptr;
    }

    auto [pos, _] = ds->queues.emplace(queue, qs);
    return &pos->second;
}

static void drawFullscreenPass(DeviceState* ds, VkCommandBuffer cb, SwapchainState* sc, VkFramebuffer fb,
                               VkPipeline pipeline, VkDescriptorSet set) {
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = sc->renderPass;
    rp.framebuffer = fb;
    rp.renderArea = {{0, 0}, sc->extent};
    ds->CmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ds->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ds->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ds->pipeLayout, 0, 1, &set, 0, nullptr);
    ds->CmdPushConstants(cb, ds->pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sc->pc), &sc->pc);

    VkViewport vp{0, 0, (float)sc->extent.width, (float)sc->extent.height, 0.0f, 1.0f};
    VkRect2D scc{{0, 0}, sc->extent};
    ds->CmdSetViewport(cb, 0, 1, &vp);
    ds->CmdSetScissor(cb, 0, 1, &scc);
    ds->CmdDraw(cb, 3, 1, 0, 0);
    ds->CmdEndRenderPass(cb);
}

static void processFrame(DeviceState* ds, SwapchainState* sc, VkQueue queue, uint32_t index) {
    if (index >= sc->images.size()) return;
    if (!ensureSwapchainResources(ds, sc)) {
        if (sc->enabled) {
            // Never leave the application rendering a reduced region into a
            // full-size swapchain. Disable FSR for this surface and ask for a
            // single recreate at the native size after the present.
            if (sc->surface) {
                g_surfaces[sc->surface].disabled = true;
                g_surfaces[sc->surface].restore = true;
            }
            sc->enabled = false;
            sc->forceRecreate = true;
            ZLOG("disabling FSR after resource failure");
        }
        return;
    }
    QueueState* qs = ensureQueue(ds, queue);
    if (!qs) return;

    sc->pc.ui[0] = overlayEnabled() ? 1u : 0u;
    sc->pc.ui[1] = (uint32_t)(ds->fps + 0.5f);
    sc->pc.ui[2] = (uint32_t)(readScale() * 100.0f + 0.5f);
    sc->pc.ui[3] = sc->extent.height;

    VkImage swImg = sc->images[index];
    VkFramebuffer fb = sc->framebuffers[index];

    ds->ResetCommandBuffer(qs->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (ds->BeginCommandBuffer(qs->cmd, &bi) != VK_SUCCESS) return;
    VkCommandBuffer cb = qs->cmd;

    // Copy the presented frame (game resolution) into our scratch image.
    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    imageBarrier(ds, cb, sc->scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyImage(ds, cb, swImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->scratch,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sc->renderExtent);
    imageBarrier(ds, cb, sc->scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (sc->upscale) {
        // EASU: low-res scratch -> full-res intermediate.
        imageBarrier(ds, cb, sc->intermediate, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        drawFullscreenPass(ds, cb, sc, sc->easuFramebuffer, sc->easuPipeline, sc->easuDesc);
        imageBarrier(ds, cb, sc->intermediate, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // RCAS: sharpen (and for the non-upscale path, read the scratch directly).
    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    drawFullscreenPass(ds, cb, sc, fb, sc->pipeline, sc->desc);
    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    if (ds->EndCommandBuffer(cb) != VK_SUCCESS) return;

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ds->ResetFences(ds->device, 1, &qs->fence);
    if (ds->QueueSubmit(queue, 1, &si, qs->fence) != VK_SUCCESS) return;
    ds->WaitForFences(ds->device, 1, &qs->fence, VK_TRUE, UINT64_MAX);

    ds->presents++;
}

static void writeStats(DeviceState* ds) {
    double now = nowSeconds();
    if (now - ds->lastStatsWrite < 0.25) return;
    ds->lastStatsWrite = now;

    const char* path = statsPath();
    FILE* f = fopen(path, "w");
    if (!f) return;

    VkExtent2D ext{0, 0};
    VkExtent2D renderExt{0, 0};
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    bool upscale = false;
    bool found = false;
    for (auto& kv : ds->swapchains) {
        if (kv.second.enabled && !kv.second.failed) {
            ext = kv.second.extent;
            renderExt = kv.second.renderExtent;
            fmt = kv.second.format;
            upscale = kv.second.upscale;
            found = true;
            break;
        }
    }
    fprintf(f,
            "{\"enabled\":%s,\"presents\":%llu,\"sharpness\":%.3f,"
            "\"width\":%u,\"height\":%u,\"renderWidth\":%u,\"renderHeight\":%u,"
            "\"upscale\":%s,\"scale\":%.3f,\"format\":%d,\"pid\":%d,\"timestamp\":%lld}\n",
            found ? "true" : "false", (unsigned long long)ds->presents, ds->sharpness, ext.width,
            ext.height, renderExt.width, renderExt.height, upscale ? "true" : "false",
            renderExt.width ? (float)ext.width / (float)renderExt.width : 1.0f, (int)fmt,
            (int)getpid(), (long long)time(nullptr));
    fclose(f);
}

static void destroySwapchainResources(DeviceState* ds, SwapchainState* sc) {
    for (auto fb : sc->framebuffers)
        if (fb) ds->DestroyFramebuffer(ds->device, fb, nullptr);
    for (auto v : sc->imageViews)
        if (v) ds->DestroyImageView(ds->device, v, nullptr);
    sc->framebuffers.clear();
    sc->imageViews.clear();

    if (sc->desc && ds->FreeDescriptorSets)
        ds->FreeDescriptorSets(ds->device, ds->descPool, 1, &sc->desc);
    if (sc->easuDesc && ds->FreeDescriptorSets)
        ds->FreeDescriptorSets(ds->device, ds->descPool, 1, &sc->easuDesc);
    sc->desc = VK_NULL_HANDLE;
    sc->easuDesc = VK_NULL_HANDLE;
    if (sc->easuFramebuffer) ds->DestroyFramebuffer(ds->device, sc->easuFramebuffer, nullptr);
    if (sc->easuPipeline) ds->DestroyPipeline(ds->device, sc->easuPipeline, nullptr);
    if (sc->pipeline) ds->DestroyPipeline(ds->device, sc->pipeline, nullptr);
    if (sc->renderPass) ds->DestroyRenderPass(ds->device, sc->renderPass, nullptr);
    if (sc->intermediateView) ds->DestroyImageView(ds->device, sc->intermediateView, nullptr);
    if (sc->intermediate) ds->DestroyImage(ds->device, sc->intermediate, nullptr);
    if (sc->intermediateMem) ds->FreeMemory(ds->device, sc->intermediateMem, nullptr);
    if (sc->scratchView) ds->DestroyImageView(ds->device, sc->scratchView, nullptr);
    if (sc->scratch) ds->DestroyImage(ds->device, sc->scratch, nullptr);
    if (sc->scratchMem) ds->FreeMemory(ds->device, sc->scratchMem, nullptr);

    sc->easuFramebuffer = VK_NULL_HANDLE;
    sc->easuPipeline = VK_NULL_HANDLE;
    sc->pipeline = VK_NULL_HANDLE;
    sc->renderPass = VK_NULL_HANDLE;
    sc->intermediateView = VK_NULL_HANDLE;
    sc->intermediate = VK_NULL_HANDLE;
    sc->intermediateMem = VK_NULL_HANDLE;
    sc->scratchView = VK_NULL_HANDLE;
    sc->scratch = VK_NULL_HANDLE;
    sc->scratchMem = VK_NULL_HANDLE;
    sc->resourcesReady = false;
}

// ---------------------------------------------------------------------------
// Surface capabilities: report a reduced size so the game renders lower-res
// ---------------------------------------------------------------------------

static VkExtent2D reducedExtent(VkExtent2D output, float scale) {
    VkExtent2D r{1, 1};
    r.width = (uint32_t)((float)output.width / scale + 0.5f);
    r.height = (uint32_t)((float)output.height / scale + 0.5f);
    if (r.width < 1) r.width = 1;
    if (r.height < 1) r.height = 1;
    return r;
}

static bool extentNear(VkExtent2D a, VkExtent2D b) {
    int dw = (int)a.width - (int)b.width;
    int dh = (int)a.height - (int)b.height;
    if (dw < 0) dw = -dw;
    if (dh < 0) dh = -dh;
    return dw <= 2 && dh <= 2;
}

// Wayland reports currentExtent=UINT32_MAX and applications remember the last
// concrete extent they saw. After detach (or when FSR turns itself off) that
// remembered extent is the reduced one, so report the learned output size once
// more. Otherwise the game window would stay at the smaller size.
static void reportFullExtent(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    if (caps->currentExtent.width != UINT32_MAX) return;
    VkExtent2D output{};
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_surfaces.find(surface);
        if (it == g_surfaces.end() || !it->second.learned || !it->second.restore) return;
        output = it->second.outputExtent;
    }
    caps->minImageExtent = {1, 1};
    if (caps->maxImageExtent.width < output.width) caps->maxImageExtent.width = output.width;
    if (caps->maxImageExtent.height < output.height) caps->maxImageExtent.height = output.height;
    caps->currentExtent = output;
    ZLOG("caps hook: restore full output %ux%u", output.width, output.height);
}

static void applyUpscaleExtent(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    ZLOG("caps hook: current=%ux%u min=%ux%u max=%ux%u scale=%.2f up=%d", caps->currentExtent.width,
         caps->currentExtent.height, caps->minImageExtent.width, caps->minImageExtent.height,
         caps->maxImageExtent.width, caps->maxImageExtent.height, (double)readScale(),
         (int)upscaleEnabled());
    if (!processActive()) {
        reportFullExtent(surface, caps);
        return;
    }
    if (!(caps->supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        ZLOG("caps hook: surface cannot be a transfer source, not reducing");
        reportFullExtent(surface, caps);
        return;
    }
    if (!upscaleEnabled()) {
        reportFullExtent(surface, caps);
        return;
    }

    VkExtent2D output = caps->currentExtent;
    bool disabled = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_surfaces.find(surface);
        disabled = it != g_surfaces.end() && it->second.disabled;
        if (!disabled && (output.width == UINT32_MAX || output.height == UINT32_MAX)) {
            if (it == g_surfaces.end() || !it->second.learned) return;
            output = it->second.outputExtent;
        }
    }
    if (disabled) {
        reportFullExtent(surface, caps);
        return;
    }

    VkExtent2D lower = reducedExtent(output, readScale());
    if (lower.width >= output.width || lower.height >= output.height) {
        reportFullExtent(surface, caps);
        return;
    }

    // Report a smaller surface so the application renders fewer pixels. The
    // real swapchain is still created at the true size in vkCreateSwapchainKHR.
    caps->minImageExtent = {1, 1};
    if (caps->maxImageExtent.width < lower.width) caps->maxImageExtent.width = lower.width;
    if (caps->maxImageExtent.height < lower.height) caps->maxImageExtent.height = lower.height;
    caps->currentExtent = lower;
    ZLOG("caps hook: output %ux%u reported %ux%u", output.width, output.height, lower.width,
         lower.height);
}

static VKAPI_ATTR VkResult VKAPI_CALL
LayerGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                             VkSurfaceCapabilitiesKHR* caps) {
    if (!g_nextSurfaceCaps) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = g_nextSurfaceCaps(gpu, surface, caps);
    if (r == VK_SUCCESS && caps) applyUpscaleExtent(surface, caps);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
LayerGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice gpu, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                              VkSurfaceCapabilities2KHR* caps) {
    if (!g_nextSurfaceCaps2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = g_nextSurfaceCaps2(gpu, info, caps);
    if (r == VK_SUCCESS && caps) applyUpscaleExtent(info->surface, &caps->surfaceCapabilities);
    return r;
}

// ---------------------------------------------------------------------------
// Layer entry points
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                          const VkAllocationCallbacks* pAllocator,
                                                          VkInstance* pInstance);
static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
static VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateSwapchainKHR(VkDevice device,
                                                              const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                              const VkAllocationCallbacks* pAllocator,
                                                              VkSwapchainKHR* pSwapchain);
static VKAPI_ATTR void VKAPI_CALL LayerDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks* pAllocator);
static VKAPI_ATTR VkResult VKAPI_CALL LayerQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                               const char* pName);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = vkGetPhysicalDeviceProcAddr;
    }
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                               const char* pName) {
    if (pName) {
        if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return (PFN_vkVoidFunction)LayerGetPhysicalDeviceSurfaceCapabilitiesKHR;
        if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
            return (PFN_vkVoidFunction)LayerGetPhysicalDeviceSurfaceCapabilities2KHR;
    }
    if (g_nextPDPA) return (PFN_vkVoidFunction)g_nextPDPA(instance, pName);
    auto gipa = chainGIPA();
    return gipa ? (PFN_vkVoidFunction)gipa(instance, pName) : nullptr;
}

static VKAPI_ATTR void VKAPI_CALL LayerDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                          const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_surfaces.erase(surface);
    }
    if (!g_nextDestroySurface) {
        auto gipa = chainGIPA();
        if (gipa) g_nextDestroySurface = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
    }
    if (g_nextDestroySurface) g_nextDestroySurface(instance, surface, pAllocator);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkGetPhysicalDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetPhysicalDeviceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)LayerCreateInstance;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)LayerCreateDevice;
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return (PFN_vkVoidFunction)LayerGetPhysicalDeviceSurfaceCapabilitiesKHR;
    if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
        return (PFN_vkVoidFunction)LayerGetPhysicalDeviceSurfaceCapabilities2KHR;
    if (!strcmp(pName, "vkDestroySurfaceKHR")) return (PFN_vkVoidFunction)LayerDestroySurfaceKHR;

    auto gipa = chainGIPA();
    return gipa ? (PFN_vkVoidFunction)gipa(instance, pName) : nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)LayerCreateSwapchainKHR;
    if (!strcmp(pName, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)LayerDestroySwapchainKHR;
    if (!strcmp(pName, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)LayerQueuePresentKHR;
    if (!strcmp(pName, "vkDestroyDevice")) return (PFN_vkVoidFunction)LayerDestroyDevice;

    auto gdpa = chainGDPA();
    return gdpa ? (PFN_vkVoidFunction)gdpa(device, pName) : nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                          const VkAllocationCallbacks* pAllocator,
                                                          VkInstance* pInstance) {
    VkLayerInstanceCreateInfo* chain = getInstanceChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGIPA = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    g_nextPDPA = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    auto fpCreateInstance = (PFN_vkCreateInstance)fpGIPA(nullptr, "vkCreateInstance");
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    if (!fpCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS) {
        g_nextGIPA = fpGIPA;
        g_instance = *pInstance;
        g_nextSurfaceCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)fpGIPA(
            *pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        g_nextSurfaceCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)fpGIPA(
            *pInstance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        g_formatProps = (PFN_vkGetPhysicalDeviceFormatProperties)fpGIPA(
            *pInstance, "vkGetPhysicalDeviceFormatProperties");
        g_nextDestroySurface =
            (PFN_vkDestroySurfaceKHR)fpGIPA(*pInstance, "vkDestroySurfaceKHR");
        ZLOG("instance created");
    }
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* chain = getDeviceChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr fpGIPA = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGDPA = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto fpCreateDevice = (PFN_vkCreateDevice)fpGIPA(nullptr, "vkCreateDevice");
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    if (!fpCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    g_nextGDPA = fpGDPA;
    g_nextGIPA = fpGIPA;

    if (!hooksEnabled()) {
        ZLOG("layer hooks disabled by environment");
        return VK_SUCCESS;
    }

    ControlState initial = snapshotControl();
    auto* ds = new DeviceState();
    ds->gpu = gpu;
    ds->device = *pDevice;
    ds->sharpness = readSharpness();
    ds->controlSerial = initial.serial;
    ds->fpsStart = nowSeconds();

#define LOAD(n) ds->n = (PFN_vk##n)fpGDPA(*pDevice, "vk" #n)
    LOAD(CreateSwapchainKHR);
    LOAD(DestroySwapchainKHR);
    LOAD(QueuePresentKHR);
    LOAD(GetSwapchainImagesKHR);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(ResetCommandBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdCopyImage);
    LOAD(CmdBeginRenderPass);
    LOAD(CmdEndRenderPass);
    LOAD(CmdSetViewport);
    LOAD(CmdSetScissor);
    LOAD(CmdBindPipeline);
    LOAD(CmdBindDescriptorSets);
    LOAD(CmdPushConstants);
    LOAD(CmdDraw);
    LOAD(QueueSubmit);
    LOAD(CreateFence);
    LOAD(DestroyFence);
    LOAD(WaitForFences);
    LOAD(ResetFences);
    LOAD(CreateImage);
    LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindImageMemory);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(CreateSampler);
    LOAD(DestroySampler);
    LOAD(CreateDescriptorSetLayout);
    LOAD(DestroyDescriptorSetLayout);
    LOAD(CreateDescriptorPool);
    LOAD(DestroyDescriptorPool);
    LOAD(AllocateDescriptorSets);
    LOAD(FreeDescriptorSets);
    LOAD(UpdateDescriptorSets);
    LOAD(CreateShaderModule);
    LOAD(DestroyShaderModule);
    LOAD(CreatePipelineLayout);
    LOAD(DestroyPipelineLayout);
    LOAD(CreateGraphicsPipelines);
    LOAD(DestroyPipeline);
    LOAD(CreateRenderPass);
    LOAD(DestroyRenderPass);
    LOAD(CreateFramebuffer);
    LOAD(DestroyFramebuffer);
    LOAD(GetDeviceQueue);
#undef LOAD

    // vkGetPhysicalDeviceMemoryProperties is an instance-level entry point.
    ds->GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)fpGIPA(g_instance, "vkGetPhysicalDeviceMemoryProperties");

    if (!ds->CreateSwapchainKHR || !ds->QueuePresentKHR || !ds->GetSwapchainImagesKHR) {
        ZLOG("required device functions missing; disabling");
        delete ds;
        return VK_SUCCESS;
    }

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_devices[*pDevice] = ds;
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const auto& qci = pCreateInfo->pQueueCreateInfos[i];
            for (uint32_t q = 0; q < qci.queueCount; ++q) {
                VkQueue vq = VK_NULL_HANDLE;
                ds->GetDeviceQueue(*pDevice, qci.queueFamilyIndex, q, &vq);
                if (vq != VK_NULL_HANDLE) {
                    g_queueDevice[vq] = ds;
                    g_queueFamily[vq] = qci.queueFamilyIndex;
                }
            }
        }
    }
    ZLOG("device created, layer active");
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL LayerCreateSwapchainKHR(VkDevice device,
                                                              const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                              const VkAllocationCallbacks* pAllocator,
                                                              VkSwapchainKHR* pSwapchain) {
    DeviceState* ds = deviceFor(device);
    if (!ds) {
        auto gdpa = chainGDPA();
        if (!gdpa) return VK_ERROR_INITIALIZATION_FAILED;
        auto fp = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
        return fp(device, pCreateInfo, pAllocator, pSwapchain);
    }

    VkExtent2D renderExtent = pCreateInfo->imageExtent;
    VkExtent2D outputExtent = renderExtent;
    bool upscale = false;
    bool forceRecreate = false;

    VkSurfaceCapabilitiesKHR real{};
    bool haveReal =
        g_nextSurfaceCaps && g_nextSurfaceCaps(ds->gpu, pCreateInfo->surface, &real) == VK_SUCCESS;
    bool concrete = haveReal && real.currentExtent.width != UINT32_MAX && real.currentExtent.height != UINT32_MAX;
    if (concrete) outputExtent = real.currentExtent;

    bool capsTransfer = haveReal && (real.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    bool usageColor = (pCreateInfo->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    bool formatOk = true;
    if (g_formatProps) {
        VkFormatProperties fp{};
        g_formatProps(ds->gpu, pCreateInfo->imageFormat, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        formatOk = (fp.optimalTilingFeatures & need) == need;
    }
    bool active = processActive();
    bool canProcess = active && capsTransfer && usageColor && formatOk;
    if (!canProcess && active) {
        ZLOG("swapchain cannot use FSR (transfer=%d color=%d format=%d)", (int)capsTransfer,
             (int)usageColor, (int)formatOk);
        bool wasReduced = false;
        if (concrete) {
            wasReduced = renderExtent.width < real.currentExtent.width ||
                         renderExtent.height < real.currentExtent.height;
        }
        bool newlyDisabled = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            SurfaceState& st = g_surfaces[pCreateInfo->surface];
            newlyDisabled = !st.disabled;
            st.disabled = true;
            st.restore = true;
            if (!concrete && st.learned) {
                outputExtent = st.outputExtent;
                wasReduced = extentNear(renderExtent, reducedExtent(st.outputExtent, readScale()));
            }
        }
        if (newlyDisabled && wasReduced) forceRecreate = true;
    } else if (upscaleEnabled()) {
        if (concrete) {
            if (real.currentExtent.width >= renderExtent.width &&
                real.currentExtent.height >= renderExtent.height &&
                (real.currentExtent.width > renderExtent.width ||
                 real.currentExtent.height > renderExtent.height)) {
                outputExtent = real.currentExtent;
                upscale = true;
            }
        } else {
            std::lock_guard<std::mutex> lk(g_mutex);
            SurfaceState& st = g_surfaces[pCreateInfo->surface];
            if (!st.disabled) {
                auto requestRecreate = [&]() {
                    double now = nowSeconds();
                    if (now - st.lastRecreate > 2.0) st.recreateBurst = 0;
                    st.lastRecreate = now;
                    st.recreateBurst++;
                    if (st.recreateBurst <= 6) {
                        forceRecreate = true;
                    } else {
                        st.disabled = true;
                        st.restore = true;
                        ZLOG("wayland: too many recreate requests, disabling upscale");
                    }
                };
                VkExtent2D expectedRender = reducedExtent(st.outputExtent, readScale());
                if (!st.learned) {
                    st.outputExtent = pCreateInfo->imageExtent;
                    st.learned = true;
                    st.recreateAttempts = 0;
                    requestRecreate();
                    ZLOG("wayland: learned output %ux%u, asking for one recreate",
                         st.outputExtent.width, st.outputExtent.height);
                } else if (extentNear(renderExtent, expectedRender)) {
                    outputExtent = st.outputExtent;
                    upscale = true;
                    st.recreateAttempts = 0;
                } else if (extentNear(renderExtent, st.outputExtent)) {
                    st.recreateAttempts++;
                    if (st.recreateAttempts >= 2) {
                        st.disabled = true;
                        st.restore = true;
                    }
                    ZLOG("wayland: app ignored reduced extent, rendering full size (attempt %u)",
                         st.recreateAttempts);
                } else if (st.recreateAttempts < 2) {
                    st.outputExtent = pCreateInfo->imageExtent;
                    st.recreateAttempts++;
                    requestRecreate();
                    ZLOG("wayland: app requested a new extent %ux%u, expected %ux%u, re-learning",
                         renderExtent.width, renderExtent.height, expectedRender.width,
                         expectedRender.height);
                } else {
                    st.disabled = true;
                    st.restore = true;
                    ZLOG("wayland: disabling upscale, app keeps its own extent");
                }
            }
        }
    }

    if (!concrete) {
        std::lock_guard<std::mutex> lk(g_mutex);
        SurfaceState& st = g_surfaces[pCreateInfo->surface];
        if (!active) {
            VkExtent2D expected =
                st.learned ? reducedExtent(st.outputExtent, readScale()) : VkExtent2D{};
            if (!st.learned || !st.restore ||
                (!extentNear(renderExtent, expected) &&
                 !extentNear(renderExtent, st.outputExtent))) {
                st.outputExtent = renderExtent;
                st.learned = true;
                ZLOG("wayland: tracking output %ux%u while inactive", st.outputExtent.width,
                     st.outputExtent.height);
            }
        }
        if (st.learned) {
            bool reachedFull = extentNear(renderExtent, st.outputExtent);
            bool reducedActive = active && canProcess && upscale;
            if (reachedFull || reducedActive) st.restore = false;
        }
    }

    if (upscale) {
        renderExtent.width = std::min(renderExtent.width, outputExtent.width);
        renderExtent.height = std::min(renderExtent.height, outputExtent.height);
        if (renderExtent.width >= outputExtent.width || renderExtent.height >= outputExtent.height)
            upscale = false;
    }

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    if (upscale) ci.imageExtent = outputExtent;
    if (canProcess) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkExtent2D createdExtent = ci.imageExtent;
    VkResult res = ds->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    bool enabled = canProcess;
    if (res != VK_SUCCESS && !active) return res;
    if (res != VK_SUCCESS) {
        // The driver rejected the extra usage or the reduced/full extent. Try the
        // application's original extent without touching imageUsage first.
        VkSwapchainCreateInfoKHR fb = *pCreateInfo;
        res = ds->CreateSwapchainKHR(device, &fb, pAllocator, pSwapchain);
        if (res == VK_SUCCESS) {
            createdExtent = fb.imageExtent;
        } else {
            fb.imageExtent = outputExtent;
            res = ds->CreateSwapchainKHR(device, &fb, pAllocator, pSwapchain);
            if (res == VK_SUCCESS) createdExtent = fb.imageExtent;
        }
        enabled = false;
        upscale = false;
        if (res != VK_SUCCESS) return res;
        forceRecreate = true;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_surfaces[pCreateInfo->surface].disabled = true;
            g_surfaces[pCreateInfo->surface].restore = true;
        }
        ZLOG("swapchain rejected FSR usage; passthrough and one recreate");
    }

    SwapchainState sc;
    sc.surface = pCreateInfo->surface;
    sc.extent = createdExtent;
    sc.renderExtent = renderExtent;
    sc.format = pCreateInfo->imageFormat;
    sc.renderExtent.width = std::min(sc.renderExtent.width, sc.extent.width);
    sc.renderExtent.height = std::min(sc.renderExtent.height, sc.extent.height);
    sc.upscale = upscale && enabled;
    sc.enabled = enabled && active;
    sc.forceRecreate = forceRecreate;

    uint32_t count = 0;
    ds->GetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
    sc.images.resize(count);
    if (count > 0) ds->GetSwapchainImagesKHR(device, *pSwapchain, &count, sc.images.data());

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ds->swapchains[*pSwapchain] = std::move(sc);
    }
    ZLOG("swapchain created %ux%u <- %ux%u upscale=%d format=%d enabled=%d", sc.extent.width,
         sc.extent.height, sc.renderExtent.width, sc.renderExtent.height, (int)sc.upscale,
         (int)pCreateInfo->imageFormat, (int)enabled);
    return res;
}

static VKAPI_ATTR void VKAPI_CALL LayerDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks* pAllocator) {
    DeviceState* ds = deviceFor(device);
    if (ds) {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = ds->swapchains.find(swapchain);
        if (it != ds->swapchains.end()) {
            destroySwapchainResources(ds, &it->second);
            ds->swapchains.erase(it);
        }
    }
    auto gdpa = chainGDPA();
    if (!gdpa) return;
    auto fp = (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
    if (fp) fp(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL LayerQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    ControlState ctl = snapshotControl();
    bool active = processActiveFor(ctl);

    DeviceState* ds = nullptr;
    {
        auto it = g_queueDevice.find(queue);
        if (it != g_queueDevice.end()) ds = it->second;
    }

    if (ds) {
        std::lock_guard<std::mutex> lk(g_mutex);
        bool configChanged = ds->controlSerial != ctl.serial;
        if (configChanged) {
            ds->controlSerial = ctl.serial;
            // Only the process being attached or detached needs a recreate;
            // unrelated games that merely loaded the implicit layer stay
            // passthrough without a hitch.
            if (active || ds->activeLast) {
                for (auto& kv : ds->swapchains) kv.second.forceRecreate = true;
            }
            if (active) {
                for (auto& kv : g_surfaces) kv.second.disabled = false;
                if (readScale() <= 1.001f) {
                    for (auto& kv : g_surfaces) kv.second.restore = true;
                }
            } else {
                // Detach now, before the recreate completes: stop processing
                // frames immediately and ask the app for a full-size swapchain.
                for (auto& kv : ds->swapchains) {
                    if (kv.second.enabled) {
                        kv.second.enabled = false;
                        kv.second.forceRecreate = true;
                        if (kv.second.surface) g_surfaces[kv.second.surface].restore = true;
                    }
                }
            }
        }
        if (active && !ds->activeLast) {
            ds->fpsFrames = 0;
            ds->fps = 0.0f;
            ds->fpsStart = nowSeconds();
        }
        ds->activeLast = active;
        if (active) {
            ds->sharpness = readSharpness();
            ds->fpsFrames++;
            double now = nowSeconds();
            double elapsed = now - ds->fpsStart;
            if (elapsed >= 1.0) {
                ds->fps = (float)(ds->fpsFrames / elapsed);
                ds->fpsFrames = 0;
                ds->fpsStart = now;
            }
        }

        bool anyEnabled = false;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto it = ds->swapchains.find(pPresentInfo->pSwapchains[i]);
            if (it == ds->swapchains.end()) continue;
            SwapchainState& sc = it->second;
            if (sc.enabled) {
                anyEnabled = true;
                processFrame(ds, &sc, queue, pPresentInfo->pImageIndices[i]);
            }
        }
        if (active || anyEnabled || (ctl.pid > 0 && (long)getpid() == ctl.pid))
            writeStats(ds);
    }

    if (!ds || !ds->QueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = ds->QueuePresentKHR(queue, pPresentInfo);
    if (res != VK_SUCCESS) ZLOG("present result=%d", (int)res);

    bool recreate = false;
    if (ds) {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto it = ds->swapchains.find(pPresentInfo->pSwapchains[i]);
            if (it != ds->swapchains.end() && it->second.forceRecreate) {
                it->second.forceRecreate = false;
                recreate = true;
            }
        }
    }
    if (recreate && (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)) {
        ZLOG("returning OUT_OF_DATE (active=%d)", (int)active);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    return res;
}

static VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    DeviceState* ds = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            ds = it->second;
            g_devices.erase(it);
        }
        for (auto qit = g_queueDevice.begin(); qit != g_queueDevice.end();) {
            if (qit->second == ds) {
                g_queueFamily.erase(qit->first);
                qit = g_queueDevice.erase(qit);
            } else {
                ++qit;
            }
        }
    }

    auto gdpa = chainGDPA();
    auto fp = gdpa ? (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice") : nullptr;

    if (ds) {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (auto& kv : ds->swapchains) destroySwapchainResources(ds, &kv.second);
        if (ds->vertModule) ds->DestroyShaderModule(device, ds->vertModule, nullptr);
        if (ds->fragModule) ds->DestroyShaderModule(device, ds->fragModule, nullptr);
        if (ds->easuModule) ds->DestroyShaderModule(device, ds->easuModule, nullptr);
        if (ds->pipeLayout) ds->DestroyPipelineLayout(device, ds->pipeLayout, nullptr);
        if (ds->descPool) ds->DestroyDescriptorPool(device, ds->descPool, nullptr);
        if (ds->dsl) ds->DestroyDescriptorSetLayout(device, ds->dsl, nullptr);
        if (ds->sampler) ds->DestroySampler(device, ds->sampler, nullptr);
        for (auto& kv : ds->queues) {
            if (kv.second.fence) ds->DestroyFence(device, kv.second.fence, nullptr);
            if (kv.second.pool) ds->DestroyCommandPool(device, kv.second.pool, nullptr);
        }
        delete ds;
    }

    if (fp) fp(device, pAllocator);
}
