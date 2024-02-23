/*
 * Copyright (C) 2023 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Mark Young <marky@lunarg.com>
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <vulkan/layer/vk_layer_settings.hpp>
#include <vulkan/vk_enum_string_helper.h>
#include "vk_layer_table.h"

namespace slowdevicesimulator {

#define kSettingsKeyFenceDelayType "fence_delay_type"
#define kSettingsKeyFenceDelayCount "fence_delay_count"
#define kSettingsKeyMemoryAdjustPercent "memory_percent"

enum class FenceDelayType {
    FENCE_DELAY_NONE = 0,
    FENCE_DELAY_MS_FROM_TRIGGER,
    FENCE_DELAY_MS_FROM_FIRST_QUERY,
    FENCE_DELAY_NUM_FAIL_WAITS,
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

static const char *GetDefaultPrefix() {
#ifdef __ANDROID__
    return "slowdevicesim";
#else
    return "SLOWDEVICESIM";
#endif
}

#ifdef ANDROID
#include <android/log.h>
#define WRITE_LOG_MESSAGE(message, ...)                                                    \
    do {                                                                                   \
        __android_log_print(ANDROID_LOG_INFO, GetDefaultPrefix(), message, ##__VA_ARGS__); \
    } while (0)
#else
#define WRITE_LOG_MESSAGE(message, ...) \
    do {                                \
        printf(message, ##__VA_ARGS__); \
        printf("\n");                   \
    } while (0)
#endif

#if ENABLE_FUNC_LOGGING == 1
#define LOG_ENTRY_FUNC(a) WRITE_LOG_MESSAGE("%s {", a)
#define LOG_EXIT_FUNC(a) WRITE_LOG_MESSAGE("} %s", a)
#define LOG_EXIT_RETURN_FUNC(a, b) WRITE_LOG_MESSAGE("} %s [0x%08x]", a, b)
#else
#define LOG_ENTRY_FUNC(a)
#define LOG_EXIT_FUNC(a)
#define LOG_EXIT_RETURN_FUNC(a, b)
#endif

static const VkLayerProperties g_layer_properties = {
    "VK_LAYER_LUNARG_slow_device_simulator",  // layerName
    VK_MAKE_VERSION(1, 3, 0),                 // specVersion (clamped to final 1.0 spec version)
    1,                                        // implementationVersion
    "Layer: Slow Device Simulator",           // description
};

// Global mutex for restring instance create/destroys and prints to one at a time
static std::mutex g_instance_mutex;

struct InstanceExtensionsEnabled {
    bool core_1_1 = false;
    bool core_1_2 = false;
    bool core_1_3 = false;
    bool KHR_device_group_create = false;
    bool KHR_external_mem_caps = false;
    bool KHR_get_phys_dev_props2 = false;
};
struct InstanceMapStruct {
    VkuInstanceDispatchTable *dispatch_table;
    InstanceExtensionsEnabled extension_enables;
    FenceDelayType fence_delay_type{FenceDelayType::FENCE_DELAY_NONE};
    bool layer_enabled{false};
    int fence_delay_count{0};
    int memory_percent{100};
};
static std::unordered_map<VkInstance, InstanceMapStruct *> g_instance_map;

struct DeviceExtensions {
    bool core_1_1 = false;
    bool core_1_2 = false;
    bool core_1_3 = false;
    bool KHR_external_mem_fd = false;
    bool KHR_swapchain = false;
    bool KHR_sync2 = false;
    bool EXT_display_control = false;
    bool EXT_mem_budget = false;
    bool EXT_swapchain_maintenance1 = false;
    bool ANDROID_ext_mem_hw_buf = false;
};

struct MemoryHeapWithBudget {
    VkDeviceSize size{0};
    VkDeviceSize allocated{0};
    VkDeviceSize budget{0};
    VkDeviceSize usage{0};
    VkMemoryHeapFlags flags;
} VkMemoryHeap;
struct PhysicalDeviceMemoryBudgetProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapCount;
    MemoryHeapWithBudget memoryHeaps[VK_MAX_MEMORY_HEAPS];
};
struct PhysDeviceMapStruct {
    VkInstance instance;
    VkPhysicalDeviceProperties props;
    PhysicalDeviceMemoryBudgetProperties memory_props;
    DeviceExtensions extensions_supported;
    bool memory_budget_updated = false;
    std::mutex device_mutex;
    bool layer_enabled{false};
    int memory_percent{100};
};
static std::unordered_map<VkPhysicalDevice, PhysDeviceMapStruct *> g_phys_device_map;

struct DeviceMapStruct {
    VkPhysicalDevice physical_device;
    VkuDeviceDispatchTable *dispatch_table;
    DeviceExtensions extension_enables;
    bool memory_bindings_updated = false;
    std::mutex memory_mutex;
    std::mutex fence_mutex;
    bool layer_enabled{false};
    FenceDelayType fence_delay_type{FenceDelayType::FENCE_DELAY_NONE};
    int fence_delay_count{0};
};
static std::unordered_map<VkDevice, DeviceMapStruct *> g_device_map;

struct ExternalMemFdMapStruct {
    VkDevice device;
    uint32_t memory_type;
};
static std::unordered_map<int64_t, ExternalMemFdMapStruct *> g_ext_mem_fd_map;

#ifdef ANDROID
struct AndroidBufferMapStruct {
    VkDevice device;
    VkDeviceSize alloc_size;
    uint32_t memory_type;
};
static std::unordered_map<const AHardwareBuffer *, AndroidBufferMapStruct *> g_android_buf_map;
#endif

struct FenceMapStruct {
    VkDevice device;
    bool signalled{false};
    bool wait_started{false};
    bool wait_completed{false};
    bool layer_enabled{false};
    FenceDelayType delay_type{FenceDelayType::FENCE_DELAY_NONE};
    uint32_t delay_count{0};    // Count or Ms required to wait
    uint32_t elapsed_count{0};  // Count or Ms elapsed
    std::chrono::high_resolution_clock::time_point start_time;
};
static std::unordered_map<VkFence, FenceMapStruct *> g_fence_map;

enum AdditionalBufferValidFlags {
    ADD_BUFFER_VALID_NONE = 0x00000000,
    ADD_BUFFER_VALID_OPAQUE_CAPTURE = 0x00000001,
    ADD_BUFFER_VALID_EXTERNAL_MEM_HANDLE_FLAGS = 0x00000002,
    ADD_BUFFER_VALID_DEVICE_ADDRESS = 0x00000004,
};

struct AdditionalBufferStruct {
    uint32_t flags = 0;
    uint64_t opaque_capture_address;                               // ADD_BUFFER_VALID_OPAQUE_CAPTURE
    VkExternalMemoryHandleTypeFlags external_memory_handle_flags;  // ADD_BUFFER_VALID_EXTERNAL_MEM_HANDLE_FLAGS
    VkDeviceAddress device_address;                                // ADD_BUFFER_VALID_DEVICE_ADDRESS
};

struct BufferMapStruct {
    VkDevice device;
    VkBufferCreateInfo create_info;
    AdditionalBufferStruct additional_info;
    VkMemoryRequirements memory_reqs;
};
static std::unordered_map<VkBuffer, BufferMapStruct *> g_buffer_map;

enum AdditionalImageValidFlags {
    ADD_IMAGE_VALID_NONE = 0x00000000,
    ADD_IMAGE_VALID_EXTERNAL_MEM_HANDLE_FLAGS = 0x00000001,
    ADD_IMAGE_VALID_FORMAT_LIST = 0x00000002,
    ADD_IMAGE_VALID_STENCIL_USAGE = 0x00000004,
    ADD_IMAGE_VALID_SWAPCHAIN = 0x00000008,
    ADD_IMAGE_VALID_COMPRESSION_CONTROL = 0x000000010,
    ADD_IMAGE_VALID_DRM_FORMAT_MOD_EXPLICIT = 0x000000020,
    ADD_IMAGE_VALID_DRM_FORMAT_MOD_LIST = 0x000000040,
    ADD_IMAGE_VALID_EXTERNAL_FORMAT_ANDROID = 0x000000080,
};

struct AdditionalImageStruct {
    uint32_t flags = 0;
    VkExternalMemoryHandleTypeFlags external_memory_handle_flags;       // ADD_IMAGE_VALID_EXTERNAL_MEM_HANDLE_FLAGS
    std::vector<VkFormat> format_list;                                  // ADD_IMAGE_VALID_FORMAT_LIST
    VkImageUsageFlags stencil_usage;                                    // ADD_IMAGE_VALID_STENCIL_USAGE
    VkSwapchainKHR swapchain;                                           // ADD_IMAGE_VALID_SWAPCHAIN
    VkImageCompressionFlagsEXT image_compress_flags;                    // ADD_IMAGE_VALID_COMPRESSION_CONTROL
    std::vector<VkImageCompressionFixedRateFlagsEXT> fixed_rate_flags;  // ADD_IMAGE_VALID_COMPRESSION_CONTROL
    uint64_t drm_format_modifier;                                       // ADD_IMAGE_VALID_DRM_FORMAT_MOD_EXPLICIT
    std::vector<VkSubresourceLayout> plane_layouts;                     // ADD_IMAGE_VALID_DRM_FORMAT_MOD_EXPLICIT
    std::vector<uint64_t> drm_format_modifiers;                         // ADD_IMAGE_VALID_DRM_FORMAT_MOD_LIST
    uint64_t external_android_format;                                   // ADD_IMAGE_VALID_EXTERNAL_FORMAT_ANDROID
};

struct ImageMapStruct {
    VkDevice device;
    VkImageCreateInfo create_info;
    AdditionalImageStruct additional_info;
    VkMemoryRequirements memory_reqs;
};
static std::unordered_map<VkImage, ImageMapStruct *> g_image_map;

struct BufferMemoryStruct {
    VkBuffer buffer;
    VkDeviceSize offset;
};

enum AdditionalImageMemoryValidFlags {
    ADD_IMAGE_MEM_VALID_NONE = 0x00000000,
    ADD_IMAGE_MEM_VALID_PLANE_MEM = 0x00000001,
    ADD_IMAGE_MEM_VALID_SWAPCHAIN = 0x00000002,
};
struct AdditionalImageMemoryStruct {
    uint32_t flags = 0;
    VkImageAspectFlagBits plane_mem_aspect;  // ADD_IMAGE_MEM_VALID_PLANE_MEM
    VkSwapchainKHR swapchain;                // ADD_IMAGE_MEM_VALID_SWAPCHAIN
    uint32_t swapchain_image_index;          // ADD_IMAGE_MEM_VALID_SWAPCHAIN
};

struct ImageMemoryStruct {
    VkImage image;
    VkDeviceSize offset;
    AdditionalImageMemoryStruct additional_info;
};

enum AdditionalMemoryValidFlags {
    ADD_MEM_VALID_NONE = 0x00000000,
    ADD_MEM_VALID_EXTERNAL_MEM_HANDLE_FLAGS = 0x00000001,
    ADD_MEM_VALID_DEDICATED_ALLOC = 0x00000002,
    ADD_MEM_VALID_ALLOCATE_FLAG_INFO = 0x00000004,
    ADD_MEM_VALID_OPAQUE_CAPTURE_ADDRESS = 0x00000008,
    ADD_MEM_VALID_EXTERNAL_MEM_FD = 0x00000010,
    ADD_MEM_VALID_IMPORT_HOST_POINTER = 0x00000020,
    ADD_MEM_VALID_PRIORITY = 0x00000040,
    ADD_MEM_VALID_ANDROID_HARDWARE_BUFFER = 0x00000080,
};

struct AdditionalMemoryStruct {
    uint32_t flags = 0;
    VkExternalMemoryHandleTypeFlags external_memory_handle_flags;    // ADD_MEM_VALID_EXTERNAL_MEM_HANDLE_FLAGS
    VkImage dedicated_image;                                         // ADD_MEM_VALID_DEDICATED_ALLOC
    VkBuffer dedicated_buffer;                                       // ADD_MEM_VALID_DEDICATED_ALLOC
    VkMemoryAllocateFlags memory_alloc_flags;                        // ADD_MEM_VALID_ALLOCATE_FLAG_INFO
    uint32_t memory_alloc_device_mask;                               // ADD_MEM_VALID_ALLOCATE_FLAG_INFO
    uint64_t opaque_capture_address;                                 // ADD_MEM_VALID_OPAQUE_CAPTURE_ADDRESS
    VkExternalMemoryHandleTypeFlagBits ext_memory_fd_handle_type;    // ADD_MEM_VALID_EXTERNAL_MEM_FD
    int64_t ext_memory_fd;                                           // ADD_MEM_VALID_EXTERNAL_MEM_FD
    VkExternalMemoryHandleTypeFlagBits import_host_ptr_handle_type;  // ADD_MEM_VALID_IMPORT_HOST_POINTER
    void *import_host_ptr;                                           // ADD_MEM_VALID_IMPORT_HOST_POINTER
    float memory_priority;                                           // ADD_MEM_VALID_PRIORITY
#ifdef ANDROID
    AHardwareBuffer *android_hw_buffer;  // ADD_MEM_VALID_ANDROID_HARDWARE_BUFFER
#endif
};

struct MemoryMapStruct {
    VkDevice device;
    VkMemoryAllocateInfo alloc_info;
    AdditionalMemoryStruct additional_info;
    std::vector<BufferMemoryStruct> buffers;
    std::vector<ImageMemoryStruct> images;
};
static std::unordered_map<VkDeviceMemory, MemoryMapStruct *> g_memory_map;

static std::unordered_map<VkQueue, VkDevice> g_queue_to_device_map;

static InstanceMapStruct *GetInstanceMapEntry(VkInstance instance) {
    auto it = g_instance_map.find(instance);
    if (it == g_instance_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseInstanceMapEntry(VkInstance instance) {
    std::unique_lock<std::mutex> lock(g_instance_mutex);
    InstanceMapStruct *map = GetInstanceMapEntry(instance);
    if (map != nullptr) {
        delete map->dispatch_table;
        delete map;
        g_instance_map.erase(instance);
    }
}

static PhysDeviceMapStruct *GetPhysicalDeviceMapEntry(VkPhysicalDevice phys_dev) {
    auto it = g_phys_device_map.find(phys_dev);
    if (it == g_phys_device_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static DeviceMapStruct *GetDeviceMapEntry(VkDevice device) {
    auto it = g_device_map.find(device);
    if (it == g_device_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseDeviceMapEntry(VkDevice device) {
    DeviceMapStruct *map = GetDeviceMapEntry(device);
    if (map != nullptr) {
        delete map->dispatch_table;
        delete map;
        g_device_map.erase(device);
    }
}

static ExternalMemFdMapStruct *GetExternalMemFdMapEntry(uint64_t fd) {
    auto it = g_ext_mem_fd_map.find(fd);
    if (it == g_ext_mem_fd_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseExternalMemFdMapEntries(VkDevice device) {
restart_loop:
    for (auto &ext_mem_fd_iter : g_ext_mem_fd_map) {
        ExternalMemFdMapStruct *map = ext_mem_fd_iter.second;
        if (map->device == device) {
            g_ext_mem_fd_map.erase(ext_mem_fd_iter.first);
            goto restart_loop;
        }
    }
}

static FenceMapStruct *GetFenceMapEntry(VkFence fence) {
    auto it = g_fence_map.find(fence);
    if (it == g_fence_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseFenceMapEntry(VkFence fence) {
    FenceMapStruct *map = GetFenceMapEntry(fence);
    if (map != nullptr) {
        delete map;
        g_fence_map.erase(fence);
    }
}

#ifdef ANDROID
static AndroidBufferMapStruct *GetAndroidBufferMapEntry(AHardwareBuffer *android_buf) {
    auto it = g_android_buf_map.find(android_buf);
    if (it == g_android_buf_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseAndroidBufferMapEntries(VkDevice device) {
restart_loop:
    for (auto &and_buf_iter : g_android_buf_map) {
        AndroidBufferMapStruct *map = and_buf_iter.second;
        if (map->device == device) {
            g_android_buf_map.erase(and_buf_iter.first);
            goto restart_loop;
        }
    }
}
#endif  // ANDROID

static BufferMapStruct *GetBufferMapEntry(VkBuffer buffer) {
    auto it = g_buffer_map.find(buffer);
    if (it == g_buffer_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseBufferMapEntry(VkBuffer buffer) {
    BufferMapStruct *map = GetBufferMapEntry(buffer);
    if (map != nullptr) {
        delete map;
        g_buffer_map.erase(buffer);

        // Erase any memory map entry that is for this buffer
        for (auto &mem_map_iter : g_memory_map) {
            MemoryMapStruct *map = mem_map_iter.second;
            for (auto buf_iter = map->buffers.begin(); buf_iter != map->buffers.end();) {
                if (buf_iter->buffer == buffer) {
                    buf_iter = map->buffers.erase(buf_iter);
                } else {
                    buf_iter++;
                }
            }
        }
    }
}

static ImageMapStruct *GetImageMapEntry(VkImage image) {
    auto it = g_image_map.find(image);
    if (it == g_image_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseImageMapEntry(VkImage image) {
    ImageMapStruct *map = GetImageMapEntry(image);
    if (map != nullptr) {
        delete map;
        g_image_map.erase(image);

        // Erase any memory map entry that is for this image
        for (auto &mem_map_iter : g_memory_map) {
            MemoryMapStruct *map = mem_map_iter.second;
            for (auto img_iter = map->images.begin(); img_iter != map->images.end();) {
                if (img_iter->image == image) {
                    img_iter = map->images.erase(img_iter);
                } else {
                    img_iter++;
                }
            }
        }
    }
}

static MemoryMapStruct *GetMemoryMapEntry(VkDeviceMemory memory) {
    auto it = g_memory_map.find(memory);
    if (it == g_memory_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

static void EraseMemoryMapEntry(VkDeviceMemory memory) {
    MemoryMapStruct *map = GetMemoryMapEntry(memory);
    if (map != nullptr) {
        delete map;
        g_memory_map.erase(memory);
    }
}

// ------------------------- Function Prototypes --------------------------------------

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);

// Prototypes defined later but used by the GetXXXProcAddr calls
static PFN_vkVoidFunction ImplementedInstanceCommands(const char *name);
static PFN_vkVoidFunction ImplementedInstanceNewerCoreCommands(InstanceMapStruct *instance_map_data, const char *name);
static PFN_vkVoidFunction ImplementedInstanceExtensionCommands(InstanceMapStruct *instance_map_data, const char *name);
static PFN_vkVoidFunction ImplementedDeviceCommands(const char *name);
static PFN_vkVoidFunction ImplementedDeviceExtensionCommands(DeviceExtensions *supported, const char *name);

// ------------------------- Instance Functions --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties) {
    LOG_ENTRY_FUNC("EnumerateInstanceLayerProperties");
    VkResult result = util_GetLayerProperties(1, &g_layer_properties, pCount, pProperties);
    LOG_EXIT_RETURN_FUNC("EnumerateInstanceLayerProperties", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                    VkExtensionProperties *pProperties) {
    VkResult result = VK_ERROR_LAYER_NOT_PRESENT;
    LOG_ENTRY_FUNC("EnumerateInstanceExtensionProperties");
    if (pLayerName && !strcmp(pLayerName, g_layer_properties.layerName)) {
        result = util_GetExtensionProperties(0, NULL, pCount, pProperties);
    }
    LOG_EXIT_FUNC("EnumerateInstanceExtensionProperties");
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                              VkLayerProperties *pProperties) {
    LOG_ENTRY_FUNC("EnumerateDeviceLayerProperties");
    VkResult result = util_GetLayerProperties(1, &g_layer_properties, pCount, pProperties);
    LOG_EXIT_RETURN_FUNC("EnumerateDeviceLayerProperties", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                              VkInstance *pInstance) {
    LOG_ENTRY_FUNC("CreateInstance");

    // First get the instance call chain information for this layer.  This contains all the pointers
    // necessary down to the next layer (or the loader's terminator functionality) for the instance
    // level functions.
    VkLayerInstanceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    assert(fpGetInstanceProcAddr);

    // Get the create instance function for the next layer.
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (fpCreateInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain before actually calling down to the next
    // layer.  This way the chain_info is ready for that layer's information when it access the pointer.
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    // Now call create instance (and if that's successful, build the instance dispatch table)
    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS) {
        initInstanceTable(*pInstance, fpGetInstanceProcAddr);

        InstanceMapStruct *instance_map_data = new InstanceMapStruct;
        instance_map_data->dispatch_table = instance_dispatch_table(*pInstance);

        uint32_t minor_api_version = 0;
        if (pCreateInfo->pApplicationInfo != nullptr && pCreateInfo->pApplicationInfo->apiVersion != 0) {
            // We really only care about minor version
            minor_api_version = VK_VERSION_MINOR(pCreateInfo->pApplicationInfo->apiVersion);
            if (minor_api_version > 0) {
                instance_map_data->extension_enables.core_1_1 = true;
            }
            if (minor_api_version > 1) {
                instance_map_data->extension_enables.core_1_2 = true;
            }
            if (minor_api_version > 2) {
                instance_map_data->extension_enables.core_1_3 = true;
            }
        }

        VkuLayerSettingSet layerSettingSet = VK_NULL_HANDLE;
        vkuCreateLayerSettingSet("VK_LAYER_LUNARG_slow_device_simulator", vkuFindLayerSettingsCreateInfo(pCreateInfo), pAllocator,
                                 nullptr, &layerSettingSet);

        vkuSetLayerSettingCompatibilityNamespace(layerSettingSet, GetDefaultPrefix());

        // Look through extensions
        for (uint32_t ext = 0; ext < pCreateInfo->enabledExtensionCount; ++ext) {
            if (0 == strcmp(pCreateInfo->ppEnabledExtensionNames[ext], VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME)) {
                instance_map_data->extension_enables.KHR_device_group_create = true;
            }
            if (0 == strcmp(pCreateInfo->ppEnabledExtensionNames[ext], VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
                instance_map_data->extension_enables.KHR_external_mem_caps = true;
            }
            if (0 == strcmp(pCreateInfo->ppEnabledExtensionNames[ext], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                instance_map_data->extension_enables.KHR_get_phys_dev_props2 = true;
            }
        }

        // Read the type of delay to add for a fence
        if (vkuHasLayerSetting(layerSettingSet, kSettingsKeyFenceDelayType)) {
            std::string value;
            vkuGetLayerSettingValue(layerSettingSet, kSettingsKeyFenceDelayType, value);
            std::transform(value.begin(), value.end(), value.begin(), ::tolower);
            if (value == "ms_from_trigger") {
                instance_map_data->fence_delay_type = FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER;
            } else if (value == "ms_from_first_query") {
                instance_map_data->fence_delay_type = FenceDelayType::FENCE_DELAY_MS_FROM_FIRST_QUERY;
            } else if (value == "num_fail_waits") {
                instance_map_data->fence_delay_type = FenceDelayType::FENCE_DELAY_NUM_FAIL_WAITS;
            }
        }

        // Read the fence delay count (ms sometimes, or skip wait count others depending on kSettingsKeyFenceDelayType)
        if (vkuHasLayerSetting(layerSettingSet, kSettingsKeyFenceDelayCount)) {
            vkuGetLayerSettingValue(layerSettingSet, kSettingsKeyFenceDelayCount, instance_map_data->fence_delay_count);
            instance_map_data->fence_delay_count = std::max(instance_map_data->fence_delay_count, 0);
        }

        if (vkuHasLayerSetting(layerSettingSet, kSettingsKeyMemoryAdjustPercent)) {
            vkuGetLayerSettingValue(layerSettingSet, kSettingsKeyMemoryAdjustPercent, instance_map_data->memory_percent);
            instance_map_data->memory_percent = std::max(std::min(instance_map_data->memory_percent, 100), 1);
        }

        if (instance_map_data->fence_delay_type == FenceDelayType::FENCE_DELAY_NONE && instance_map_data->memory_percent == 100) {
            instance_map_data->layer_enabled = false;
        } else {
            instance_map_data->layer_enabled = true;
        }

        vkuDestroyLayerSettingSet(layerSettingSet, pAllocator);

        std::unique_lock<std::mutex> lock(g_instance_mutex);
        g_instance_map[*pInstance] = instance_map_data;
    }

    LOG_EXIT_RETURN_FUNC("CreateInstance", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("DestroyInstance");

    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(instance);
    // Loop through all physical devices and determine if it's associated with the instance
    // If so:
    //  - Loop through all devices and see if they are associated with the physical device and destroy it
    //  - Then destroy the entry for the physical device
    for (auto phys_dev_map_iter = g_phys_device_map.begin(); phys_dev_map_iter != g_phys_device_map.end();) {
        PhysDeviceMapStruct *map = phys_dev_map_iter->second;
        if (map->instance == instance) {
            VkPhysicalDevice phs_dev = phys_dev_map_iter->first;

            for (auto dev_map_iter = g_device_map.begin(); dev_map_iter != g_device_map.end();) {
                if (dev_map_iter->second->physical_device == phs_dev) {
                    DestroyDevice(dev_map_iter->first, pAllocator);
                } else {
                    dev_map_iter++;
                }
            }
            phys_dev_map_iter = g_phys_device_map.erase(phys_dev_map_iter);
            delete map;
        } else {
            phys_dev_map_iter++;
        }
    }
    EraseInstanceMapEntry(instance);

    LOG_EXIT_FUNC("DestroyInstance");
}

// ------------------------- Physical Device Functions --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                        VkPhysicalDevice *pPhysicalDevices) {
    LOG_ENTRY_FUNC("EnumeratePhysicalDevices");

    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(instance);
    VkResult result =
        instance_data_entry->dispatch_table->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (result == VK_SUCCESS && *pPhysicalDeviceCount > 0 && pPhysicalDevices) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            // Create a mapping from a physicalDevice to an instance
            if (g_phys_device_map[pPhysicalDevices[i]] == nullptr) {
                PhysDeviceMapStruct *phys_dev_data_entry = new PhysDeviceMapStruct;
                memset(&phys_dev_data_entry->props, 0, sizeof(VkPhysicalDeviceProperties));
                memset(&phys_dev_data_entry->memory_props, 0, sizeof(PhysicalDeviceMemoryBudgetProperties));
                phys_dev_data_entry->memory_percent = instance_data_entry->memory_percent;
                phys_dev_data_entry->layer_enabled = instance_data_entry->layer_enabled;
                g_phys_device_map[pPhysicalDevices[i]] = phys_dev_data_entry;
            }
            g_phys_device_map[pPhysicalDevices[i]]->instance = instance;
        }
    }

    LOG_EXIT_RETURN_FUNC("EnumeratePhysicalDevices", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount,
                                                             VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
    LOG_ENTRY_FUNC("EnumeratePhysicalDeviceGroups");

    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(instance);
    VkResult result = instance_data_entry->dispatch_table->EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount,
                                                                                         pPhysicalDeviceGroupProperties);
    if (result == VK_SUCCESS && *pPhysicalDeviceGroupCount > 0 && pPhysicalDeviceGroupProperties) {
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
            for (uint32_t j = 0; j < pPhysicalDeviceGroupProperties[i].physicalDeviceCount; j++) {
                // Create a mapping from each physicalDevice to an instance
                if (g_phys_device_map[pPhysicalDeviceGroupProperties[i].physicalDevices[j]] == nullptr) {
                    PhysDeviceMapStruct *phys_dev_data_entry = new PhysDeviceMapStruct;
                    memset(&phys_dev_data_entry->props, 0, sizeof(VkPhysicalDeviceProperties));
                    memset(&phys_dev_data_entry->memory_props, 0, sizeof(PhysicalDeviceMemoryBudgetProperties));
                    phys_dev_data_entry->memory_percent = instance_data_entry->memory_percent;
                    phys_dev_data_entry->layer_enabled = instance_data_entry->layer_enabled;
                    g_phys_device_map[pPhysicalDeviceGroupProperties[i].physicalDevices[j]] = phys_dev_data_entry;
                }
                g_phys_device_map[pPhysicalDeviceGroupProperties[i].physicalDevices[j]]->instance = instance;
            }
        }
    }

    LOG_EXIT_RETURN_FUNC("EnumeratePhysicalDeviceGroups", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount,
                                                                  VkPhysicalDeviceToolPropertiesEXT *pToolProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceToolPropertiesEXT");

    VkResult result;
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    if (phys_dev_data_entry->layer_enabled) {
        static const VkPhysicalDeviceToolPropertiesEXT memory_tracker_props = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
            nullptr,
            "Slow Device Simulator Layer",
            "1",
            VK_TOOL_PURPOSE_MODIFYING_FEATURES_BIT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT,
            "This layer intentionally slows down responses to fence waits and reduces reported memory to simulate a slow/lower-end "
            "device.",
            "VK_LAYER_LUNARG_slow_device_simulator"};

        auto original_pToolProperties = pToolProperties;
        if (pToolProperties != nullptr) {
            *pToolProperties = memory_tracker_props;
            pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
            (*pToolCount)--;
        }

        VkResult result =
            instance_data_entry->dispatch_table->GetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);
        if (original_pToolProperties != nullptr) {
            pToolProperties = original_pToolProperties;
        }
        (*pToolCount)++;

    } else {
        result =
            instance_data_entry->dispatch_table->GetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);
    }

    LOG_EXIT_RETURN_FUNC("GetPhysicalDeviceToolPropertiesEXT", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceProperties");
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceProperties(physicalDevice, pProperties);
    if (phys_dev_data_entry->layer_enabled && nullptr != pProperties) {
        memcpy(&phys_dev_data_entry->props, pProperties, sizeof(VkPhysicalDeviceProperties));
    }
    LOG_EXIT_FUNC("GetPhysicalDeviceProperties");
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceProperties2");
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceProperties2(physicalDevice, pProperties);
    if (phys_dev_data_entry->layer_enabled && nullptr != pProperties) {
        memcpy(&phys_dev_data_entry->props, &pProperties->properties, sizeof(VkPhysicalDeviceProperties));
    }
    LOG_EXIT_FUNC("GetPhysicalDeviceProperties2");
}

VkDeviceSize AdjustMemoryByPercent(VkDeviceSize in_size, uint32_t percent) {
    double double_size = static_cast<double>(in_size);
    double double_perc = static_cast<double>(percent) / 100.0;
    double adjusted_size = double_size * double_perc;
    return static_cast<VkDeviceSize>(adjusted_size);
}

void ManageMemoryProperties(PhysDeviceMapStruct *phys_dev_data_entry, VkPhysicalDeviceMemoryProperties *vulkan_props,
                            VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget_props) {
    LOG_ENTRY_FUNC("ManageMemoryProperties");
    PhysicalDeviceMemoryBudgetProperties *local_props = &phys_dev_data_entry->memory_props;
    local_props->memoryTypeCount = vulkan_props->memoryTypeCount;
    for (uint32_t type = 0; type < vulkan_props->memoryTypeCount; ++type) {
        local_props->memoryTypes[type].heapIndex = vulkan_props->memoryTypes[type].heapIndex;
        local_props->memoryTypes[type].propertyFlags = vulkan_props->memoryTypes[type].propertyFlags;
    }

    local_props->memoryHeapCount = vulkan_props->memoryHeapCount;
    for (uint32_t heap = 0; heap < vulkan_props->memoryHeapCount; ++heap) {
        // If we have a percent to adjust memory by, adjust the returned value before recording it internally
        if (phys_dev_data_entry->memory_percent < 100) {
            vulkan_props->memoryHeaps[heap].size =
                AdjustMemoryByPercent(vulkan_props->memoryHeaps[heap].size, phys_dev_data_entry->memory_percent);
            if (budget_props) {
                budget_props->heapUsage[heap] =
                    AdjustMemoryByPercent(budget_props->heapUsage[heap], phys_dev_data_entry->memory_percent);
            }
        }

        phys_dev_data_entry->memory_props.memoryHeaps[heap].size = vulkan_props->memoryHeaps[heap].size;
        local_props->memoryHeaps[heap].flags = vulkan_props->memoryHeaps[heap].flags;
        if (budget_props) {
            local_props->memoryHeaps[heap].usage = budget_props->heapUsage[heap];
            local_props->memoryHeaps[heap].budget = budget_props->heapBudget[heap];

            phys_dev_data_entry->memory_budget_updated = true;
        } else {
            local_props->memoryHeaps[heap].usage = 0;
            local_props->memoryHeaps[heap].budget = 0;
        }
    }
    LOG_EXIT_FUNC("ManageMemoryProperties");
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceMemoryProperties");
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    if (phys_dev_data_entry->layer_enabled && nullptr != pMemoryProperties) {
        ManageMemoryProperties(phys_dev_data_entry, pMemoryProperties, nullptr);
    }
    LOG_EXIT_FUNC("GetPhysicalDeviceMemoryProperties");
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceMemoryProperties2");
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
    if (phys_dev_data_entry->layer_enabled && nullptr != pMemoryProperties) {
        VkBaseOutStructure *next_chain = reinterpret_cast<VkBaseOutStructure *>(pMemoryProperties->pNext);
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *mem_budget = nullptr;
        while (next_chain != nullptr) {
            if (next_chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
                mem_budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT *>(next_chain);
                break;
            }
        }
        ManageMemoryProperties(phys_dev_data_entry, &pMemoryProperties->memoryProperties, mem_budget);
    }
    LOG_EXIT_FUNC("GetPhysicalDeviceMemoryProperties2");
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                  uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    LOG_ENTRY_FUNC("EnumerateDeviceExtensionProperties");

    if (pLayerName && !strcmp(pLayerName, g_layer_properties.layerName)) {
        result = util_GetExtensionProperties(0, NULL, pPropertyCount, pProperties);
    } else {
        assert(physicalDevice);
        PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
        if (phys_dev_data_entry != NULL) {
            InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
            result = instance_data_entry->dispatch_table->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName,
                                                                                             pPropertyCount, pProperties);
            if (phys_dev_data_entry->layer_enabled && result == VK_SUCCESS && pProperties != nullptr) {
                if (phys_dev_data_entry->props.deviceName[0] == 0) {
                    VkPhysicalDeviceProperties tempProps;
                    GetPhysicalDeviceProperties(physicalDevice, &tempProps);
                }
                uint32_t minor_api_version = VK_VERSION_MINOR(phys_dev_data_entry->props.apiVersion);
                if (instance_data_entry->extension_enables.core_1_1 && minor_api_version > 0) {
                    phys_dev_data_entry->extensions_supported.core_1_1 = true;
                }
                if (instance_data_entry->extension_enables.core_1_2 && minor_api_version > 1) {
                    phys_dev_data_entry->extensions_supported.core_1_2 = true;
                }
                if (instance_data_entry->extension_enables.core_1_3 && minor_api_version > 2) {
                    phys_dev_data_entry->extensions_supported.core_1_3 = true;
                }

                for (uint32_t prop = 0; prop < *pPropertyCount; ++prop) {
                    if (0 == strcmp(pProperties[prop].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.KHR_sync2 = true;
                    }
                    if (0 == strcmp(pProperties[prop].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.KHR_external_mem_fd = true;
                    }
                    if (0 == strcmp(pProperties[prop].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.KHR_swapchain = true;
                    }
                    if (0 == strcmp(pProperties[prop].extensionName, VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.EXT_display_control = true;
                    }
                    if (0 == strcmp(pProperties[prop].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.EXT_mem_budget = true;
                    }
                    if (0 == strcmp(pProperties[prop].extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.EXT_swapchain_maintenance1 = true;
                    }
#ifdef ANDROID
                    if (0 == strcmp(pProperties[prop].extensionName,
                                    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
                        phys_dev_data_entry->extensions_supported.ANDROID_ext_mem_hw_buf = true;
                    }
#endif
                }
            }
        }
    }
    LOG_EXIT_RETURN_FUNC("EnumerateDeviceExtensionProperties", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice,
                                                                     const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
                                                                     VkExternalBufferProperties *pExternalBufferProperties) {
    LOG_ENTRY_FUNC("GetPhysicalDeviceExternalBufferProperties");
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo,
                                                                                   pExternalBufferProperties);
    LOG_EXIT_FUNC("GetPhysicalDeviceExternalBufferProperties");
}

// ------------------------- Device Functions --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    LOG_ENTRY_FUNC("CreateDevice");
    VkResult result;
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physical_device);
    assert(phys_dev_data_entry != nullptr);

    // First get the device call chain information for this layer.  This contains all the pointers
    // necessary down to the next layer (or the loader's terminator functionality) for the device
    // level functions.
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    VkInstance instance = g_phys_device_map[physical_device]->instance;

    // Get the create device function for the next layer.
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(instance, "vkCreateDevice");
    if (fpCreateDevice == nullptr) {
        result = VK_ERROR_INITIALIZATION_FAILED;
    } else {
        // Advance the link info for the next element on the chain before actually calling down to the next
        // layer.  This way the chain_info is ready for that layer's information when it access the pointer.
        chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

        // Force on mem budge if it's available
        VkDeviceCreateInfo local_create = {};
        memcpy(&local_create, pCreateInfo, sizeof(VkDeviceCreateInfo));

        std::vector<const char *> extensions;
        if (phys_dev_data_entry->extensions_supported.core_1_1 && phys_dev_data_entry->extensions_supported.EXT_mem_budget) {
            bool enables_mem_budget = false;
            for (uint32_t ext = 0; ext < local_create.enabledExtensionCount; ++ext) {
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                    enables_mem_budget = true;
                    break;
                }
            }

            if (!enables_mem_budget) {
                const char *mem_budget_ext_name = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
                extensions.resize(local_create.enabledExtensionCount);
                memcpy(extensions.data(), local_create.ppEnabledExtensionNames,
                       sizeof(const char *) * local_create.enabledExtensionCount);
                extensions.push_back(mem_budget_ext_name);
                local_create.enabledExtensionCount = extensions.size();
                local_create.ppEnabledExtensionNames = extensions.data();
            }
        }

        // Now call create device (and if that's successful, build the device dispatch table)
        result = fpCreateDevice(physical_device, &local_create, pAllocator, pDevice);
        if (result == VK_SUCCESS) {
            InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(instance);

            DeviceMapStruct *device_map_data = new DeviceMapStruct;
            device_map_data->physical_device = physical_device;
            device_map_data->dispatch_table = new VkuDeviceDispatchTable;
            device_map_data->fence_delay_type = instance_data_entry->fence_delay_type;
            device_map_data->fence_delay_count = instance_data_entry->fence_delay_count;
            device_map_data->layer_enabled = instance_data_entry->layer_enabled;
            vkuInitDeviceDispatchTable(*pDevice, device_map_data->dispatch_table, fpGetDeviceProcAddr);

            // Look through extensions
            for (uint32_t ext = 0; ext < local_create.enabledExtensionCount; ++ext) {
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                    device_map_data->extension_enables.KHR_sync2 = true;
                }
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
                    device_map_data->extension_enables.KHR_external_mem_fd = true;
                }
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                    device_map_data->extension_enables.KHR_swapchain = true;
                }
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME)) {
                    device_map_data->extension_enables.EXT_display_control = true;
                }
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                    device_map_data->extension_enables.EXT_mem_budget = true;
                }
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
                    device_map_data->extension_enables.EXT_swapchain_maintenance1 = true;
                }
#ifdef ANDROID
                if (0 == strcmp(local_create.ppEnabledExtensionNames[ext],
                                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
                    device_map_data->extension_enables.ANDROID_ext_mem_hw_buf = true;
                }
#endif
            }

            if (phys_dev_data_entry->props.deviceName[0] == 0) {
                VkPhysicalDeviceProperties tempProps;
                GetPhysicalDeviceProperties(physical_device, &tempProps);
            }
            if (phys_dev_data_entry->memory_props.memoryHeapCount == 0) {
                if (device_map_data->extension_enables.EXT_mem_budget) {
                    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props = {};
                    budget_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
                    budget_props.pNext = nullptr;
                    VkPhysicalDeviceMemoryProperties2 mem_props2 = {};
                    mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
                    mem_props2.pNext = &budget_props;
                    GetPhysicalDeviceMemoryProperties2(device_map_data->physical_device, &mem_props2);
                } else {
                    VkPhysicalDeviceMemoryProperties tempProps;
                    GetPhysicalDeviceMemoryProperties(physical_device, &tempProps);
                }
            }

            std::unique_lock<std::mutex> lock(phys_dev_data_entry->device_mutex);
            g_device_map[*pDevice] = device_map_data;
        }
    }

    LOG_EXIT_RETURN_FUNC("CreateDevice", result);

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("DestroyDevice");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyDevice(device, pAllocator);

    EraseExternalMemFdMapEntries(device);
#ifdef ANDROID
    EraseAndroidBufferMapEntries(device);
#endif

    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
    std::unique_lock<std::mutex> lock(phys_dev_data_entry->device_mutex);
    EraseDeviceMapEntry(device);
    LOG_EXIT_FUNC("DestroyDevice");
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    LOG_ENTRY_FUNC("CreateBuffer");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        BufferMapStruct *buffer_map_data = new BufferMapStruct;
        memset(buffer_map_data, 0, sizeof(BufferMapStruct));
        buffer_map_data->device = device;
        memcpy(&buffer_map_data->create_info, pCreateInfo, sizeof(VkBufferCreateInfo));
        buffer_map_data->create_info.pNext = nullptr;

        const VkBaseInStructure *next_struct = reinterpret_cast<const VkBaseInStructure *>(pCreateInfo->pNext);
        while (next_struct != nullptr) {
            switch (next_struct->sType) {
                case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: {
                    const VkBufferOpaqueCaptureAddressCreateInfo *ci =
                        reinterpret_cast<const VkBufferOpaqueCaptureAddressCreateInfo *>(next_struct);
                    buffer_map_data->additional_info.flags |= ADD_BUFFER_VALID_OPAQUE_CAPTURE;
                    buffer_map_data->additional_info.opaque_capture_address = ci->opaqueCaptureAddress;
                    break;
                }
                case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO: {
                    const VkExternalMemoryBufferCreateInfo *ci =
                        reinterpret_cast<const VkExternalMemoryBufferCreateInfo *>(next_struct);
                    buffer_map_data->additional_info.flags |= ADD_BUFFER_VALID_EXTERNAL_MEM_HANDLE_FLAGS;
                    buffer_map_data->additional_info.external_memory_handle_flags = ci->handleTypes;
                    break;
                }
                case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: {
                    const VkBufferDeviceAddressCreateInfoEXT *ci =
                        reinterpret_cast<const VkBufferDeviceAddressCreateInfoEXT *>(next_struct);
                    buffer_map_data->additional_info.flags |= ADD_BUFFER_VALID_DEVICE_ADDRESS;
                    buffer_map_data->additional_info.device_address = ci->deviceAddress;
                    break;
                }
                default:
                    break;
            }
            next_struct = reinterpret_cast<const VkBaseInStructure *>(next_struct->pNext);
        }

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        g_buffer_map[*pBuffer] = buffer_map_data;
    }
    LOG_EXIT_RETURN_FUNC("CreateBuffer", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("DestroyBuffer");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyBuffer(device, buffer, pAllocator);

    std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
    EraseBufferMapEntry(buffer);
    LOG_EXIT_FUNC("DestroyBuffer");
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                           const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    LOG_ENTRY_FUNC("CreateImage");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->CreateImage(device, pCreateInfo, pAllocator, pImage);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        ImageMapStruct *image_map_data = new ImageMapStruct;
        memset(image_map_data, 0, sizeof(ImageMapStruct));
        image_map_data->device = device;
        memcpy(&image_map_data->create_info, pCreateInfo, sizeof(VkImageCreateInfo));
        image_map_data->create_info.pNext = nullptr;

        const VkBaseInStructure *next_struct = reinterpret_cast<const VkBaseInStructure *>(pCreateInfo->pNext);
        while (next_struct != nullptr) {
            switch (next_struct->sType) {
                case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: {
                    const VkExternalMemoryImageCreateInfo *ci =
                        reinterpret_cast<const VkExternalMemoryImageCreateInfo *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_EXTERNAL_MEM_HANDLE_FLAGS;
                    image_map_data->additional_info.external_memory_handle_flags = ci->handleTypes;
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
                    const VkImageFormatListCreateInfo *ci = reinterpret_cast<const VkImageFormatListCreateInfo *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_FORMAT_LIST;
                    for (uint32_t i = 0; i < ci->viewFormatCount; ++i) {
                        image_map_data->additional_info.format_list.push_back(ci->pViewFormats[i]);
                    }
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: {
                    const VkImageStencilUsageCreateInfo *ci = reinterpret_cast<const VkImageStencilUsageCreateInfo *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_STENCIL_USAGE;
                    image_map_data->additional_info.stencil_usage = ci->stencilUsage;
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: {
                    const VkImageSwapchainCreateInfoKHR *ci = reinterpret_cast<const VkImageSwapchainCreateInfoKHR *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_SWAPCHAIN;
                    image_map_data->additional_info.swapchain = ci->swapchain;
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: {
                    const VkImageCompressionControlEXT *ci = reinterpret_cast<const VkImageCompressionControlEXT *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_COMPRESSION_CONTROL;
                    image_map_data->additional_info.image_compress_flags = ci->flags;
                    for (uint32_t i = 0; i < ci->compressionControlPlaneCount; ++i) {
                        image_map_data->additional_info.fixed_rate_flags.push_back(ci->pFixedRateFlags[i]);
                    }
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: {
                    const VkImageDrmFormatModifierExplicitCreateInfoEXT *ci =
                        reinterpret_cast<const VkImageDrmFormatModifierExplicitCreateInfoEXT *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_DRM_FORMAT_MOD_EXPLICIT;
                    image_map_data->additional_info.drm_format_modifier = ci->drmFormatModifier;
                    for (uint32_t i = 0; i < ci->drmFormatModifierPlaneCount; ++i) {
                        image_map_data->additional_info.plane_layouts.push_back(ci->pPlaneLayouts[i]);
                    }
                    break;
                }
                case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: {
                    const VkImageDrmFormatModifierListCreateInfoEXT *ci =
                        reinterpret_cast<const VkImageDrmFormatModifierListCreateInfoEXT *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_DRM_FORMAT_MOD_LIST;
                    for (uint32_t i = 0; i < ci->drmFormatModifierCount; ++i) {
                        image_map_data->additional_info.drm_format_modifiers.push_back(ci->pDrmFormatModifiers[i]);
                    }
                    break;
                }
#ifdef ANDROID
                case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID: {
                    const VkExternalFormatANDROID *ci = reinterpret_cast<const VkExternalFormatANDROID *>(next_struct);
                    image_map_data->additional_info.flags |= ADD_IMAGE_VALID_EXTERNAL_FORMAT_ANDROID;
                    image_map_data->additional_info.external_android_format = ci->externalFormat;
                    break;
                }
#endif
                default:
                    break;
            }
            next_struct = reinterpret_cast<const VkBaseInStructure *>(next_struct->pNext);
        }

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        g_image_map[*pImage] = image_map_data;
    }
    LOG_EXIT_RETURN_FUNC("CreateImage", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("DestroyImage");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyImage(device, image, pAllocator);

    std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
    EraseImageMapEntry(image);
    LOG_EXIT_FUNC("DestroyImage");
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                       VkMemoryRequirements *pMemoryRequirements) {
    LOG_ENTRY_FUNC("GetBufferMemoryRequirements");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetBufferMemoryRequirements(device, buffer, pMemoryRequirements);

    if (device_map_data->layer_enabled) {
        BufferMapStruct *buffer_map_data = GetBufferMapEntry(buffer);
        assert(buffer_map_data);
        assert(buffer_map_data->device == device);

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        memcpy(&buffer_map_data->memory_reqs, pMemoryRequirements, sizeof(VkMemoryRequirements));
    }
    LOG_EXIT_FUNC("GetBufferMemoryRequirements");
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
                                                        VkMemoryRequirements2 *pMemoryRequirements) {
    LOG_ENTRY_FUNC("GetBufferMemoryRequirements2");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);

    if (device_map_data->layer_enabled) {
        BufferMapStruct *buffer_map_data = GetBufferMapEntry(pInfo->buffer);
        assert(buffer_map_data);
        assert(buffer_map_data->device == device);

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        memcpy(&buffer_map_data->memory_reqs, &pMemoryRequirements->memoryRequirements, sizeof(VkMemoryRequirements));
    }
    LOG_EXIT_FUNC("GetBufferMemoryRequirements2");
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements) {
    LOG_ENTRY_FUNC("GetImageMemoryRequirements");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetImageMemoryRequirements(device, image, pMemoryRequirements);

    if (device_map_data->layer_enabled) {
        ImageMapStruct *image_map_data = GetImageMapEntry(image);
        assert(image_map_data);
        assert(image_map_data->device == device);

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        memcpy(&image_map_data->memory_reqs, pMemoryRequirements, sizeof(VkMemoryRequirements));
    }
    LOG_EXIT_FUNC("GetImageMemoryRequirements");
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
                                                       VkMemoryRequirements2 *pMemoryRequirements) {
    LOG_ENTRY_FUNC("GetImageMemoryRequirements2");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);

    if (device_map_data->layer_enabled) {
        ImageMapStruct *image_map_data = GetImageMapEntry(pInfo->image);
        assert(image_map_data);
        assert(image_map_data->device == device);

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        memcpy(&image_map_data->memory_reqs, &pMemoryRequirements->memoryRequirements, sizeof(VkMemoryRequirements));
    }
    LOG_EXIT_FUNC("GetImageMemoryRequirements2");
}

VKAPI_ATTR VkResult VKAPI_CALL GetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, int fd,
                                                        VkMemoryFdPropertiesKHR *pMemoryFdProperties) {
    LOG_ENTRY_FUNC("GetMemoryFdPropertiesKHR");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->GetMemoryFdPropertiesKHR(device, handleType, fd, pMemoryFdProperties);
    if (device_map_data->layer_enabled && result == VK_SUCCESS && pMemoryFdProperties != nullptr) {
        ExternalMemFdMapStruct *ext_mem_fd_map_data = new ExternalMemFdMapStruct;
        memset(ext_mem_fd_map_data, 0, sizeof(ExternalMemFdMapStruct));
        ext_mem_fd_map_data->device = device;
        ext_mem_fd_map_data->memory_type = pMemoryFdProperties->memoryTypeBits;
        g_ext_mem_fd_map[static_cast<uint64_t>(fd)] = ext_mem_fd_map_data;
    }
    LOG_EXIT_RETURN_FUNC("GetMemoryFdPropertiesKHR", result);
    return result;
}

#ifdef ANDROID
VKAPI_ATTR VkResult VKAPI_CALL GetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const AHardwareBuffer *buffer,
                                                                         VkAndroidHardwareBufferPropertiesANDROID *pProperties) {
    LOG_ENTRY_FUNC("GetAndroidHardwareBufferPropertiesANDROID");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->GetAndroidHardwareBufferPropertiesANDROID(device, buffer, pProperties);
    if (device_map_data->layer_enabled && result == VK_SUCCESS && pProperties != nullptr) {
        AndroidBufferMapStruct *and_buf_map_data = new AndroidBufferMapStruct;
        memset(and_buf_map_data, 0, sizeof(AndroidBufferMapStruct));
        and_buf_map_data->device = device;
        and_buf_map_data->alloc_size = pProperties->allocationSize;
        and_buf_map_data->memory_type = pProperties->memoryTypeBits;
        g_android_buf_map[buffer] = and_buf_map_data;
    }
    LOG_EXIT_RETURN_FUNC("GetAndroidHardwareBufferPropertiesANDROID", result);
    return result;
}
#endif  // ANDROID

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    LOG_ENTRY_FUNC("AllocateMemory");
    VkResult result = VK_SUCCESS;
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    PhysDeviceMapStruct *phys_device_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;

    // Check amount of memory available and return VK_ERROR_OUT_OF_DEVICE_MEMORY if not enough is present
    if (device_map_data->layer_enabled) {
        if (pAllocateInfo != nullptr && phys_device_map_data->memory_percent < 100) {
            uint32_t heap = phys_device_map_data->memory_props.memoryTypes[pAllocateInfo->memoryTypeIndex].heapIndex;
            MemoryHeapWithBudget *budget_heap = &phys_device_map_data->memory_props.memoryHeaps[heap];
            if (budget_heap->budget > 0) {
                VkDeviceSize potential_alloc = budget_heap->allocated + pAllocateInfo->allocationSize;
                if (potential_alloc > budget_heap->budget) {
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
            } else {
                VkDeviceSize potential_alloc = budget_heap->allocated + pAllocateInfo->allocationSize;
                if (potential_alloc > budget_heap->size) {
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
            }
        }
    }

    if (result == VK_SUCCESS) {
        result = pDisp->AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
        if (device_map_data->layer_enabled && result == VK_SUCCESS) {
            MemoryMapStruct *memory_map_data = new MemoryMapStruct;
            memset(memory_map_data, 0, sizeof(MemoryMapStruct));
            memory_map_data->device = device;
            memcpy(&memory_map_data->alloc_info, pAllocateInfo, sizeof(VkMemoryAllocateInfo));
            memory_map_data->alloc_info.pNext = nullptr;

            const VkBaseInStructure *next_struct = reinterpret_cast<const VkBaseInStructure *>(pAllocateInfo->pNext);
            while (next_struct != nullptr) {
                switch (next_struct->sType) {
                    case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO: {
                        const VkExportMemoryAllocateInfo *ci = reinterpret_cast<const VkExportMemoryAllocateInfo *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_EXTERNAL_MEM_HANDLE_FLAGS;
                        memory_map_data->additional_info.external_memory_handle_flags = ci->handleTypes;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: {
                        const VkMemoryDedicatedAllocateInfo *ci =
                            reinterpret_cast<const VkMemoryDedicatedAllocateInfo *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_DEDICATED_ALLOC;
                        memory_map_data->additional_info.dedicated_image = ci->image;
                        memory_map_data->additional_info.dedicated_buffer = ci->buffer;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: {
                        const VkMemoryAllocateFlagsInfo *ci = reinterpret_cast<const VkMemoryAllocateFlagsInfo *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_ALLOCATE_FLAG_INFO;
                        memory_map_data->additional_info.memory_alloc_flags = ci->flags;
                        memory_map_data->additional_info.memory_alloc_device_mask = ci->deviceMask;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: {
                        const VkMemoryOpaqueCaptureAddressAllocateInfo *ci =
                            reinterpret_cast<const VkMemoryOpaqueCaptureAddressAllocateInfo *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_OPAQUE_CAPTURE_ADDRESS;
                        memory_map_data->additional_info.opaque_capture_address = ci->opaqueCaptureAddress;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR: {
                        const VkImportMemoryFdInfoKHR *ci = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_EXTERNAL_MEM_FD;
                        memory_map_data->additional_info.ext_memory_fd_handle_type = ci->handleType;
                        memory_map_data->additional_info.ext_memory_fd = ci->fd;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT: {
                        const VkImportMemoryHostPointerInfoEXT *ci =
                            reinterpret_cast<const VkImportMemoryHostPointerInfoEXT *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_IMPORT_HOST_POINTER;
                        memory_map_data->additional_info.import_host_ptr_handle_type = ci->handleType;
                        memory_map_data->additional_info.import_host_ptr = ci->pHostPointer;
                        break;
                    }
                    case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: {
                        const VkMemoryPriorityAllocateInfoEXT *ci =
                            reinterpret_cast<const VkMemoryPriorityAllocateInfoEXT *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_PRIORITY;
                        memory_map_data->additional_info.memory_priority = ci->priority;
                        break;
                    }
#ifdef ANDROID
                    case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID: {
                        const VkImportAndroidHardwareBufferInfoANDROID *ci =
                            reinterpret_cast<const VkImportAndroidHardwareBufferInfoANDROID *>(next_struct);
                        memory_map_data->additional_info.flags |= ADD_MEM_VALID_ANDROID_HARDWARE_BUFFER;
                        memory_map_data->additional_info.android_hw_buffer = ci->buffer;
                        break;
                    }
#endif
                    default:
                        break;
                }

                next_struct = reinterpret_cast<const VkBaseInStructure *>(next_struct->pNext);
            }

            std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
            g_memory_map[*pMemory] = memory_map_data;

            if (pAllocateInfo != nullptr && phys_device_map_data->memory_percent < 100) {
                uint32_t heap = phys_device_map_data->memory_props.memoryTypes[pAllocateInfo->memoryTypeIndex].heapIndex;
                MemoryHeapWithBudget *budget_heap = &phys_device_map_data->memory_props.memoryHeaps[heap];
                budget_heap->allocated += pAllocateInfo->allocationSize;
            }
        }
    }

    LOG_EXIT_RETURN_FUNC("AllocateMemory", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("FreeMemory");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->FreeMemory(device, memory, pAllocator);

    if (device_map_data->layer_enabled) {
        PhysDeviceMapStruct *phys_device_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        if (phys_device_map_data->memory_percent < 100) {
            MemoryMapStruct *memory_map_data = g_memory_map[memory];
            uint32_t heap = phys_device_map_data->memory_props.memoryTypes[memory_map_data->alloc_info.memoryTypeIndex].heapIndex;
            MemoryHeapWithBudget *budget_heap = &phys_device_map_data->memory_props.memoryHeaps[heap];
            budget_heap->allocated -= memory_map_data->alloc_info.allocationSize;
        }

        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);
        EraseMemoryMapEntry(memory);
    }
    LOG_EXIT_FUNC("FreeMemory");
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                VkDeviceSize memoryOffset) {
    LOG_ENTRY_FUNC("BindBufferMemory");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindBufferMemory(device, buffer, memory, memoryOffset);
    if (device_map_data->layer_enabled && result == VK_SUCCESS && buffer != VK_NULL_HANDLE) {
        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);

        // Make sure it's not associated with a memory allocation already
        for (auto &mem_map_iter : g_memory_map) {
            MemoryMapStruct *map = mem_map_iter.second;
            for (auto buf_iter = map->buffers.begin(); buf_iter != map->buffers.end();) {
                if (buf_iter->buffer == buffer) {
                    buf_iter = map->buffers.erase(buf_iter);
                } else {
                    buf_iter++;
                }
            }
        }

        if (memory != VK_NULL_HANDLE) {
            MemoryMapStruct *memory_map_data = GetMemoryMapEntry(memory);
            assert(memory_map_data->device == device);
            BufferMemoryStruct buffer_data{buffer, memoryOffset};
            memory_map_data->buffers.push_back(buffer_data);
        }
        device_map_data->memory_bindings_updated = true;

        PhysDeviceMapStruct *phys_dev_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        assert(phys_dev_map_data);
        phys_dev_map_data->memory_budget_updated = false;
    }
    LOG_EXIT_RETURN_FUNC("BindBufferMemory", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    LOG_ENTRY_FUNC("BindImageMemory");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindImageMemory(device, image, memory, memoryOffset);
    if (device_map_data->layer_enabled && result == VK_SUCCESS && image != VK_NULL_HANDLE) {
        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);

        // Make sure it's not associated with a memory allocation already
        for (auto &mem_map_iter : g_memory_map) {
            MemoryMapStruct *map = mem_map_iter.second;
            for (auto img_iter = map->images.begin(); img_iter != map->images.end();) {
                if (img_iter->image == image) {
                    img_iter = map->images.erase(img_iter);
                } else {
                    img_iter++;
                }
            }
        }

        if (memory != VK_NULL_HANDLE) {
            MemoryMapStruct *memory_map_data = GetMemoryMapEntry(memory);
            assert(memory_map_data->device == device);
            ImageMemoryStruct image_data{image, memoryOffset};
            memory_map_data->images.push_back(image_data);
        }
        device_map_data->memory_bindings_updated = true;

        PhysDeviceMapStruct *phys_dev_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        assert(phys_dev_map_data);
        phys_dev_map_data->memory_budget_updated = false;
    }
    LOG_EXIT_RETURN_FUNC("BindImageMemory", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                                                 const VkBindBufferMemoryInfo *pBindInfos) {
    LOG_ENTRY_FUNC("BindBufferMemory2");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindBufferMemory2(device, bindInfoCount, pBindInfos);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);

        for (uint32_t buf = 0; buf < bindInfoCount; ++buf) {
            if (pBindInfos[buf].buffer != VK_NULL_HANDLE) {
                // Make sure it's not associated with a memory allocation already
                for (auto &mem_map_iter : g_memory_map) {
                    MemoryMapStruct *map = mem_map_iter.second;
                    for (auto buf_iter = map->buffers.begin(); buf_iter != map->buffers.end();) {
                        if (buf_iter->buffer == pBindInfos[buf].buffer) {
                            buf_iter = map->buffers.erase(buf_iter);
                        } else {
                            buf_iter++;
                        }
                    }
                }

                if (pBindInfos[buf].memory != VK_NULL_HANDLE) {
                    MemoryMapStruct *memory_map_data = GetMemoryMapEntry(pBindInfos[buf].memory);
                    assert(memory_map_data->device == device);
                    BufferMemoryStruct buffer_data{pBindInfos[buf].buffer, pBindInfos[buf].memoryOffset};
                    memory_map_data->buffers.push_back(buffer_data);
                }
            }
        }
        device_map_data->memory_bindings_updated = true;

        PhysDeviceMapStruct *phys_dev_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        assert(phys_dev_map_data);
        phys_dev_map_data->memory_budget_updated = false;
    }
    LOG_EXIT_RETURN_FUNC("BindBufferMemory2", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos) {
    LOG_ENTRY_FUNC("BindImageMemory2");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindImageMemory2(device, bindInfoCount, pBindInfos);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        std::unique_lock<std::mutex> lock(device_map_data->memory_mutex);

        for (uint32_t img = 0; img < bindInfoCount; ++img) {
            if (pBindInfos[img].image != VK_NULL_HANDLE) {
                // Make sure it's not associated with a memory allocation already
                for (auto &mem_map_iter : g_memory_map) {
                    MemoryMapStruct *map = mem_map_iter.second;
                    for (auto img_iter = map->images.begin(); img_iter != map->images.end();) {
                        if (img_iter->image == pBindInfos[img].image) {
                            img_iter = map->images.erase(img_iter);
                        } else {
                            img_iter++;
                        }
                    }
                }

                if (pBindInfos[img].memory != VK_NULL_HANDLE) {
                    MemoryMapStruct *memory_map_data = GetMemoryMapEntry(pBindInfos[img].memory);
                    assert(memory_map_data->device == device);
                    ImageMemoryStruct image_data{pBindInfos[img].image, pBindInfos[img].memoryOffset};

                    const VkBaseInStructure *next_struct = reinterpret_cast<const VkBaseInStructure *>(pBindInfos[img].pNext);
                    while (next_struct != nullptr) {
                        switch (next_struct->sType) {
                            case VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO: {
                                const VkBindImagePlaneMemoryInfo *ci =
                                    reinterpret_cast<const VkBindImagePlaneMemoryInfo *>(next_struct);
                                image_data.additional_info.flags |= ADD_IMAGE_MEM_VALID_PLANE_MEM;
                                image_data.additional_info.plane_mem_aspect = ci->planeAspect;
                                break;
                            }
                            case VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR: {
                                const VkBindImageMemorySwapchainInfoKHR *ci =
                                    reinterpret_cast<const VkBindImageMemorySwapchainInfoKHR *>(next_struct);
                                image_data.additional_info.flags |= ADD_IMAGE_MEM_VALID_SWAPCHAIN;
                                image_data.additional_info.swapchain = ci->swapchain;
                                image_data.additional_info.swapchain_image_index = ci->imageIndex;
                                break;
                            }
                            default:
                                break;
                        }
                        next_struct = reinterpret_cast<const VkBaseInStructure *>(next_struct->pNext);
                    }

                    memory_map_data->images.push_back(image_data);
                }
            }
        }
        device_map_data->memory_bindings_updated = true;

        PhysDeviceMapStruct *phys_dev_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        assert(phys_dev_map_data);
        phys_dev_map_data->memory_budget_updated = false;
    }
    LOG_EXIT_RETURN_FUNC("BindImageMemory2", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo,
                                           const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    LOG_ENTRY_FUNC("CreateFence");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->CreateFence(device, pCreateInfo, pAllocator, pFence);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        FenceMapStruct *fence_map_data = new FenceMapStruct;
        fence_map_data->device = device;
        fence_map_data->signalled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
        fence_map_data->delay_type = device_map_data->fence_delay_type;
        fence_map_data->delay_count = device_map_data->fence_delay_count;
        fence_map_data->wait_started = false;
        fence_map_data->wait_completed = false;
        fence_map_data->elapsed_count = 0;
        std::unique_lock<std::mutex> lock(device_map_data->fence_mutex);
        g_fence_map[*pFence] = fence_map_data;
    }
    LOG_EXIT_RETURN_FUNC("CreateFence", result);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator) {
    LOG_ENTRY_FUNC("DestroyFence");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyFence(device, fence, pAllocator);

    if (device_map_data->layer_enabled) {
        std::unique_lock<std::mutex> lock(device_map_data->fence_mutex);
        EraseFenceMapEntry(fence);
    }
    LOG_EXIT_FUNC("DestroyFence");
}

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    LOG_ENTRY_FUNC("ResetFences");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    if (device_map_data->layer_enabled) {
        for (uint32_t ii = 0; ii < fenceCount; ++ii) {
            FenceMapStruct *fence_map_data = g_fence_map[pFences[ii]];
            if (fence_map_data == nullptr) {
                continue;
            }
            fence_map_data->signalled = false;
            fence_map_data->wait_started = false;
            fence_map_data->wait_completed = false;
            fence_map_data->elapsed_count = 0;
        }
    }
    VkResult result = pDisp->ResetFences(device, fenceCount, pFences);
    LOG_EXIT_RETURN_FUNC("ResetFences", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice device, VkFence fence) {
    LOG_ENTRY_FUNC("GetFenceStatus");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    if (device_map_data->layer_enabled) {
        FenceMapStruct *fence_map_data = g_fence_map[fence];
        if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
            switch (fence_map_data->delay_type) {
                case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                    auto cur_time = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(cur_time - fence_map_data->start_time);
                    fence_map_data->elapsed_count += duration.count();
                    break;
                }
                case FenceDelayType::FENCE_DELAY_MS_FROM_FIRST_QUERY: {
                    if (!fence_map_data->wait_started) {
                        fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                    } else {
                        auto cur_time = std::chrono::high_resolution_clock::now(

                        );
                        auto duration =
                            std::chrono::duration_cast<std::chrono::milliseconds>(cur_time - fence_map_data->start_time);
                        fence_map_data->elapsed_count += duration.count();
                    }
                    break;
                }
                case FenceDelayType::FENCE_DELAY_NUM_FAIL_WAITS:
                    fence_map_data->elapsed_count++;
                    break;
                default:
                    break;
            }
            if (!fence_map_data->wait_started) {
                fence_map_data->wait_started = true;
            }
            if (!fence_map_data->signalled || fence_map_data->delay_count > fence_map_data->elapsed_count) {
                return VK_NOT_READY;
            } else {
                fence_map_data->wait_completed = true;
            }
        }
    }
    VkResult result = pDisp->GetFenceStatus(device, fence);
    LOG_EXIT_RETURN_FUNC("GetFenceStatus", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll,
                                             uint64_t timeout) {
    LOG_ENTRY_FUNC("WaitForFences");
    VkResult result;
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    if (device_map_data->layer_enabled) {
        std::vector<VkFence> wait_for_fences;
        // Take into account timeout when determining wait times because if the fence wait time would have completed in the
        // timeframe remaining, then treat it as if that period passed.
        uint32_t milliseconds_till_timeout = static_cast<uint32_t>(timeout / 1000000);
        for (uint32_t ii = 0; ii < fenceCount; ++ii) {
            FenceMapStruct *fence_map_data = g_fence_map[pFences[ii]];
            if (fence_map_data != nullptr && fence_map_data->signalled &&
                fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
                bool can_timeout = true;
                bool can_sleep = false;
                uint32_t sleep_time = 0;
                switch (fence_map_data->delay_type) {
                    case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                        auto cur_time = std::chrono::high_resolution_clock::now();
                        auto duration =
                            std::chrono::duration_cast<std::chrono::milliseconds>(cur_time - fence_map_data->start_time);
                        fence_map_data->elapsed_count += duration.count();
                        if (milliseconds_till_timeout > 0 && fence_map_data->elapsed_count < fence_map_data->delay_count) {
                            can_sleep = true;
                            sleep_time = fence_map_data->delay_count - fence_map_data->elapsed_count;
                        }
                        break;
                    }
                    case FenceDelayType::FENCE_DELAY_MS_FROM_FIRST_QUERY: {
                        if (!fence_map_data->wait_started) {
                            fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                        } else {
                            auto cur_time = std::chrono::high_resolution_clock::now();
                            auto duration =
                                std::chrono::duration_cast<std::chrono::milliseconds>(cur_time - fence_map_data->start_time);
                            fence_map_data->elapsed_count += duration.count();
                        }
                        if (milliseconds_till_timeout > 0 && fence_map_data->elapsed_count < fence_map_data->delay_count) {
                            can_sleep = true;
                            sleep_time = fence_map_data->delay_count - fence_map_data->elapsed_count;
                        }
                        break;
                    }
                    case FenceDelayType::FENCE_DELAY_NUM_FAIL_WAITS:
                        if (timeout >= 1000000000) {
                            // If we have a huge timeout, like > 1 second, do something to pretend like we're slower like
                            // delaying for 10 milliseconds for every wait count.
                            can_timeout = false;
                            can_sleep = true;
                            sleep_time = fence_map_data->delay_count * 10;
                        } else {
                            fence_map_data->elapsed_count++;
                        }
                        break;
                    default:
                        break;
                }

                if (!fence_map_data->wait_started) {
                    fence_map_data->wait_started = true;
                }

                uint32_t total_max_elapsed_count = fence_map_data->elapsed_count;
                if (can_sleep) {
                    total_max_elapsed_count += milliseconds_till_timeout;
                }
                if (can_timeout && (!fence_map_data->signalled || fence_map_data->delay_count > total_max_elapsed_count)) {
                    // If we're waiting for all, and only one is not ready by our delay, return not ready.
                    // Otherwise, we just don't add it to the wait list and see if any of the other fences
                    // we are looping through are ready.
                    if (waitAll == VK_TRUE) {
                        return VK_TIMEOUT;
                    }
                } else {
                    if (can_sleep && sleep_time != 0) {
                        // Need to sleep the diff
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
                    }

                    fence_map_data->wait_completed = true;
                    wait_for_fences.push_back(pFences[ii]);
                }
            } else {
                // fence_map_data == nullptr or delay type == FENCE_DELAY_NONE
                wait_for_fences.push_back(pFences[ii]);
            }
        }
        // If no fences were ready because of a delay (even if not waitAll) return not ready
        if (fenceCount > 0 && wait_for_fences.size() == 0) {
            result = VK_TIMEOUT;
        } else {
            result = pDisp->WaitForFences(device, wait_for_fences.size(), wait_for_fences.data(), waitAll, timeout);
        }
    } else {
        result = pDisp->WaitForFences(device, fenceCount, pFences, waitAll, timeout);
    }
    LOG_EXIT_RETURN_FUNC("WaitForFences", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
    LOG_ENTRY_FUNC("AcquireNextImageKHR");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);

    // If a fence is there, and we need to signal it, do so.
    if (device_map_data->layer_enabled && fence != VK_NULL_HANDLE) {
        FenceMapStruct *fence_map_data = g_fence_map[fence];
        if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
            switch (fence_map_data->delay_type) {
                case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                    fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                    break;
                }
                default:
                    break;
            }

            fence_map_data->signalled = true;
        }
    }

    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    LOG_EXIT_RETURN_FUNC("AcquireNextImageKHR", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
                                                    uint32_t *pImageIndex) {
    LOG_ENTRY_FUNC("AcquireNextImage2KHR");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);

    // If a fence is there, and we need to signal it, do so.
    if (device_map_data->layer_enabled && pAcquireInfo != nullptr && pAcquireInfo->fence != VK_NULL_HANDLE) {
        FenceMapStruct *fence_map_data = g_fence_map[pAcquireInfo->fence];
        if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
            switch (fence_map_data->delay_type) {
                case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                    fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                    break;
                }
                default:
                    break;
            }

            fence_map_data->signalled = true;
        }
    }

    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    LOG_EXIT_RETURN_FUNC("AcquireNextImage2KHR", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL RegisterDeviceEventEXT(VkDevice device, const VkDeviceEventInfoEXT *pDeviceEventInfo,
                                                      const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    LOG_ENTRY_FUNC("RegisterDeviceEventEXT");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->RegisterDeviceEventEXT(device, pDeviceEventInfo, pAllocator, pFence);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        FenceMapStruct *fence_map_data = new FenceMapStruct;
        fence_map_data->device = device;
        fence_map_data->signalled = false;
        fence_map_data->delay_type = device_map_data->fence_delay_type;
        fence_map_data->delay_count = device_map_data->fence_delay_count;
        fence_map_data->wait_started = false;
        fence_map_data->wait_completed = false;
        fence_map_data->elapsed_count = 0;
        std::unique_lock<std::mutex> lock(device_map_data->fence_mutex);
        g_fence_map[*pFence] = fence_map_data;
    }
    LOG_EXIT_RETURN_FUNC("RegisterDeviceEventEXT", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL RegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display,
                                                       const VkDisplayEventInfoEXT *pDisplayEventInfo,
                                                       const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    LOG_ENTRY_FUNC("RegisterDisplayEventEXT");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->RegisterDisplayEventEXT(device, display, pDisplayEventInfo, pAllocator, pFence);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        FenceMapStruct *fence_map_data = new FenceMapStruct;
        fence_map_data->device = device;
        fence_map_data->signalled = false;
        fence_map_data->delay_type = device_map_data->fence_delay_type;
        fence_map_data->delay_count = device_map_data->fence_delay_count;
        fence_map_data->wait_started = false;
        fence_map_data->wait_completed = false;
        fence_map_data->elapsed_count = 0;
        std::unique_lock<std::mutex> lock(device_map_data->fence_mutex);
        g_fence_map[*pFence] = fence_map_data;
    }
    LOG_EXIT_RETURN_FUNC("RegisterDisplayEventEXT", result);
    return result;
}

// ------------------------- Queue Functions --------------------------------------

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    LOG_ENTRY_FUNC("GetDeviceQueue");
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    g_queue_to_device_map[*pQueue] = device;
    LOG_EXIT_FUNC("GetDeviceQueue");
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo,
                                               VkFence fence) {
    LOG_ENTRY_FUNC("QueueBindSparse");
    VkDevice device = g_queue_to_device_map[queue];
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);

    // If a fence is there, and we need to signal it, do so.
    if (device_map_data->layer_enabled && fence != VK_NULL_HANDLE) {
        FenceMapStruct *fence_map_data = g_fence_map[fence];
        if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
            switch (fence_map_data->delay_type) {
                case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                    fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                    break;
                }
                default:
                    break;
            }

            fence_map_data->signalled = true;
        }
    }

    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->QueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
    LOG_EXIT_RETURN_FUNC("QueueBindSparse", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    LOG_ENTRY_FUNC("QueueSubmit");
    VkDevice device = g_queue_to_device_map[queue];
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->QueueSubmit(queue, submitCount, pSubmits, fence);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        // If a fence is there, and we need to signal it, do so.
        if (fence != VK_NULL_HANDLE) {
            FenceMapStruct *fence_map_data = g_fence_map[fence];
            if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
                switch (fence_map_data->delay_type) {
                    case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                        fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                        break;
                    }
                    default:
                        break;
                }

                fence_map_data->signalled = true;
            }
        }

        PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        if (device_map_data->memory_bindings_updated) {
            if (!phys_dev_data_entry->memory_budget_updated) {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props = {};
                budget_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
                budget_props.pNext = nullptr;
                VkPhysicalDeviceMemoryProperties2 mem_props2 = {};
                mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
                mem_props2.pNext = &budget_props;
                GetPhysicalDeviceMemoryProperties2(device_map_data->physical_device, &mem_props2);
            }

            device_map_data->memory_bindings_updated = false;
        }
    }
    LOG_EXIT_RETURN_FUNC("QueueSubmit", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    LOG_ENTRY_FUNC("QueueSubmit2");
    VkDevice device = g_queue_to_device_map[queue];
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->QueueSubmit2(queue, submitCount, pSubmits, fence);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        // If a fence is there, and we need to signal it, do so.
        if (fence != VK_NULL_HANDLE) {
            FenceMapStruct *fence_map_data = g_fence_map[fence];
            if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
                switch (fence_map_data->delay_type) {
                    case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                        fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                        break;
                    }
                    default:
                        break;
                }

                fence_map_data->signalled = true;
            }
        }

        PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        if (device_map_data->memory_bindings_updated) {
            if (!phys_dev_data_entry->memory_budget_updated) {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props = {};
                budget_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
                budget_props.pNext = nullptr;
                VkPhysicalDeviceMemoryProperties2 mem_props2 = {};
                mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
                mem_props2.pNext = &budget_props;
                GetPhysicalDeviceMemoryProperties2(device_map_data->physical_device, &mem_props2);
            }

            device_map_data->memory_bindings_updated = false;
        }
    }
    LOG_EXIT_RETURN_FUNC("QueueSubmit2", result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    LOG_ENTRY_FUNC("QueuePresentKHR");
    VkDevice device = g_queue_to_device_map[queue];
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkuDeviceDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->QueuePresentKHR(queue, pPresentInfo);
    if (device_map_data->layer_enabled && result == VK_SUCCESS) {
        const VkBaseInStructure *cur_struct = reinterpret_cast<const VkBaseInStructure *>(pPresentInfo->pNext);
        while (cur_struct != nullptr) {
            // If a structure that might contain a fence is there, and we need to signal it, do so.
            if (cur_struct->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT &&
                device_map_data->extension_enables.EXT_swapchain_maintenance1) {
                const VkSwapchainPresentFenceInfoEXT *actual_struct =
                    reinterpret_cast<const VkSwapchainPresentFenceInfoEXT *>(cur_struct);
                for (uint32_t ii = 0; ii < actual_struct->swapchainCount; ++ii) {
                    if (actual_struct->pFences[ii] != VK_NULL_HANDLE) {
                        FenceMapStruct *fence_map_data = g_fence_map[actual_struct->pFences[ii]];
                        if (fence_map_data != nullptr && fence_map_data->delay_type != FenceDelayType::FENCE_DELAY_NONE) {
                            switch (fence_map_data->delay_type) {
                                case FenceDelayType::FENCE_DELAY_MS_FROM_TRIGGER: {
                                    fence_map_data->start_time = std::chrono::high_resolution_clock::now();
                                    break;
                                }
                                default:
                                    break;
                            }

                            fence_map_data->signalled = true;
                        }
                    }
                }
            }
            cur_struct = reinterpret_cast<const VkBaseInStructure *>(pPresentInfo->pNext);
        }
    }
    LOG_EXIT_RETURN_FUNC("QueuePresentKHR", result);
    return result;
}

// ------------------------- Interface Functions --------------------------------------

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *funcName) {
    PFN_vkVoidFunction proc = ImplementedInstanceCommands(funcName);
    if (proc) {
        return proc;
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    InstanceMapStruct *instance_map_data = GetInstanceMapEntry(instance);
    if (instance_map_data == VK_NULL_HANDLE) {
        return nullptr;
    }
    proc = ImplementedInstanceNewerCoreCommands(instance_map_data, funcName);
    if (proc) {
        return proc;
    }
    proc = ImplementedInstanceExtensionCommands(instance_map_data, funcName);
    if (proc) {
        return proc;
    }

    proc = ImplementedDeviceCommands(funcName);
    if (proc) {
        return proc;
    }
    proc = ImplementedDeviceExtensionCommands(nullptr, funcName);
    if (proc) {
        return proc;
    }

    if (instance_map_data->dispatch_table == nullptr || instance_map_data->dispatch_table->GetInstanceProcAddr == nullptr) {
        return nullptr;
    }
    proc = instance_map_data->dispatch_table->GetInstanceProcAddr(instance, funcName);
    return proc;
}

static PFN_vkVoidFunction ImplementedInstanceCommands(const char *name) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction proc;
    } base_instance_commands[] = {
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice)},
        {"vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDevices)},
        {"vkEnumerateInstanceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceLayerProperties)},
        {"vkEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceExtensionProperties)},
        {"vkEnumerateDeviceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceLayerProperties)},
        {"vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties)},
        {"vkGetPhysicalDeviceProperties", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties)},
        {"vkGetPhysicalDeviceMemoryProperties", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceMemoryProperties)},
        {"vkGetPhysicalDeviceToolPropertiesEXT", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceToolPropertiesEXT)}};

    for (size_t i = 0; i < ARRAY_SIZE(base_instance_commands); i++) {
        if (!strcmp(base_instance_commands[i].name, name)) return base_instance_commands[i].proc;
    }

    return nullptr;
}

static PFN_vkVoidFunction ImplementedInstanceNewerCoreCommands(InstanceMapStruct *instance_map_data, const char *name) {
    if (instance_map_data->extension_enables.core_1_1) {
        static const struct {
            const char *name;
            PFN_vkVoidFunction proc;
        } version_instance_commands[] = {
            {"vkEnumeratePhysicalDeviceGroups2", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDeviceGroups)},
            {"vkGetPhysicalDeviceProperties2", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2)},
            {"vkGetPhysicalDeviceMemoryProperties2", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceMemoryProperties2)},
            {"vkGetPhysicalDeviceExternalBufferProperties",
             reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceExternalBufferProperties)}};
        for (size_t i = 0; i < ARRAY_SIZE(version_instance_commands); i++) {
            if (!strcmp(version_instance_commands[i].name, name)) return version_instance_commands[i].proc;
        }
    }

    return nullptr;
}

static PFN_vkVoidFunction ImplementedInstanceExtensionCommands(InstanceMapStruct *instance_map_data, const char *name) {
    if (instance_map_data->extension_enables.KHR_device_group_create) {
        static const struct {
            const char *name;
            PFN_vkVoidFunction proc;
        } ext_commands[] = {
            {"vkEnumeratePhysicalDeviceGroupsKHR", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDeviceGroups)}};
        for (size_t i = 0; i < ARRAY_SIZE(ext_commands); i++) {
            if (!strcmp(ext_commands[i].name, name)) return ext_commands[i].proc;
        }
    }
    if (instance_map_data->extension_enables.KHR_external_mem_caps) {
        static const struct {
            const char *name;
            PFN_vkVoidFunction proc;
        } ext_commands[] = {{"vkGetPhysicalDeviceExternalBufferPropertiesKHR",
                             reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceExternalBufferProperties)}};
        for (size_t i = 0; i < ARRAY_SIZE(ext_commands); i++) {
            if (!strcmp(ext_commands[i].name, name)) return ext_commands[i].proc;
        }
    }
    if (instance_map_data->extension_enables.KHR_get_phys_dev_props2) {
        static const struct {
            const char *name;
            PFN_vkVoidFunction proc;
        } ext_commands[] = {
            {"vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2)},
            {"vkGetPhysicalDeviceMemoryProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceMemoryProperties2)}};
        for (size_t i = 0; i < ARRAY_SIZE(ext_commands); i++) {
            if (!strcmp(ext_commands[i].name, name)) return ext_commands[i].proc;
        }
    }

    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char *funcName) {
    PFN_vkVoidFunction proc = ImplementedDeviceCommands(funcName);
    if (proc) {
        return proc;
    }
    DeviceMapStruct *device_map_entry = GetDeviceMapEntry(dev);
    assert(device_map_entry);
    PhysDeviceMapStruct *phys_dev_map_entry = GetPhysicalDeviceMapEntry(device_map_entry->physical_device);
    assert(phys_dev_map_entry);
    proc = ImplementedDeviceExtensionCommands(&phys_dev_map_entry->extensions_supported, funcName);
    if (proc) {
        return proc;
    }

    VkuDeviceDispatchTable *pDisp = device_map_entry->dispatch_table;
    if (pDisp == nullptr || pDisp->GetDeviceProcAddr == nullptr) {
        return nullptr;
    }
    return pDisp->GetDeviceProcAddr(dev, funcName);
}

static PFN_vkVoidFunction ImplementedDeviceCommands(const char *name) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction proc;
    } core_device_commands[] = {
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(CreateBuffer)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(DestroyBuffer)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(CreateImage)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(DestroyImage)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(AllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(FreeMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(BindBufferMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(BindImageMemory)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(GetBufferMemoryRequirements)},
        {"vkGetImageMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(GetImageMemoryRequirements)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueue)},
        {"vkQueueBindSparse", reinterpret_cast<PFN_vkVoidFunction>(QueueBindSparse)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(CreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(DestroyFence)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(ResetFences)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(GetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(WaitForFences)}};

    for (size_t i = 0; i < ARRAY_SIZE(core_device_commands); i++) {
        if (!strcmp(core_device_commands[i].name, name)) return core_device_commands[i].proc;
    }

    return nullptr;
}

static PFN_vkVoidFunction ImplementedDeviceExtensionCommands(DeviceExtensions *supported, const char *name) {
    if (supported != nullptr) {
        if (supported->core_1_1) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } core_device_commands[] = {
                {"vkGetImageMemoryRequirements2", reinterpret_cast<PFN_vkVoidFunction>(GetImageMemoryRequirements2)},
                {"vkGetBufferMemoryRequirements2", reinterpret_cast<PFN_vkVoidFunction>(GetBufferMemoryRequirements2)},
                {"vkBindBufferMemory2", reinterpret_cast<PFN_vkVoidFunction>(BindBufferMemory2)},
                {"vkBindImageMemory2", reinterpret_cast<PFN_vkVoidFunction>(BindImageMemory2)}};

            for (size_t i = 0; i < ARRAY_SIZE(core_device_commands); i++) {
                if (!strcmp(core_device_commands[i].name, name)) return core_device_commands[i].proc;
            }
        }
        if (supported->core_1_3) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } core_device_commands[] = {{"vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2)}};

            for (size_t i = 0; i < ARRAY_SIZE(core_device_commands); i++) {
                if (!strcmp(core_device_commands[i].name, name)) return core_device_commands[i].proc;
            }
        }
        if (supported->KHR_external_mem_fd) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } device_ext_commands[] = {
                {"vkGetMemoryFdPropertiesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetMemoryFdPropertiesKHR)}};

            for (size_t i = 0; i < ARRAY_SIZE(device_ext_commands); i++) {
                if (!strcmp(device_ext_commands[i].name, name)) return device_ext_commands[i].proc;
            }
        }
        if (supported->KHR_sync2) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } device_ext_commands[] = {{"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2)}};

            for (size_t i = 0; i < ARRAY_SIZE(device_ext_commands); i++) {
                if (!strcmp(device_ext_commands[i].name, name)) return device_ext_commands[i].proc;
            }
        }
        if (supported->KHR_swapchain) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } device_ext_commands[] = {{"vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(AcquireNextImageKHR)},
                                       {"vkAcquireNextImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(AcquireNextImage2KHR)},
                                       {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(QueuePresentKHR)}};

            for (size_t i = 0; i < ARRAY_SIZE(device_ext_commands); i++) {
                if (!strcmp(device_ext_commands[i].name, name)) return device_ext_commands[i].proc;
            }
        }
        if (supported->EXT_display_control) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } device_ext_commands[] = {
                {"vkRegisterDeviceEventEXT", reinterpret_cast<PFN_vkVoidFunction>(RegisterDeviceEventEXT)},
                {"vkRegisterDisplayEventEXT", reinterpret_cast<PFN_vkVoidFunction>(RegisterDisplayEventEXT)}};

            for (size_t i = 0; i < ARRAY_SIZE(device_ext_commands); i++) {
                if (!strcmp(device_ext_commands[i].name, name)) return device_ext_commands[i].proc;
            }
        }
#ifdef ANDROID
        if (supported->ANDROID_ext_mem_hw_buf) {
            static const struct {
                const char *name;
                PFN_vkVoidFunction proc;
            } device_ext_commands[] = {{"vkGetAndroidHardwareBufferPropertiesANDROID",
                                        reinterpret_cast<PFN_vkVoidFunction>(GetAndroidHardwareBufferPropertiesANDROID)}};

            for (size_t i = 0; i < ARRAY_SIZE(device_ext_commands); i++) {
                if (!strcmp(device_ext_commands[i].name, name)) return device_ext_commands[i].proc;
            }
        }
#endif
    }
    return nullptr;
}

}  // namespace slowdevicesimulator

#if defined(__GNUC__) && __GNUC__ >= 4
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define EXPORT_FUNCTION __attribute__((visibility("default")))
#else
#define EXPORT_FUNCTION
#endif

// loader-layer interface v0, just wrappers since there is only a layer

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pCount,
                                                                                  VkLayerProperties *pProperties) {
    return slowdevicesimulator::EnumerateInstanceLayerProperties(pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                                      VkExtensionProperties *pProperties) {
    return slowdevicesimulator::EnumerateInstanceExtensionProperties(pLayerName, pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return slowdevicesimulator::GetInstanceProcAddr(instance, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    assert(dev != VK_NULL_HANDLE);
    return slowdevicesimulator::GetDeviceProcAddr(dev, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                                          VkPhysicalDevice *pPhysicalDevices) {
    assert(instance != VK_NULL_HANDLE);
    return slowdevicesimulator::EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                                                VkLayerProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice != VK_NULL_HANDLE);
    return slowdevicesimulator::EnumerateDeviceLayerProperties(VK_NULL_HANDLE, pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                    const char *pLayerName, uint32_t *pCount,
                                                                                    VkExtensionProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice != VK_NULL_HANDLE);
    return slowdevicesimulator::EnumerateDeviceExtensionProperties(VK_NULL_HANDLE, pLayerName, pCount, pProperties);
}
