#include "layer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>

namespace {

struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_GetPhysicalDeviceProcAddr GetPhysicalDeviceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
};

std::mutex g_mutex;
std::unordered_map<VkInstance, InstanceDispatch*> g_instances;
std::unordered_map<VkDevice, DeviceDispatch*> g_devices;
std::unordered_map<VkQueue, DeviceDispatch*> g_queues;
std::atomic<uint64_t> g_presentCount{0};
PFN_vkQueuePresentKHR g_fallbackPresent = nullptr;

bool debugEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("ZYRAX_DEBUG");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void logMessage(const char* message, uint64_t value) {
    if (debugEnabled()) {
        std::fprintf(stderr, "[zyrax-layer] %s (%llu)\n", message,
                     static_cast<unsigned long long>(value));
    }
}

VkLayerInstanceCreateInfo* instanceLink(const VkInstanceCreateInfo* createInfo) {
    auto* info =
        reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(createInfo->pNext));
    while (info != nullptr) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO) {
            return info;
        }
        info = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* deviceLink(const VkDeviceCreateInfo* createInfo) {
    auto* info =
        reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(createInfo->pNext));
    while (info != nullptr) {
        if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            info->function == VK_LAYER_LINK_INFO) {
            return info;
        }
        info = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    }
    return nullptr;
}

InstanceDispatch* instanceDispatch(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : it->second;
}

DeviceDispatch* deviceDispatch(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

void registerQueue(VkQueue queue, DeviceDispatch* dispatch) {
    if (queue == VK_NULL_HANDLE || dispatch == nullptr) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queues[queue] = dispatch;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* createInfo,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkInstance* instance);
VKAPI_ATTR void VKAPI_CALL LayerDestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* createInfo,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkDevice* device);
VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL LayerGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                               uint32_t queueIndex, VkQueue* queue);
VKAPI_ATTR void VKAPI_CALL LayerGetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2* queueInfo,
                                                VkQueue* queue);
VKAPI_ATTR VkResult VKAPI_CALL LayerQueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* presentInfo);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(VkInstance instance,
                                                                 const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(VkDevice device,
                                                               const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                       const char* pName);

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* createInfo,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkInstance* instance) {
    VkLayerInstanceCreateInfo* link = instanceLink(createInfo);
    if (link == nullptr || link->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr nextGIPA = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_GetPhysicalDeviceProcAddr nextPDPA =
        link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    if (nextGIPA == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto nextCreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(nextGIPA(nullptr, "vkCreateInstance"));
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (nextCreateInstance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = nextCreateInstance(createInfo, allocator, instance);
    if (result != VK_SUCCESS) return result;

    auto* dispatch = new (std::nothrow) InstanceDispatch;
    if (dispatch == nullptr) {
        auto nextDestroy =
            reinterpret_cast<PFN_vkDestroyInstance>(nextGIPA(*instance, "vkDestroyInstance"));
        if (nextDestroy != nullptr) nextDestroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    dispatch->GetInstanceProcAddr = nextGIPA;
    dispatch->GetPhysicalDeviceProcAddr = nextPDPA;
    dispatch->GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        nextGIPA(*instance, "vkGetDeviceProcAddr"));
    dispatch->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        nextGIPA(*instance, "vkDestroyInstance"));
    dispatch->CreateDevice =
        reinterpret_cast<PFN_vkCreateDevice>(nextGIPA(*instance, "vkCreateDevice"));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[*instance] = dispatch;
    }

    logMessage("instance created", 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* allocator) {
    InstanceDispatch* dispatch = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) {
            dispatch = it->second;
            g_instances.erase(it);
        }
    }

    if (dispatch != nullptr) {
        if (dispatch->DestroyInstance != nullptr) {
            dispatch->DestroyInstance(instance, allocator);
        }
        delete dispatch;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* createInfo,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkDevice* device) {
    VkLayerDeviceCreateInfo* link = deviceLink(createInfo);
    if (link == nullptr || link->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr nextGIPA = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGDPA = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (nextGIPA == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    auto nextCreateDevice =
        reinterpret_cast<PFN_vkCreateDevice>(nextGIPA(nullptr, "vkCreateDevice"));
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (nextCreateDevice == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = nextCreateDevice(physicalDevice, createInfo, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto* dispatch = new (std::nothrow) DeviceDispatch;
    if (dispatch == nullptr) {
        if (nextGDPA != nullptr) {
            auto nextDestroy = reinterpret_cast<PFN_vkDestroyDevice>(
                nextGDPA(*device, "vkDestroyDevice"));
            if (nextDestroy != nullptr) nextDestroy(*device, allocator);
        }
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if (nextGDPA != nullptr) {
        dispatch->GetDeviceProcAddr = nextGDPA;
        dispatch->DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            nextGDPA(*device, "vkDestroyDevice"));
        dispatch->GetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            nextGDPA(*device, "vkGetDeviceQueue"));
        dispatch->GetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
            nextGDPA(*device, "vkGetDeviceQueue2"));
        dispatch->QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
            nextGDPA(*device, "vkQueuePresentKHR"));
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices[*device] = dispatch;
        if (dispatch->QueuePresentKHR != nullptr) {
            g_fallbackPresent = dispatch->QueuePresentKHR;
        }
    }

    if (dispatch->GetDeviceQueue != nullptr) {
        for (uint32_t i = 0; i < createInfo->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo& queueInfo = createInfo->pQueueCreateInfos[i];
            for (uint32_t j = 0; j < queueInfo.queueCount; ++j) {
                VkQueue queue = VK_NULL_HANDLE;
                dispatch->GetDeviceQueue(*device, queueInfo.queueFamilyIndex, j, &queue);
                registerQueue(queue, dispatch);
            }
        }
    }

    logMessage("device created", 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* allocator) {
    DeviceDispatch* dispatch = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            dispatch = it->second;
            g_devices.erase(it);
        }
        if (dispatch != nullptr) {
            for (auto queueIt = g_queues.begin(); queueIt != g_queues.end();) {
                if (queueIt->second == dispatch) {
                    queueIt = g_queues.erase(queueIt);
                } else {
                    ++queueIt;
                }
            }
            if (g_fallbackPresent == dispatch->QueuePresentKHR) {
                g_fallbackPresent = nullptr;
            }
        }
    }

    if (dispatch != nullptr) {
        if (dispatch->DestroyDevice != nullptr) {
            dispatch->DestroyDevice(device, allocator);
        }
        delete dispatch;
    }
}

VKAPI_ATTR void VKAPI_CALL LayerGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                               uint32_t queueIndex, VkQueue* queue) {
    DeviceDispatch* dispatch = deviceDispatch(device);
    if (dispatch == nullptr || dispatch->GetDeviceQueue == nullptr) {
        if (queue != nullptr) *queue = VK_NULL_HANDLE;
        return;
    }

    dispatch->GetDeviceQueue(device, queueFamilyIndex, queueIndex, queue);
    if (queue != nullptr) registerQueue(*queue, dispatch);
}

VKAPI_ATTR void VKAPI_CALL LayerGetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2* queueInfo,
                                                VkQueue* queue) {
    DeviceDispatch* dispatch = deviceDispatch(device);
    if (dispatch == nullptr || dispatch->GetDeviceQueue2 == nullptr) {
        if (queue != nullptr) *queue = VK_NULL_HANDLE;
        return;
    }

    dispatch->GetDeviceQueue2(device, queueInfo, queue);
    if (queue != nullptr) registerQueue(*queue, dispatch);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerQueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* presentInfo) {
    PFN_vkQueuePresentKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_queues.find(queue);
        if (it != g_queues.end()) next = it->second->QueuePresentKHR;
        if (next == nullptr) next = g_fallbackPresent;
    }

    if (next == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    uint64_t count = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    logMessage("frame presented", count);
    return next(queue, presentInfo);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(VkInstance instance,
                                                                 const char* pName) {
    if (pName == nullptr) return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkGetPhysicalDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetPhysicalDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateInstance);
    }
    if (std::strcmp(pName, "vkDestroyInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyInstance);
    }
    if (std::strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateDevice);
    }

    if (instance == VK_NULL_HANDLE) return nullptr;

    InstanceDispatch* dispatch = instanceDispatch(instance);
    if (dispatch == nullptr || dispatch->GetInstanceProcAddr == nullptr) return nullptr;
    return dispatch->GetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(VkDevice device,
                                                               const char* pName) {
    if (pName == nullptr) return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyDevice);
    }
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceQueue);
    }
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceQueue2);
    }
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerQueuePresentKHR);
    }

    DeviceDispatch* dispatch = deviceDispatch(device);
    if (dispatch == nullptr || dispatch->GetDeviceProcAddr == nullptr) return nullptr;
    return dispatch->GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetPhysicalDeviceProcAddr(
    VkInstance instance, const char* pName) {
    if (pName == nullptr) return nullptr;

    InstanceDispatch* dispatch = instanceDispatch(instance);
    if (dispatch == nullptr || dispatch->GetPhysicalDeviceProcAddr == nullptr) return nullptr;
    return dispatch->GetPhysicalDeviceProcAddr(instance, pName);
}

}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* pName) {
    return LayerGetInstanceProcAddr(instance, pName);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char* pName) {
    return LayerGetDeviceProcAddr(device, pName);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetPhysicalDeviceProcAddr(
    VkInstance instance, const char* pName) {
    return LayerGetPhysicalDeviceProcAddr(instance, pName);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (version == nullptr || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (version->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        version->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }

    version->pfnGetInstanceProcAddr = LayerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = LayerGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = LayerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
