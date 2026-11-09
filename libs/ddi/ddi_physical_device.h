/* SPDX-License-Identifier: MIT
 *
 * The Vulkan physical device of an adapter LUID (BD-331). Only Vulkan and Windows types: the engine
 * (ddi_engine.c) and a host test without a device (physical_device_test.c) use it.
 *
 * The search returns the device and a report. The report names the step that ended the search and what that step
 * answered, so that a caller can tell a driver without the entry points, a failed allocation, a failed
 * enumeration, an empty list and a list without this LUID apart.
 */

#ifndef __BC250_DDI_PHYSICAL_DEVICE_H
#define __BC250_DDI_PHYSICAL_DEVICE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The host test replaces the allocator to make the allocation fail. */
#ifndef BC250_LUID_SEARCH_CALLOC
#define BC250_LUID_SEARCH_CALLOC calloc
#endif

struct bc250_luid_search
{
    /* The step that ended the search: "vkGetInstanceProcAddr", "calloc" or "vkEnumeratePhysicalDevices". */
    const char *step;
    /* What that step answered. VK_ERROR_INCOMPATIBLE_DRIVER: an entry point is missing. VK_ERROR_OUT_OF_HOST_MEMORY:
     * the allocation failed. Otherwise the VkResult of vkEnumeratePhysicalDevices. */
    VkResult result;
    /* The number of devices that vkEnumeratePhysicalDevices listed (0 before it ran). */
    uint32_t count;
    /* Devices with this LUID that are below the minimum Vulkan version, and the name of the first one. */
    uint32_t below_api;
    char below_api_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    /* More devices with this LUID after the first match (a driver defect: the first one is used). */
    uint32_t extra;
};

static inline VkPhysicalDevice bc250_find_physical_device_by_luid(VkInstance vk_instance,
        PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr, const LUID *luid, uint32_t min_api_version,
        struct bc250_luid_search *search)
{
    PFN_vkGetPhysicalDeviceProperties2 pfn_vkGetPhysicalDeviceProperties2;
    PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
    VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceIDProperties id_properties;
    VkPhysicalDeviceProperties2 properties2;
    VkPhysicalDevice *vk_physical_devices;
    uint32_t count, i;

    memset(search, 0, sizeof(*search));
    search->step = "vkGetInstanceProcAddr";
    search->result = VK_ERROR_INCOMPATIBLE_DRIVER;
    pfn_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)pfn_vkGetInstanceProcAddr(
            vk_instance, "vkEnumeratePhysicalDevices");
    pfn_vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)pfn_vkGetInstanceProcAddr(
            vk_instance, "vkGetPhysicalDeviceProperties2");
    if (!pfn_vkEnumeratePhysicalDevices || !pfn_vkGetPhysicalDeviceProperties2)
        return VK_NULL_HANDLE;

    search->step = "vkEnumeratePhysicalDevices";
    count = 0;
    search->result = pfn_vkEnumeratePhysicalDevices(vk_instance, &count, NULL);
    search->count = count;
    if (search->result < 0 || !count)
        return VK_NULL_HANDLE;

    if (!(vk_physical_devices = BC250_LUID_SEARCH_CALLOC(count, sizeof(*vk_physical_devices))))
    {
        search->step = "calloc";
        search->result = VK_ERROR_OUT_OF_HOST_MEMORY;
        return VK_NULL_HANDLE;
    }

    search->result = pfn_vkEnumeratePhysicalDevices(vk_instance, &count, vk_physical_devices);
    search->count = count;
    if (search->result >= 0)
    {
        for (i = 0; i < count; ++i)
        {
            memset(&id_properties, 0, sizeof(id_properties));
            id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            memset(&properties2, 0, sizeof(properties2));
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &id_properties;
            pfn_vkGetPhysicalDeviceProperties2(vk_physical_devices[i], &properties2);

            /* V2: the LUID alone decides. */
            if (!id_properties.deviceLUIDValid || memcmp(id_properties.deviceLUID, luid, VK_LUID_SIZE))
                continue;
            if (properties2.properties.apiVersion < min_api_version)
            {
                if (!search->below_api++)
                    memcpy(search->below_api_name, properties2.properties.deviceName,
                            sizeof(search->below_api_name));
                continue;
            }
            if (vk_physical_device)
            {
                ++search->extra;
                continue;
            }
            vk_physical_device = vk_physical_devices[i];
        }
    }

    free(vk_physical_devices);
    return vk_physical_device;
}

#endif /* __BC250_DDI_PHYSICAL_DEVICE_H */
