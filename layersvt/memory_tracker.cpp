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

#include <inttypes.h>
#include <unordered_map>
#include <vector>

#include "generated/vk_dispatch_table_helper.h"
#include "generated/vk_enum_string_helper.h"
#include "vk_layer_config.h"
#include "vk_layer_table.h"
#include "utils/vk_layer_extension_utils.h"
#include "utils/vk_layer_utils.h"

namespace memorytracker {

#ifdef ANDROID
#include <android/log.h>
#define WRITE_LOG_MESSAGE(message, ...)                                                 \
    do {                                                                                \
        __android_log_print(ANDROID_LOG_INFO, "MemTrackLayer", message, ##__VA_ARGS__); \
    } while (0)
#else
#define WRITE_LOG_MESSAGE(message, ...) \
    do {                                \
        printf(message, ##__VA_ARGS__); \
        printf("\n");                   \
    } while (0)
#endif

static const VkLayerProperties g_layer_properties = {
    "VK_LAYER_LUNARG_memory_tracker",  // layerName
    VK_MAKE_VERSION(1, 0, 213),        // specVersion (clamped to final 1.0 spec version)
    1,                                 // implementationVersion
    "Layer: memory_tracker",           // description
};

struct InstanceExtensionsEnabled {
    bool core_1_1 = false;
    bool core_1_2 = false;
    bool core_1_3 = false;
    bool KHR_device_group_create = false;
    bool KHR_get_phys_dev_props2 = false;
};
struct InstanceMapStruct {
    VkLayerInstanceDispatchTable *dispatch_table;
    InstanceExtensionsEnabled extension_enables;
};
static std::unordered_map<VkInstance, InstanceMapStruct *> g_instance_map;

struct DeviceExtensions {
    bool core_1_1 = false;
    bool core_1_2 = false;
    bool core_1_3 = false;
    bool EXT_mem_budget = false;
};
struct MemoryHeapWithBudget {
    VkDeviceSize size;
    VkDeviceSize budget;
    VkDeviceSize usage;
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
};
static std::unordered_map<VkPhysicalDevice, PhysDeviceMapStruct *> g_phys_device_map;

struct DeviceMapStruct {
    VkPhysicalDevice physical_device;
    VkLayerDispatchTable *dispatch_table;
    DeviceExtensions extension_enables;
    bool memory_bindings_updated = false;
};
static std::unordered_map<VkDevice, DeviceMapStruct *> g_device_map;

struct BufferMapStruct {
    VkDevice device;
    VkBufferCreateInfo create_info;
    VkMemoryRequirements memory_reqs;
};
static std::unordered_map<VkBuffer, BufferMapStruct *> g_buffer_map;

struct ImageMapStruct {
    VkDevice device;
    VkImageCreateInfo create_info;
    VkMemoryRequirements memory_reqs;
};
static std::unordered_map<VkImage, ImageMapStruct *> g_image_map;

struct BufferMemoryStruct {
    VkBuffer buffer;
    VkDeviceSize offset;
};
struct ImageMemoryStruct {
    VkImage image;
    VkDeviceSize offset;
};
struct MemoryMapStruct {
    VkDevice device;
    VkMemoryAllocateInfo alloc_info;
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

void DumpMemory(DeviceMapStruct *device_map_data, PhysDeviceMapStruct *phys_dev_data_entry, bool supports_memory_budget) {
    WRITE_LOG_MESSAGE("Device : %s", phys_dev_data_entry->props.deviceName);

    for (uint32_t heap = 0; heap < phys_dev_data_entry->memory_props.memoryHeapCount; ++heap) {
        WRITE_LOG_MESSAGE("  --------------Heap %02d----------------", heap);
        WRITE_LOG_MESSAGE("  |    Total Size %14" PRIu64 "       |", phys_dev_data_entry->memory_props.memoryHeaps[heap].size);
        if (supports_memory_budget) {
            WRITE_LOG_MESSAGE("  |    Budget     %14" PRIu64 "       |",
                              phys_dev_data_entry->memory_props.memoryHeaps[heap].budget);
            WRITE_LOG_MESSAGE("  |    Usage      %14" PRIu64 "       |", phys_dev_data_entry->memory_props.memoryHeaps[heap].usage);
        }
        WRITE_LOG_MESSAGE("  |    Flags                           |");
        if (phys_dev_data_entry->memory_props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            WRITE_LOG_MESSAGE("  |      DEVICE_LOCAL                  |");
        }
        if (phys_dev_data_entry->memory_props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT) {
            WRITE_LOG_MESSAGE("  |      MULTI_INSTANCE                |");
        }
        for (uint32_t type = 0; type < phys_dev_data_entry->memory_props.memoryTypeCount; ++type) {
            if (heap == phys_dev_data_entry->memory_props.memoryTypes[type].heapIndex) {
                WRITE_LOG_MESSAGE("  |                                    |");
                WRITE_LOG_MESSAGE("  |   ---Type %02d---                    |", type);
                WRITE_LOG_MESSAGE("  |     Flags                          |");
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags == 0) {
                    WRITE_LOG_MESSAGE("  |        <No Flags>                  |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                    WRITE_LOG_MESSAGE("  |        DEVICE_LOCAL                |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                    WRITE_LOG_MESSAGE("  |        HOST_VISIBLE                |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
                    WRITE_LOG_MESSAGE("  |        HOST_COHERENT               |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
                    WRITE_LOG_MESSAGE("  |        HOST_CACHED                 |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
                    WRITE_LOG_MESSAGE("  |        LAZY_ALLOC                  |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) {
                    WRITE_LOG_MESSAGE("  |        PROTECTED                   |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags &
                    VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD) {
                    WRITE_LOG_MESSAGE("  |        DEV_COHERENT_AMD            |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags &
                    VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD) {
                    WRITE_LOG_MESSAGE("  |        DEV_UNCACHED_AMD            |");
                }
                if (phys_dev_data_entry->memory_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV) {
                    WRITE_LOG_MESSAGE("  |        RDMA_CAPABLE_NV             |");
                }
                bool printed_alloc = false;
                for (auto &memory_data : g_memory_map) {
                    if (memory_data.second->alloc_info.memoryTypeIndex == type) {
                        WRITE_LOG_MESSAGE("  |                                    |");
                        if (!printed_alloc) {
                            WRITE_LOG_MESSAGE("  |     Allocated Memory               |");
                            WRITE_LOG_MESSAGE("  |     -------------------            |");
                            printed_alloc = true;
                        }
                        WRITE_LOG_MESSAGE("  |        VkMemory %013" PRIx64 "      |",
                                          reinterpret_cast<uint64_t>(memory_data.first));
                        WRITE_LOG_MESSAGE("  |          Size %10" PRIu64 "           |",
                                          memory_data.second->alloc_info.allocationSize);
                        WRITE_LOG_MESSAGE("  |                                    |");
                        bool printed_buffers = false;
                        for (auto &buffer : memory_data.second->buffers) {
                            if (!printed_buffers) {
                                WRITE_LOG_MESSAGE("  |          Bound Buffers             |");
                                WRITE_LOG_MESSAGE("  |          .....................     |");
                                printed_buffers = true;
                            }
                            WRITE_LOG_MESSAGE("  |             VkBuffer %013" PRIx64 " |",
                                              reinterpret_cast<uint64_t>(buffer.buffer));
                            WRITE_LOG_MESSAGE("  |                 Size   %10" PRIu64 "  |",
                                              g_buffer_map[buffer.buffer]->memory_reqs.size);
                            WRITE_LOG_MESSAGE("  |                 Align  %10" PRIu64 "  |",
                                              g_buffer_map[buffer.buffer]->memory_reqs.alignment);
                            WRITE_LOG_MESSAGE("  |                 Offset %10" PRIu64 "  |", buffer.offset);
                            WRITE_LOG_MESSAGE("  |                 Flags  0x%08x  |",
                                              g_buffer_map[buffer.buffer]->memory_reqs.memoryTypeBits);
                        }
                        bool printed_images = false;
                        for (auto &image : memory_data.second->images) {
                            if (!printed_images) {
                                WRITE_LOG_MESSAGE("  |          Bound Images              |");
                                WRITE_LOG_MESSAGE("  |          .....................     |");
                                printed_images = true;
                            }
                            WRITE_LOG_MESSAGE("  |             VkImage %013" PRIx64 "  |", reinterpret_cast<uint64_t>(image.image));
                            WRITE_LOG_MESSAGE("  |                 Size   %10" PRIu64 "  |",
                                              g_image_map[image.image]->memory_reqs.size);
                            WRITE_LOG_MESSAGE("  |                 Align  %10" PRIu64 "  |",
                                              g_image_map[image.image]->memory_reqs.alignment);
                            WRITE_LOG_MESSAGE("  |                 Offset %10" PRIu64 "  |", image.offset);
                            WRITE_LOG_MESSAGE("  |                 Flags  0x%08x  |",
                                              g_image_map[image.image]->memory_reqs.memoryTypeBits);
                        }
                    }
                }
            }
        }
        WRITE_LOG_MESSAGE("  |                                    |");
        WRITE_LOG_MESSAGE("  -------------------------------------");
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
    return util_GetLayerProperties(1, &g_layer_properties, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                    VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, g_layer_properties.layerName)) {
        return util_GetExtensionProperties(0, NULL, pCount, pProperties);
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                              VkLayerProperties *pProperties) {
    return util_GetLayerProperties(1, &g_layer_properties, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                              VkInstance *pInstance) {
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
        InstanceMapStruct *instance_map_data = new InstanceMapStruct;
        instance_map_data->dispatch_table = initInstanceTable(*pInstance, fpGetInstanceProcAddr);

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

        // Look through extensions
        for (uint32_t ext = 0; ext < pCreateInfo->enabledExtensionCount; ++ext) {
            if (0 == strcmp(pCreateInfo->ppEnabledExtensionNames[ext], VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME)) {
                instance_map_data->extension_enables.KHR_device_group_create = true;
            }
            if (0 == strcmp(pCreateInfo->ppEnabledExtensionNames[ext], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                instance_map_data->extension_enables.KHR_get_phys_dev_props2 = true;
            }
        }
        g_instance_map[*pInstance] = instance_map_data;
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
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
}

// ------------------------- Physical Device Functions --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                        VkPhysicalDevice *pPhysicalDevices) {
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
                g_phys_device_map[pPhysicalDevices[i]] = phys_dev_data_entry;
            }
            g_phys_device_map[pPhysicalDevices[i]]->instance = instance;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount,
                                                             VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
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
                    g_phys_device_map[pPhysicalDeviceGroupProperties[i].physicalDevices[j]] = phys_dev_data_entry;
                }
                g_phys_device_map[pPhysicalDeviceGroupProperties[i].physicalDevices[j]]->instance = instance;
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount,
                                                                  VkPhysicalDeviceToolPropertiesEXT *pToolProperties) {
    static const VkPhysicalDeviceToolPropertiesEXT memory_tracker_props = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
        nullptr,
        "Memory Tracker Layer",
        "1",
        VK_TOOL_PURPOSE_TRACING_BIT_EXT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT,
        "The VK_LAYER_LUNARG_memory_tracker layer tracks memory usage.",
        "VK_LAYER_LUNARG_memory_tracker"};

    auto original_pToolProperties = pToolProperties;
    if (pToolProperties != nullptr) {
        *pToolProperties = memory_tracker_props;
        pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
        (*pToolCount)--;
    }

    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    VkResult result =
        instance_data_entry->dispatch_table->GetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);
    if (original_pToolProperties != nullptr) {
        pToolProperties = original_pToolProperties;
    }
    (*pToolCount)++;

    return result;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceProperties(physicalDevice, pProperties);
    if (nullptr != pProperties) {
        memcpy(&phys_dev_data_entry->props, pProperties, sizeof(VkPhysicalDeviceProperties));
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceProperties2(physicalDevice, pProperties);
    if (nullptr != pProperties) {
        memcpy(&phys_dev_data_entry->props, &pProperties->properties, sizeof(VkPhysicalDeviceProperties));
    }
}

void CopyMemoryProperties(const VkPhysicalDeviceMemoryProperties *vulkan_props, PhysicalDeviceMemoryBudgetProperties *local_props,
                          VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget_props) {
    local_props->memoryTypeCount = vulkan_props->memoryTypeCount;
    for (uint32_t type = 0; type < vulkan_props->memoryTypeCount; ++type) {
        local_props->memoryTypes[type].heapIndex = vulkan_props->memoryTypes[type].heapIndex;
        local_props->memoryTypes[type].propertyFlags = vulkan_props->memoryTypes[type].propertyFlags;
    }
    local_props->memoryHeapCount = vulkan_props->memoryHeapCount;
    for (uint32_t heap = 0; heap < vulkan_props->memoryHeapCount; ++heap) {
        local_props->memoryHeaps[heap].size = vulkan_props->memoryHeaps[heap].size;
        local_props->memoryHeaps[heap].flags = vulkan_props->memoryHeaps[heap].flags;
        if (budget_props) {
            local_props->memoryHeaps[heap].usage = budget_props->heapUsage[heap];
            local_props->memoryHeaps[heap].budget = budget_props->heapBudget[heap];
        } else {
            local_props->memoryHeaps[heap].usage = 0;
            local_props->memoryHeaps[heap].budget = 0;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    if (nullptr != pMemoryProperties) {
        CopyMemoryProperties(pMemoryProperties, &phys_dev_data_entry->memory_props, nullptr);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
    instance_data_entry->dispatch_table->GetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
    if (nullptr != pMemoryProperties) {
        VkBaseOutStructure *next_chain = reinterpret_cast<VkBaseOutStructure *>(pMemoryProperties->pNext);
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *mem_budget = nullptr;
        while (next_chain != nullptr) {
            if (next_chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
                mem_budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT *>(next_chain);
                break;
            }
        }
        CopyMemoryProperties(&pMemoryProperties->memoryProperties, &phys_dev_data_entry->memory_props, mem_budget);
        if (mem_budget) {
            phys_dev_data_entry->memory_budget_updated = true;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                  uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;

    if (pLayerName && !strcmp(pLayerName, g_layer_properties.layerName)) {
        return util_GetExtensionProperties(0, NULL, pPropertyCount, pProperties);
    }
    assert(physicalDevice);
    PhysDeviceMapStruct *phys_dev_data_entry = GetPhysicalDeviceMapEntry(physicalDevice);
    if (phys_dev_data_entry != NULL) {
        InstanceMapStruct *instance_data_entry = GetInstanceMapEntry(phys_dev_data_entry->instance);
        result = instance_data_entry->dispatch_table->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount,
                                                                                         pProperties);
        if (result == VK_SUCCESS && pProperties != nullptr) {
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
                if (0 == strcmp(pProperties[prop].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                    phys_dev_data_entry->extensions_supported.EXT_mem_budget = true;
                }
            }
        }
    }
    return result;
}

// ------------------------- Device Functions --------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
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
        return VK_ERROR_INITIALIZATION_FAILED;
    }

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
    VkResult result = fpCreateDevice(physical_device, &local_create, pAllocator, pDevice);
    if (result == VK_SUCCESS) {
        DeviceMapStruct *device_map_data = new DeviceMapStruct;
        device_map_data->physical_device = physical_device;
        device_map_data->dispatch_table = new VkLayerDispatchTable;
        layer_init_device_dispatch_table(*pDevice, device_map_data->dispatch_table, fpGetDeviceProcAddr);

        // Look through extensions
        for (uint32_t ext = 0; ext < local_create.enabledExtensionCount; ++ext) {
            if (0 == strcmp(local_create.ppEnabledExtensionNames[ext], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                device_map_data->extension_enables.EXT_mem_budget = true;
            }
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

        DumpMemory(device_map_data, phys_dev_data_entry, device_map_data->extension_enables.EXT_mem_budget);

        g_device_map[*pDevice] = device_map_data;
    }

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyDevice(device, pAllocator);
    EraseDeviceMapEntry(device);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->CreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
    if (result == VK_SUCCESS) {
        BufferMapStruct *buffer_map_data = new BufferMapStruct;
        buffer_map_data->device = device;
        memcpy(&buffer_map_data->create_info, pCreateInfo, sizeof(VkBufferCreateInfo));
        memset(&buffer_map_data->memory_reqs, 0, sizeof(VkMemoryRequirements));
        buffer_map_data->create_info.pNext = nullptr;
        g_buffer_map[*pBuffer] = buffer_map_data;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyBuffer(device, buffer, pAllocator);
    EraseBufferMapEntry(buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                           const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->CreateImage(device, pCreateInfo, pAllocator, pImage);
    if (result == VK_SUCCESS) {
        ImageMapStruct *image_map_data = new ImageMapStruct;
        image_map_data->device = device;
        memcpy(&image_map_data->create_info, pCreateInfo, sizeof(VkImageCreateInfo));
        memset(&image_map_data->memory_reqs, 0, sizeof(VkMemoryRequirements));
        image_map_data->create_info.pNext = nullptr;
        g_image_map[*pImage] = image_map_data;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->DestroyImage(device, image, pAllocator);
    EraseImageMapEntry(image);
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                       VkMemoryRequirements *pMemoryRequirements) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    BufferMapStruct *buffer_map_data = GetBufferMapEntry(buffer);
    assert(device_map_data);
    assert(buffer_map_data);
    assert(buffer_map_data->device == device);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetBufferMemoryRequirements(device, buffer, pMemoryRequirements);
    memcpy(&buffer_map_data->memory_reqs, pMemoryRequirements, sizeof(VkMemoryRequirements));
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    ImageMapStruct *image_map_data = GetImageMapEntry(image);
    assert(device_map_data);
    assert(image_map_data);
    assert(image_map_data->device == device);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetImageMemoryRequirements(device, image, pMemoryRequirements);
    memcpy(&image_map_data->memory_reqs, pMemoryRequirements, sizeof(VkMemoryRequirements));
}

VKAPI_ATTR void VKAPI_CALL GetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t *pSparseMemoryRequirementCount,
                                                            VkSparseImageMemoryRequirements *pSparseMemoryRequirements) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetImageSparseMemoryRequirements(device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (result == VK_SUCCESS) {
        MemoryMapStruct *memory_map_data = new MemoryMapStruct;
        memory_map_data->device = device;
        memcpy(&memory_map_data->alloc_info, pAllocateInfo, sizeof(VkMemoryAllocateInfo));
        memory_map_data->alloc_info.pNext = nullptr;
        g_memory_map[*pMemory] = memory_map_data;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->FreeMemory(device, memory, pAllocator);
    EraseMemoryMapEntry(memory);
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                VkDeviceSize memoryOffset) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindBufferMemory(device, buffer, memory, memoryOffset);
    if (result == VK_SUCCESS && buffer != VK_NULL_HANDLE) {
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
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindImageMemory(device, image, memory, memoryOffset);
    if (result == VK_SUCCESS && image != VK_NULL_HANDLE) {
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
    return result;
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
                                                       VkMemoryRequirements2 *pMemoryRequirements) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    ImageMapStruct *image_map_data = GetImageMapEntry(pInfo->image);
    assert(device_map_data);
    assert(image_map_data);
    assert(image_map_data->device == device);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
    memcpy(&image_map_data->memory_reqs, &pMemoryRequirements->memoryRequirements, sizeof(VkMemoryRequirements));
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
                                                        VkMemoryRequirements2 *pMemoryRequirements) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    BufferMapStruct *buffer_map_data = GetBufferMapEntry(pInfo->buffer);
    assert(device_map_data);
    assert(buffer_map_data);
    assert(buffer_map_data->device == device);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
    memcpy(&buffer_map_data->memory_reqs, &pMemoryRequirements->memoryRequirements, sizeof(VkMemoryRequirements));
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                                                 const VkBindBufferMemoryInfo *pBindInfos) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindBufferMemory2(device, bindInfoCount, pBindInfos);
    if (result == VK_SUCCESS) {
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
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->BindImageMemory2(device, bindInfoCount, pBindInfos);
    if (result == VK_SUCCESS) {
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
                    memory_map_data->images.push_back(image_data);
                }
            }
        }
        device_map_data->memory_bindings_updated = true;

        PhysDeviceMapStruct *phys_dev_map_data = GetPhysicalDeviceMapEntry(device_map_data->physical_device);
        assert(phys_dev_map_data);
        phys_dev_map_data->memory_budget_updated = false;
    }
    return result;
}

// ------------------------- Queue Functions --------------------------------------

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    pDisp->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    g_queue_to_device_map[*pQueue] = device;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    VkDevice device = g_queue_to_device_map[queue];
    DeviceMapStruct *device_map_data = GetDeviceMapEntry(device);
    assert(device_map_data);
    VkLayerDispatchTable *pDisp = device_map_data->dispatch_table;
    VkResult result = pDisp->QueueSubmit(queue, submitCount, pSubmits, fence);
    if (result == VK_SUCCESS) {
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

            DumpMemory(device_map_data, phys_dev_data_entry, device_map_data->extension_enables.EXT_mem_budget);
            device_map_data->memory_bindings_updated = false;
        }
    }
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
            {"vkGetPhysicalDeviceMemoryProperties2", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceMemoryProperties2)}};
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
        } dev_group_create_commands[] = {
            {"vkEnumeratePhysicalDeviceGroupsKHR", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDeviceGroups)}};
        for (size_t i = 0; i < ARRAY_SIZE(dev_group_create_commands); i++) {
            if (!strcmp(dev_group_create_commands[i].name, name)) return dev_group_create_commands[i].proc;
        }
    }
    if (instance_map_data->extension_enables.KHR_get_phys_dev_props2) {
        static const struct {
            const char *name;
            PFN_vkVoidFunction proc;
        } phys_dev_props2_commands[] = {
            {"vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2)},
            {"vkGetPhysicalDeviceMemoryProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceMemoryProperties2)}};
        for (size_t i = 0; i < ARRAY_SIZE(phys_dev_props2_commands); i++) {
            if (!strcmp(phys_dev_props2_commands[i].name, name)) return phys_dev_props2_commands[i].proc;
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

    VkLayerDispatchTable *pDisp = device_map_entry->dispatch_table;
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
        {"vkGetImageSparseMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(GetImageSparseMemoryRequirements)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueue)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit)}};

    for (size_t i = 0; i < ARRAY_SIZE(core_device_commands); i++) {
        if (!strcmp(core_device_commands[i].name, name)) return core_device_commands[i].proc;
    }

    return nullptr;
}

static PFN_vkVoidFunction ImplementedDeviceExtensionCommands(DeviceExtensions *supported, const char *name) {
    if (supported == nullptr || supported->core_1_1) {
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

    return nullptr;
}

}  // namespace memorytracker

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
    return memorytracker::EnumerateInstanceLayerProperties(pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                                      VkExtensionProperties *pProperties) {
    return memorytracker::EnumerateInstanceExtensionProperties(pLayerName, pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return memorytracker::GetInstanceProcAddr(instance, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    assert(dev != VK_NULL_HANDLE);
    return memorytracker::GetDeviceProcAddr(dev, funcName);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                                          VkPhysicalDevice *pPhysicalDevices) {
    assert(instance != VK_NULL_HANDLE);
    return memorytracker::EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                                                VkLayerProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice != VK_NULL_HANDLE);
    return memorytracker::EnumerateDeviceLayerProperties(VK_NULL_HANDLE, pCount, pProperties);
}

EXPORT_FUNCTION VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                    const char *pLayerName, uint32_t *pCount,
                                                                                    VkExtensionProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice != VK_NULL_HANDLE);
    return memorytracker::EnumerateDeviceExtensionProperties(VK_NULL_HANDLE, pLayerName, pCount, pProperties);
}
