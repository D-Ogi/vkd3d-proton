/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 * Copyright 2019 Conor McCarthy for CodeWeavers
 * Copyright 2021 Philip Rebohle for Valve Corporation
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

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include "vkd3d_private.h"

/* ID3D12Heap */
static HRESULT STDMETHODCALLTYPE d3d12_heap_QueryInterface(d3d12_heap_iface *iface,
        REFIID iid, void **object)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, iid %s, object %p.\n", iface, debugstr_guid(iid), object);

    if (!object)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_ID3D12Heap)
            || IsEqualGUID(iid, &IID_ID3D12Heap1)
            || IsEqualGUID(iid, &IID_ID3D12Pageable)
            || IsEqualGUID(iid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D12Object)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID3D12Heap1_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_ID3DDestructionNotifier))
    {
        ID3DDestructionNotifier_AddRef(&heap->destruction_notifier.ID3DDestructionNotifier_iface);
        *object = &heap->destruction_notifier.ID3DDestructionNotifier_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_heap_AddRef(d3d12_heap_iface *iface)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);
    unsigned int refcount = InterlockedIncrement(&heap->refcount);

    if (refcount == 1)
    {
        d3d12_device_add_ref(heap->device);
        d3d12_heap_incref(heap);
    }

    TRACE("%p increasing refcount to %u.\n", heap, refcount);
    return refcount;
}

static void d3d12_heap_destroy(struct d3d12_heap *heap)
{
    TRACE("Destroying heap %p.\n", heap);

    d3d_destruction_notifier_free(&heap->destruction_notifier);

#ifdef VKD3D_ENABLE_BREADCRUMBS
    /* Heap is held alive until all placements are released. */
    assert(heap->placements_count == 0);
    vkd3d_free(heap->placements);
    pthread_mutex_destroy(&heap->placement_lock);
#endif

    vkd3d_free_memory(heap->device, &heap->device->memory_allocator, &heap->allocation);
    vkd3d_private_store_destroy(&heap->private_store);
    vkd3d_free(heap);
}

static void d3d12_heap_set_name(struct d3d12_heap *heap, const char *name)
{
    if (!heap->allocation.chunk)
        vkd3d_set_vk_object_name(heap->device, (uint64_t)heap->allocation.device_allocation.vk_memory,
                VK_OBJECT_TYPE_DEVICE_MEMORY, name);
}

static ULONG STDMETHODCALLTYPE d3d12_heap_Release(d3d12_heap_iface *iface)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);
    unsigned int refcount = InterlockedDecrement(&heap->refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = heap->device;

        d3d_destruction_notifier_notify(&heap->destruction_notifier);
        d3d12_heap_decref(heap);
        d3d12_device_release(device);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_heap_GetPrivateData(d3d12_heap_iface *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&heap->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_heap_SetPrivateData(d3d12_heap_iface *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&heap->private_store, guid, data_size, data,
            (vkd3d_set_name_callback) d3d12_heap_set_name, heap);
}

static HRESULT STDMETHODCALLTYPE d3d12_heap_SetPrivateDataInterface(d3d12_heap_iface *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&heap->private_store, guid, data,
            (vkd3d_set_name_callback) d3d12_heap_set_name, heap);
}

static HRESULT STDMETHODCALLTYPE d3d12_heap_GetDevice(d3d12_heap_iface *iface, REFIID iid, void **device)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(heap->device, iid, device);
}

static D3D12_HEAP_DESC * STDMETHODCALLTYPE d3d12_heap_GetDesc(d3d12_heap_iface *iface,
        D3D12_HEAP_DESC *desc)
{
    struct d3d12_heap *heap = impl_from_ID3D12Heap1(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = heap->desc;
    return desc;
}

static HRESULT STDMETHODCALLTYPE d3d12_heap_GetProtectedResourceSession(d3d12_heap_iface *iface,
        REFIID iid, void **protected_session)
{
    FIXME("iface %p, iid %s, protected_session %p stub!", iface, debugstr_guid(iid), protected_session);

    return E_NOTIMPL;
}

ULONG d3d12_heap_incref(struct d3d12_heap *heap)
{
    unsigned int refcount = InterlockedIncrement(&heap->internal_refcount);

    TRACE("%p increasing refcount to %u.\n", heap, refcount);

    return refcount;
}

ULONG d3d12_heap_decref(struct d3d12_heap *heap)
{
    unsigned int refcount = InterlockedDecrement(&heap->internal_refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
        d3d12_heap_destroy(heap);

    return refcount;
}

CONST_VTBL struct ID3D12Heap1Vtbl d3d12_heap_vtbl =
{
    /* IUnknown methods */
    d3d12_heap_QueryInterface,
    d3d12_heap_AddRef,
    d3d12_heap_Release,
    /* ID3D12Object methods */
    d3d12_heap_GetPrivateData,
    d3d12_heap_SetPrivateData,
    d3d12_heap_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_heap_GetDevice,
    /* ID3D12Heap methods */
    d3d12_heap_GetDesc,
    /* ID3D12Heap1 methods */
    d3d12_heap_GetProtectedResourceSession,
};

HRESULT d3d12_device_validate_custom_heap_type(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties)
{
    if (heap_properties->Type != D3D12_HEAP_TYPE_CUSTOM)
        return S_OK;

    if (heap_properties->MemoryPoolPreference == D3D12_MEMORY_POOL_UNKNOWN)
    {
        WARN("Invalid memory pool preference.\n");
        return E_INVALIDARG;
    }

    if (heap_properties->MemoryPoolPreference == D3D12_MEMORY_POOL_L1
        && heap_properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)
    {
        WARN("Invalid memory pool preference and CPU page property combination.\n");
        return E_INVALIDARG;
    }

    if (heap_properties->MemoryPoolPreference == D3D12_MEMORY_POOL_L1
        && d3d12_device_is_uma(device, NULL))
    {
        WARN("Invalid memory pool preference on UMA device.\n");
        return E_INVALIDARG;
    }

    if (heap_properties->MemoryPoolPreference == D3D12_MEMORY_POOL_L1
        && heap_properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE
        && !device->memory_info.has_gpu_upload_heap)
    {
        WARN("Invalid memory pool preference (device does not support rebar).\n");
        return E_INVALIDARG;
    }

    if (heap_properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_UNKNOWN)
    {
        WARN("Must have explicit CPU page property for CUSTOM heap type.\n");
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT validate_heap_desc(struct d3d12_device *device, const D3D12_HEAP_DESC *desc)
{
    HRESULT hr;

    if (!desc->SizeInBytes)
    {
        WARN("Invalid size %"PRIu64".\n", desc->SizeInBytes);
        return E_INVALIDARG;
    }

    if (desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
            && desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
    {
        WARN("Invalid alignment %"PRIu64".\n", desc->Alignment);
        return E_INVALIDARG;
    }

    if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
    {
        WARN("D3D12_HEAP_FLAG_ALLOW_DISPLAY is only for committed resources.\n");
        return E_INVALIDARG;
    }

    /* OpenExistingHeapFrom* maps to D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
     * which in turn maps to VK_EXT_external_memory_host.
     */
    if ((desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) &&
        !device->vk_info.EXT_external_memory_host)
    {
        WARN("D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER is not supported.\n");
        return E_INVALIDARG;
    }

    if (FAILED(hr = d3d12_device_validate_custom_heap_type(device, &desc->Properties)))
        return hr;

    return S_OK;
}

static HRESULT d3d12_heap_init(struct d3d12_heap *heap, struct d3d12_device *device,
        const D3D12_HEAP_DESC *desc, void* host_address, const struct vkd3d_device_memory_allocation *borrowed)
{
    struct vkd3d_allocate_heap_memory_info alloc_info;
    HRESULT hr;

    memset(heap, 0, sizeof(*heap));
    heap->ID3D12Heap_iface.lpVtbl = &d3d12_heap_vtbl;
    heap->internal_refcount = 1;
    heap->refcount = 1;
    heap->desc = *desc;
    heap->device = device;
    heap->priority.allows_dynamic_residency = false;
    spinlock_init(&heap->priority.spinlock);
    heap->priority.d3d12priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
    heap->priority.residency_count = 1;

    if (!heap->desc.Properties.CreationNodeMask)
        heap->desc.Properties.CreationNodeMask = 1;
    if (!heap->desc.Properties.VisibleNodeMask)
        heap->desc.Properties.VisibleNodeMask = 1;
    if (!heap->desc.Alignment)
        heap->desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    if (FAILED(hr = validate_heap_desc(device, &heap->desc)))
        return hr;

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.heap_desc = heap->desc;
    alloc_info.host_ptr = host_address;
    alloc_info.borrowed_memory = borrowed;

    /* Borrowed memory (amdgpu-wddm fork) is never suballocated or padded. */
    if ((alloc_info.heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
        d3d12_device_allow_image_heap_suballocation(device) && !borrowed)
    {
        alloc_info.extra_allocation_flags = VKD3D_ALLOCATION_FLAG_ALLOW_IMAGE_SUBALLOCATION;
    }

    if (!(alloc_info.heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) && !borrowed)
        alloc_info.extra_allocation_flags |= VKD3D_ALLOCATION_FLAG_REQUIRE_ALIGNED_GPU_ADDRESS;

    if (!VKD3D_CONFIG_FLAG_IS_SET(DAMAGE_NOT_ZEROED_ALLOCATIONS))
    {
        /* Unfortunately, we cannot trust CREATE_NOT_ZEROED to actually do anything.
         * Stress tests on Windows suggest that it drivers always clear anyway.
         * This suggests we have a lot of potential game bugs in the wild that will randomly be exposed
         * if we try to skip clears.
         * For render targets, we expect the transition away from UNDEFINED to deal with it. */
        alloc_info.heap_desc.Flags &= ~D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    }

    /* Buffers are far more sensitive to memory clears than images. */
    if ((alloc_info.heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
            VKD3D_CONFIG_FLAG_IS_SET(MEMORY_ALLOCATOR_SKIP_IMAGE_HEAP_CLEAR))
        alloc_info.heap_desc.Flags |= D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    if (FAILED(hr = vkd3d_private_store_init(&heap->private_store)))
        return hr;

    if (device->device_info.memory_priority_features.memoryPriority)
    {
        /* this clause isn't trying to reproduce some precise d3d12 behavior,
           though it's hinted in the public docs that a similar prioritization
           is done there... and it seems like a good idea anyway. :) */
        if (heap->desc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)
        {
            uint32_t adjust = vkd3d_get_priority_adjust(heap->desc.SizeInBytes);
            heap->priority.d3d12priority = D3D12_RESIDENCY_PRIORITY_HIGH | adjust;
        }

        if (device->device_info.pageable_device_memory_features.pageableDeviceLocalMemory)
        {
            if (heap->desc.Flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT)
                heap->priority.residency_count = 0;
        }
    }

    alloc_info.vk_memory_priority = heap->priority.residency_count ?
        vkd3d_convert_to_vk_prio(heap->priority.d3d12priority) : 0.f;

    if (FAILED(hr = vkd3d_allocate_heap_memory(device,
            &device->memory_allocator, &alloc_info, &heap->allocation)))
    {
        vkd3d_private_store_destroy(&heap->private_store);
        return hr;
    }

    /* The residency and priority of borrowed memory belong to the embedder. */
    heap->priority.allows_dynamic_residency = !borrowed &&
        device->device_info.pageable_device_memory_features.pageableDeviceLocalMemory &&
        heap->allocation.chunk == NULL /* not suballocated */ &&
        (device->memory_properties.memoryTypes[heap->allocation.device_allocation.vk_memory_type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkd3d_queue_timeline_trace_register_instantaneous(&device->queue_timeline_trace,
            VKD3D_QUEUE_TIMELINE_TRACE_STATE_TYPE_HEAP_ALLOCATION, desc->SizeInBytes);

    d3d_destruction_notifier_init(&heap->destruction_notifier, (IUnknown*)&heap->ID3D12Heap_iface);

#ifdef VKD3D_ENABLE_BREADCRUMBS
    pthread_mutex_init(&heap->placement_lock, NULL);
#endif

    d3d12_device_add_ref(heap->device);
    return S_OK;
}

HRESULT d3d12_heap_create(struct d3d12_device *device, const D3D12_HEAP_DESC *desc,
        void* host_address, struct d3d12_heap **heap)
{
    struct d3d12_heap *object;
    HRESULT hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_heap_init(object, device, desc, host_address, NULL)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created heap %p.\n", object);

    *heap = object;
    return S_OK;
}

/* amdgpu-wddm fork: engine ABI 1.2 CreateHeapFromMemory (libs/ddi/bc250_vkd3d_engine.h, rule V10).
 * Everything the heap cannot honour is refused here with E_INVALIDARG, before any object exists. */
static HRESULT d3d12_heap_validate_borrowed_memory(struct d3d12_device *device,
        const struct vkd3d_borrowed_memory_info *memory, const D3D12_HEAP_DESC *desc)
{
    /* Sharing, display, write watch and protection need memory this library allocates itself. */
    const D3D12_HEAP_FLAGS refused_flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_DISPLAY |
            D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_HARDWARE_PROTECTED |
            D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH;
    /* TOOLS_USE_MANUAL_WRITE_TRACKING (0x2000, not in this library's IDL) is accepted and ignored,
     * as are the residency and zeroing hints: residency and contents are the embedder's. */
    const D3D12_HEAP_FLAGS accepted_flags = D3D12_HEAP_FLAG_DENY_BUFFERS |
            D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES |
            D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT |
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED | 0x2000;
    const D3D12_HEAP_PROPERTIES *properties = &desc->Properties;
    bool buffers, textures, rt_ds;
    unsigned int categories;

    if (memory->vk_memory == VK_NULL_HANDLE)
    {
        WARN("No memory.\n");
        return E_INVALIDARG;
    }

    if (memory->vk_memory_type_index >= device->memory_properties.memoryTypeCount)
    {
        WARN("Memory type %u out of range (%u types).\n", memory->vk_memory_type_index,
                device->memory_properties.memoryTypeCount);
        return E_INVALIDARG;
    }

    if (!desc->SizeInBytes || desc->SizeInBytes > memory->size)
    {
        WARN("Heap size %"PRIu64" does not fit the memory (%"PRIu64" bytes).\n", desc->SizeInBytes, memory->size);
        return E_INVALIDARG;
    }

    if (desc->Flags & (refused_flags | ~(refused_flags | accepted_flags)))
    {
        WARN("Heap flags %#x are not supported on borrowed memory.\n", desc->Flags);
        return E_INVALIDARG;
    }

    switch (properties->Type)
    {
        case D3D12_HEAP_TYPE_DEFAULT:
        case D3D12_HEAP_TYPE_UPLOAD:
        case D3D12_HEAP_TYPE_READBACK:
        case D3D12_HEAP_TYPE_CUSTOM:
            break;
        case D3D12_HEAP_TYPE_GPU_UPLOAD:
            if (device->memory_info.has_gpu_upload_heap)
                break;
            /* fall through */
        default:
            WARN("Heap type %#x is not supported.\n", properties->Type);
            return E_INVALIDARG;
    }

    if (FAILED(d3d12_device_validate_custom_heap_type(device, properties)))
        return E_INVALIDARG;

    /* The heap tier rules are D3D12's: tier 1 heaps hold exactly one resource category. */
    buffers = !(desc->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS);
    textures = !(desc->Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES);
    rt_ds = !(desc->Flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES);
    categories = buffers + textures + rt_ds;
    if (!categories || (categories > 1 && device->d3d12_caps.options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2))
    {
        WARN("Heap flags %#x allow %u resource categories on heap tier %u.\n", desc->Flags, categories,
                device->d3d12_caps.options.ResourceHeapTier);
        return E_INVALIDARG;
    }

    /* This library places no textures on CPU-visible memory (they go through a staging copy with memory
     * of their own), so a CPU-visible borrowed heap is for buffers. */
    if (is_cpu_accessible_heap(properties) && !buffers)
    {
        WARN("A CPU-visible borrowed heap must allow buffers.\n");
        return E_INVALIDARG;
    }

    /* Placed buffers get GPU virtual addresses from the heap's buffer. */
    if (buffers && !memory->device_address)
    {
        WARN("A borrowed heap for buffers needs memory allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT.\n");
        return E_INVALIDARG;
    }

    if (!vkd3d_memory_type_supports_heap(device, memory->vk_memory_type_index, properties, desc->Flags))
    {
        WARN("Memory type %u cannot back heap type %#x with flags %#x.\n",
                memory->vk_memory_type_index, properties->Type, desc->Flags);
        return E_INVALIDARG;
    }

    return S_OK;
}

HRESULT vkd3d_create_heap_from_memory(ID3D12Device *device_iface, const struct vkd3d_borrowed_memory_info *memory,
        const D3D12_HEAP_DESC *desc, REFIID iid, void **heap)
{
    struct vkd3d_device_memory_allocation borrowed;
    struct d3d12_device *device;
    struct d3d12_heap *object;
    HRESULT hr;

    TRACE("device %p, memory %p, desc %p, iid %s, heap %p.\n", device_iface, memory, desc, debugstr_guid(iid), heap);

    if (!heap)
        return E_INVALIDARG;
    *heap = NULL;

    if (!device_iface || !memory || !desc || !iid)
        return E_INVALIDARG;
    device = impl_from_ID3D12Device((d3d12_device_iface *)device_iface);

    if (FAILED(hr = d3d12_heap_validate_borrowed_memory(device, memory, desc)))
        return hr;

    borrowed.vk_memory = memory->vk_memory;
    borrowed.vk_memory_type = memory->vk_memory_type_index;
    borrowed.size = memory->size;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_heap_init(object, device, desc, NULL, &borrowed)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created heap %p over borrowed memory %#"PRIx64".\n", object, (uint64_t)memory->vk_memory);

    return return_interface(&object->ID3D12Heap_iface, &IID_ID3D12Heap, iid, heap);
}

/* amdgpu-wddm fork: engine ABI 1.2 MapHeap and UnmapHeap. */
static struct d3d12_heap *d3d12_heap_from_foreign_iface(ID3D12Heap *iface)
{
    /* Every COM object starts with its vtable, so a pointer to another kind of object is told apart
     * here instead of tripping the assertion in impl_from_ID3D12Heap1(). */
    if (!iface || ((ID3D12Heap1 *)iface)->lpVtbl != &d3d12_heap_vtbl)
        return NULL;
    return impl_from_ID3D12Heap1((ID3D12Heap1 *)iface);
}

static void d3d12_heap_sync_mapping(struct d3d12_heap *heap, bool flush)
{
    const struct vkd3d_vk_device_procs *vk_procs = &heap->device->vk_procs;
    struct d3d12_device *device = heap->device;
    VkMappedMemoryRange range;

    if (device->memory_properties.memoryTypes[heap->allocation.device_allocation.vk_memory_type].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        return;

    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.pNext = NULL;
    range.memory = heap->allocation.device_allocation.vk_memory;
    range.offset = heap->allocation.offset + heap->allocation.realignment_offset;
    range.size = heap->desc.SizeInBytes;
    vkd3d_mapped_memory_range_align(device, &range, heap->allocation.device_allocation.size);

    if (flush)
        VK_CALL(vkFlushMappedMemoryRanges(device->vk_device, 1, &range));
    else
        VK_CALL(vkInvalidateMappedMemoryRanges(device->vk_device, 1, &range));
}

HRESULT vkd3d_heap_map(ID3D12Heap *iface, void **cpu_address)
{
    struct d3d12_heap *heap = d3d12_heap_from_foreign_iface(iface);

    TRACE("heap %p, cpu_address %p.\n", iface, cpu_address);

    if (!cpu_address)
        return E_INVALIDARG;
    *cpu_address = NULL;

    if (!heap)
    {
        WARN("Not a heap of this library.\n");
        return E_INVALIDARG;
    }

    /* Only heaps whose memory this library keeps mapped have an address: CPU-visible heaps other than
     * the CPU-visible texture-only ones, which have no memory of their own. */
    if (!heap->allocation.cpu_address)
    {
        WARN("Heap %p is not CPU visible.\n", heap);
        return E_INVALIDARG;
    }

    vkd3d_atomic_uint32_increment(&heap->map_count, vkd3d_memory_order_relaxed);
    d3d12_heap_sync_mapping(heap, false);

    /* Heap offset 0 is where vkd3d_memory_allocation_slice() places resources at offset 0. */
    *cpu_address = void_ptr_offset(heap->allocation.cpu_address, heap->allocation.realignment_offset);
    return S_OK;
}

HRESULT vkd3d_heap_unmap(ID3D12Heap *iface)
{
    struct d3d12_heap *heap = d3d12_heap_from_foreign_iface(iface);
    uint32_t count, old;

    TRACE("heap %p.\n", iface);

    if (!heap)
    {
        WARN("Not a heap of this library.\n");
        return E_INVALIDARG;
    }

    count = vkd3d_atomic_uint32_load_explicit(&heap->map_count, vkd3d_memory_order_relaxed);
    do
    {
        if (!count)
        {
            WARN("Heap %p is not mapped.\n", heap);
            return E_INVALIDARG;
        }
        old = count;
        count = vkd3d_atomic_uint32_compare_exchange(&heap->map_count, old, old - 1,
                vkd3d_memory_order_relaxed, vkd3d_memory_order_relaxed);
    } while (count != old);

    d3d12_heap_sync_mapping(heap, true);
    return S_OK;
}
