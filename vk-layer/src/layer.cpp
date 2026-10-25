// Zyrax Vulkan implicit layer (Linux)
// ------------------------------------
// Intercepts swapchain presentation and applies an FSR-style RCAS (Robust
// Contrast-Adaptive Sharpening) fragment pass to every presented frame.
//
// Enabled only when ZYRAX_FSR_ENABLE=1 is set (see VkLayer_zyrax_fsr.json).
// Environment variables:
//   ZYRAX_FSR_SHARPNESS   0.0 .. 1.0   (default 0.5)
//   ZYRAX_FSR_DISABLE     set to 1 to hard-disable
//   ZYRAX_FSR_DEBUG       set to 1 for stderr logging
//   ZYRAX_FSR_STATS       stats output path
//                          (default /tmp/zyrax_fsr_stats.json)

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rcas_frag.h"
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

static bool layerEnabled() {
    const char* dis = getenv("ZYRAX_FSR_DISABLE");
    if (dis && dis[0] == '1') return false;
    const char* en = getenv("ZYRAX_FSR_ENABLE");
    if (en && en[0] == '0') return false;
    return true;
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

static float readSharpness() {
    const char* s = getenv("ZYRAX_FSR_SHARPNESS");
    float v = s ? (float)atof(s) : 0.5f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static const char* statsPath() {
    const char* p = getenv("ZYRAX_FSR_STATS");
    return p && p[0] ? p : "/tmp/zyrax_fsr_stats.json";
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct PushConstants {
    float resolution[2];
    float sharpness;
    float pad;
};

struct QueueState {
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

struct SwapchainState {
    VkExtent2D extent{0, 0};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    std::vector<VkFramebuffer> framebuffers;
    bool enabled = false;
    bool failed = false;
    bool resourcesReady = false;

    VkImage scratch = VK_NULL_HANDLE;
    VkDeviceMemory scratchMem = VK_NULL_HANDLE;
    VkImageView scratchView = VK_NULL_HANDLE;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct DeviceState {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

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

    float sharpness = 0.5f;
    uint64_t presents = 0;
    double lastStatsWrite = 0.0;

    std::unordered_map<VkQueue, QueueState> queues;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
};

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

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
    VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets = 32;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &size;
    if (ds->CreateDescriptorPool(ds->device, &poolCi, nullptr, &ds->descPool) != VK_SUCCESS) {
        ds->commonFailed = true;
        return false;
    }

    VkSamplerCreateInfo sCi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sCi.magFilter = VK_FILTER_NEAREST;
    sCi.minFilter = VK_FILTER_NEAREST;
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
    ds->fragModule = makeModule(ds, rcas_frag_spv, rcas_frag_spv_len);
    if (!ds->vertModule || !ds->fragModule) {
        ds->commonFailed = true;
        return false;
    }

    ds->sharpness = readSharpness();
    ds->commonReady = true;
    ZLOG("common resources ready (sharpness=%.2f)", ds->sharpness);
    return true;
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

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ds->vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ds->fragModule;
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

    if (ds->CreateGraphicsPipelines(ds->device, VK_NULL_HANDLE, 1, &gp, nullptr, &sc->pipeline) != VK_SUCCESS)
        return false;
    return true;
}

static bool ensureSwapchainResources(DeviceState* ds, SwapchainState* sc) {
    if (sc->resourcesReady) return true;
    if (sc->failed || !sc->enabled) return false;
    if (!ensureCommon(ds)) return false;

    sc->scratch = createImage(ds, sc->format, sc->extent,
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

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = ds->descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &ds->dsl;
    if (ds->AllocateDescriptorSets(ds->device, &ai, &sc->desc) != VK_SUCCESS) {
        sc->failed = true;
        return false;
    }

    VkDescriptorImageInfo srcInfo{};
    srcInfo.sampler = ds->sampler;
    srcInfo.imageView = sc->scratchView;
    srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sc->desc;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &srcInfo;
    ds->UpdateDescriptorSets(ds->device, 1, &write, 0, nullptr);

    sc->resourcesReady = true;
    ZLOG("swapchain resources ready %ux%u format=%d", sc->extent.width, sc->extent.height, (int)sc->format);
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
    if (ds->AllocateCommandBuffers(ds->device, &ai, &qs.cmd) != VK_SUCCESS) return nullptr;

    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (ds->CreateFence(ds->device, &fc, nullptr, &qs.fence) != VK_SUCCESS) return nullptr;

    auto [pos, _] = ds->queues.emplace(queue, qs);
    return &pos->second;
}

static void processFrame(DeviceState* ds, SwapchainState* sc, VkQueue queue, uint32_t index) {
    if (index >= sc->images.size()) return;
    if (!ensureSwapchainResources(ds, sc)) return;
    QueueState* qs = ensureQueue(ds, queue);
    if (!qs) return;

    VkImage swImg = sc->images[index];
    VkFramebuffer fb = sc->framebuffers[index];

    ds->ResetCommandBuffer(qs->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (ds->BeginCommandBuffer(qs->cmd, &bi) != VK_SUCCESS) return;
    VkCommandBuffer cb = qs->cmd;

    // Copy the presented frame into our scratch image.
    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    imageBarrier(ds, cb, sc->scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyImage(ds, cb, swImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->scratch,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, sc->extent);
    imageBarrier(ds, cb, sc->scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Sharpen into the swapchain image.
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = sc->renderPass;
    rp.framebuffer = fb;
    rp.renderArea = {{0, 0}, sc->extent};
    ds->CmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ds->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sc->pipeline);
    ds->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ds->pipeLayout, 0, 1, &sc->desc, 0, nullptr);

    PushConstants pc{};
    pc.resolution[0] = (float)sc->extent.width;
    pc.resolution[1] = (float)sc->extent.height;
    pc.sharpness = ds->sharpness;
    pc.pad = 0.0f;
    ds->CmdPushConstants(cb, ds->pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    VkViewport vp{0, 0, (float)sc->extent.width, (float)sc->extent.height, 0.0f, 1.0f};
    VkRect2D scc{{0, 0}, sc->extent};
    ds->CmdSetViewport(cb, 0, 1, &vp);
    ds->CmdSetScissor(cb, 0, 1, &scc);
    ds->CmdDraw(cb, 3, 1, 0, 0);
    ds->CmdEndRenderPass(cb);

    imageBarrier(ds, cb, swImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    for (auto& kv : ds->swapchains) {
        if (kv.second.enabled) {
            ext = kv.second.extent;
            fmt = kv.second.format;
            break;
        }
    }
    fprintf(f,
            "{\"enabled\":true,\"presents\":%llu,\"sharpness\":%.3f,"
            "\"width\":%u,\"height\":%u,\"format\":%d,\"pid\":%d,\"timestamp\":%lld}\n",
            (unsigned long long)ds->presents, ds->sharpness, ext.width, ext.height, (int)fmt,
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

    if (sc->pipeline) ds->DestroyPipeline(ds->device, sc->pipeline, nullptr);
    if (sc->renderPass) ds->DestroyRenderPass(ds->device, sc->renderPass, nullptr);
    if (sc->scratchView) ds->DestroyImageView(ds->device, sc->scratchView, nullptr);
    if (sc->scratch) ds->DestroyImage(ds->device, sc->scratch, nullptr);
    if (sc->scratchMem) ds->FreeMemory(ds->device, sc->scratchMem, nullptr);

    sc->pipeline = VK_NULL_HANDLE;
    sc->renderPass = VK_NULL_HANDLE;
    sc->scratchView = VK_NULL_HANDLE;
    sc->scratch = VK_NULL_HANDLE;
    sc->scratchMem = VK_NULL_HANDLE;
    sc->resourcesReady = false;
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
    if (g_nextPDPA) return (PFN_vkVoidFunction)g_nextPDPA(instance, pName);
    auto gipa = chainGIPA();
    return gipa ? (PFN_vkVoidFunction)gipa(instance, pName) : nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkGetPhysicalDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetPhysicalDeviceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)LayerCreateInstance;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)LayerCreateDevice;

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

    if (!layerEnabled()) {
        ZLOG("layer disabled by environment");
        return VK_SUCCESS;
    }

    auto* ds = new DeviceState();
    ds->gpu = gpu;
    ds->device = *pDevice;
    ds->sharpness = readSharpness();

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
    if (!ds || !layerEnabled()) {
        auto gdpa = chainGDPA();
        if (!gdpa) return VK_ERROR_INITIALIZATION_FAILED;
        auto fp = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
        return fp(device, pCreateInfo, pAllocator, pSwapchain);
    }

    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = ds->CreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
    bool enabled = true;
    if (res != VK_SUCCESS) {
        res = ds->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        enabled = false;
        if (res != VK_SUCCESS) return res;
    }

    SwapchainState sc;
    sc.extent = pCreateInfo->imageExtent;
    sc.format = pCreateInfo->imageFormat;
    sc.enabled = enabled && layerEnabled();

    uint32_t count = 0;
    ds->GetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
    sc.images.resize(count);
    if (count > 0) ds->GetSwapchainImagesKHR(device, *pSwapchain, &count, sc.images.data());

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        ds->swapchains[*pSwapchain] = std::move(sc);
    }
    ZLOG("swapchain created %ux%u format=%d enabled=%d", pCreateInfo->imageExtent.width,
         pCreateInfo->imageExtent.height, (int)pCreateInfo->imageFormat, (int)enabled);
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
    DeviceState* ds = nullptr;
    {
        auto it = g_queueDevice.find(queue);
        if (it != g_queueDevice.end()) ds = it->second;
    }

    if (ds && layerEnabled()) {
        std::lock_guard<std::mutex> lk(g_mutex);
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto it = ds->swapchains.find(pPresentInfo->pSwapchains[i]);
            if (it != ds->swapchains.end() && it->second.enabled) {
                processFrame(ds, &it->second, queue, pPresentInfo->pImageIndices[i]);
            }
        }
        writeStats(ds);
    }

    if (ds && ds->QueuePresentKHR) return ds->QueuePresentKHR(queue, pPresentInfo);
    return VK_ERROR_INITIALIZATION_FAILED;
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
