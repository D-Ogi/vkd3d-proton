/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __VKD3D_H
#define __VKD3D_H

#include <vkd3d_types.h>

#ifndef VKD3D_NO_WIN32_TYPES
# define COBJMACROS
# include <vkd3d_windows.h>

# define WIDL_C_INLINE_WRAPPERS

# ifdef __MINGW32__
/* Workaround for MinGW-tools WIDL when using inline wrappers.
 * FORCEINLINE is extern which conflicts. It is okay to override it here.
 * All relevant system headers have been included. */
#  undef FORCEINLINE
#  define FORCEINLINE inline
# endif

# include <vkd3d_d3d12.h>
# include <vkd3d_core_interface.h>
# undef WIDL_C_INLINE_WRAPPERS
#endif  /* VKD3D_NO_WIN32_TYPES */

#ifndef VKD3D_NO_VULKAN_H
# ifdef _WIN32
#  define VK_USE_PLATFORM_WIN32_KHR
# endif
# include <vulkan/vulkan.h>
# include "private/vulkan_private_extensions.h"
#endif  /* VKD3D_NO_VULKAN_H */

#include <stdbool.h>

#define VKD3D_MIN_API_VERSION VK_API_VERSION_1_3
#define VKD3D_MAX_API_VERSION VK_API_VERSION_1_3

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

struct vkd3d_instance;

/* Inline queue mode (amdgpu-wddm fork, used by libs/ddi; see rule V7 of libs/ddi/bc250_vkd3d_engine.h).
 * The device starts no threads: every queue operation runs on the caller's thread, and every
 * D3D12 queue owns one VkQueue. The embedder binds each VkQueue before the device first submits
 * to it (queue_cookie is NULL for the device's internal queue) and gets it back once it is idle.
 * The calling convention matches the embedder's ABI, so it can pass its own callbacks through. */
typedef HRESULT (WINAPI *PFN_vkd3d_bind_queue)(void *userdata, void *queue_cookie, VkQueue vk_queue);
typedef void (WINAPI *PFN_vkd3d_unbind_queue)(void *userdata, void *queue_cookie, VkQueue vk_queue);

struct vkd3d_inline_queue_callbacks
{
    void *userdata;
    PFN_vkd3d_bind_queue pfn_bind_queue;
    PFN_vkd3d_unbind_queue pfn_unbind_queue;
};

/* Inline queue mode: the longest a CPU wait for the GPU may take. A wait that runs out marks the device
 * removed with DXGI_ERROR_DEVICE_HUNG and fails; after that, waits only poll. */
#define VKD3D_INLINE_QUEUE_WAIT_TIMEOUT_MS 10000u

struct vkd3d_instance_create_info
{
    /* If set to NULL, libvkd3d loads libvulkan. */
    PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;

    const char * const *instance_extensions;
    uint32_t instance_extension_count;

    const char * const *optional_instance_extensions;
    uint32_t optional_instance_extension_count;
};

struct vkd3d_device_create_info
{
    D3D_FEATURE_LEVEL minimum_feature_level;

    struct vkd3d_instance *instance;
    const struct vkd3d_instance_create_info *instance_create_info;

    VkPhysicalDevice vk_physical_device;

    const char * const *device_extensions;
    uint32_t device_extension_count;

    const char * const *optional_device_extensions;
    uint32_t optional_device_extension_count;

    IUnknown *parent;
    LUID adapter_luid;

    D3D12_DEVICE_FACTORY_FLAGS device_factory_flags;
    bool independent;

    /* Non-NULL selects the inline queue mode (amdgpu-wddm fork); the device keeps a copy.
     * Requires independent. */
    const struct vkd3d_inline_queue_callbacks *inline_queue_callbacks;
};

struct vkd3d_image_resource_create_info
{
    VkImage vk_image;
    D3D12_RESOURCE_DESC desc;
    unsigned int flags;
    D3D12_RESOURCE_STATES present_state;
};

HRESULT vkd3d_create_instance(const struct vkd3d_instance_create_info *create_info,
        struct vkd3d_instance **instance);
ULONG vkd3d_instance_decref(struct vkd3d_instance *instance);
VkInstance vkd3d_instance_get_vk_instance(struct vkd3d_instance *instance);
ULONG vkd3d_instance_incref(struct vkd3d_instance *instance);

HRESULT vkd3d_create_device(const struct vkd3d_device_create_info *create_info,
        REFIID iid, void **device);
IUnknown *vkd3d_get_device_parent(ID3D12Device *device);
VkDevice vkd3d_get_vk_device(ID3D12Device *device);
VkPhysicalDevice vkd3d_get_vk_physical_device(ID3D12Device *device);
struct vkd3d_instance *vkd3d_instance_from_device(ID3D12Device *device);
/* amdgpu-wddm fork: the Vulkan queue family of the device's DIRECT queues (and of its internal queue in the
 * inline queue mode). */
uint32_t vkd3d_get_vk_direct_queue_family_index(ID3D12Device *device);

uint32_t vkd3d_get_vk_queue_family_index(ID3D12CommandQueue *queue);
uint32_t vkd3d_get_vk_queue_index(ID3D12CommandQueue *queue);
uint32_t vkd3d_get_vk_queue_flags(ID3D12CommandQueue *queue);
VkQueue vkd3d_acquire_vk_queue(ID3D12CommandQueue *queue);
void vkd3d_release_vk_queue(ID3D12CommandQueue *queue);
VkQueue vkd3d_lock_vk_queue(ID3D12CommandQueue *queue);
void vkd3d_unlock_vk_queue(ID3D12CommandQueue *queue);
void vkd3d_enqueue_initial_transition(ID3D12CommandQueue *queue, ID3D12Resource *resource);
/* Inline queue mode (amdgpu-wddm fork): the only way to create a queue of such a device. */
HRESULT vkd3d_create_inline_command_queue(ID3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
        void *queue_cookie, REFIID iid, void **command_queue);

/* Borrowed memory (amdgpu-wddm fork, engine ABI 1.2 CreateHeapFromMemory; rule V10 of
 * libs/ddi/bc250_vkd3d_engine.h). The heap uses the embedder's VkDeviceMemory, which must be a
 * non-dedicated allocation on the device's VkDevice that the embedder has not mapped. The library
 * never frees it: it unmaps its own mapping, if any, in the release that destroys the heap, which is
 * the last of the heap's final Release and the final Releases of the resources placed on it. */
struct vkd3d_borrowed_memory_info
{
    VkDeviceMemory vk_memory;
    VkDeviceSize size;
    uint32_t vk_memory_type_index;
    /* Allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; required when the heap allows buffers. */
    bool device_address;
};

HRESULT vkd3d_create_heap_from_memory(ID3D12Device *device, const struct vkd3d_borrowed_memory_info *memory,
        const D3D12_HEAP_DESC *desc, REFIID iid, void **heap);
/* amdgpu-wddm fork (engine ABI 1.2 MapHeap and UnmapHeap): the CPU address of heap offset 0, which a
 * placed buffer at offset X maps at plus X. Only heaps whose memory the library keeps mapped have one;
 * anything else, including an object that is not a heap of this library, gives E_INVALIDARG. Map and
 * unmap are counted; an unmap without a map gives E_INVALIDARG. For memory that is not host-coherent,
 * map invalidates and unmap flushes the heap's range. The address stays valid until the heap is
 * destroyed, whatever the count. */
HRESULT vkd3d_heap_map(ID3D12Heap *heap, void **cpu_address);
HRESULT vkd3d_heap_unmap(ID3D12Heap *heap);

ULONG vkd3d_resource_decref(ID3D12Resource *resource);
ULONG vkd3d_resource_incref(ID3D12Resource *resource);

HRESULT vkd3d_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC *desc,
        D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error_blob);
HRESULT vkd3d_create_root_signature_deserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer);

VkFormat vkd3d_get_vk_format(DXGI_FORMAT format);

DXGI_FORMAT vkd3d_get_dxgi_format(VkFormat format);

HRESULT vkd3d_serialize_versioned_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
        ID3DBlob **blob, ID3DBlob **error_blob);
HRESULT vkd3d_create_versioned_root_signature_deserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer);
HRESULT vkd3d_create_versioned_root_signature_deserializer_for_subobject(const void *data, SIZE_T data_size,
        LPCWSTR subobject_name, REFIID iid, void **deserializer);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif  /* __VKD3D_H */
