/* SPDX-License-Identifier: MIT
 *
 * amdgpu_wddm_vkd3d.dll: vkd3d-proton as the engine behind the BC-250 system D3D12 user-mode driver.
 * The contract is bc250_vkd3d_engine.h; this file only adapts it to vkd3d-proton's public create path
 * (vkd3d_create_instance, vkd3d_create_device), the same path d3d12core.dll takes, without the DXGI
 * adapter lookup, the Vulkan loader and the WSI extensions: the shell owns adapter, driver and present.
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#define VK_NO_PROTOTYPES
#include "vkd3d.h"
#include "vkd3d_debug.h"
#include "vkd3d_core_interface.h"

#include "bc250_vkd3d_engine.h"

#include <stdlib.h>
#include <string.h>

/* libs/vkd3d/device.c: VKD3D_CONFIG defaults of the embedding module. */
void vkd3d_config_set_embedder_defaults(const char *config);

/* libs/vkd3d/cache.c: directory of the disk cache when VKD3D_SHADER_CACHE_PATH is not set, and whether the driver
 * lacks a disk cache of its own (the archive then keeps SPIR-V, and the device persists a VkPipelineCache). */
void vkd3d_set_embedder_shader_cache_path(const char *path, bool driver_without_disk_cache);

/* The disk cache lives per user, as a vendor driver's shader cache does: %LOCALAPPDATA%\amdgpu-wddm\vkd3d, one
 * vkd3d-proton.<program>.cache pair per application. vkd3d-proton's own default is the process's working
 * directory, which for a system driver is the application's and often read-only (Program Files): with that
 * default a game recompiled every pipeline on every start. Without a usable per-user directory the engine
 * keeps V6's behaviour: no disk cache, applications' own ID3D12PipelineLibrary caches keep working.
 * In the inline queue mode (V7) the cache starts no thread: libvkd3d merges and parses the archive during
 * CreateDevice, and the thread that creates a pipeline appends it to the write archive (libs/vkd3d/cache.c).
 * The text of V6 and V7 in bc250_vkd3d_engine.h (no files, the disk cache refused in INLINE) predates this; the
 * header keeps its pinned revision until the next one.
 * The cache keeps full SPIR-V. With shader module identifiers it would keep only the names of pipelines in the
 * driver's own cache, and hosted RADV has no disk cache on Windows (Mesa does not build one there), so a new
 * process would find none of them; SPIR-V at least saves the DXIL and DXBC translation on every later start.
 * For the driver's compilation, the device creates every pipeline with one VkPipelineCache and persists it next to
 * the archive (vkd3d-proton.<program>.cache.driver): loaded at CreateDevice, saved by a pipeline-creating thread at
 * most every 30 s after 64 new pipelines, by a submitting thread once no pipeline was created for 2 s, and at the
 * device's final Release. Those engine callers' threads only take the data (vkGetPipelineCacheData); a work item
 * of the process thread pool writes the file, and the opt-in pipeline log, holding a reference to this module
 * until it is done, so no caller, final Release or FreeLibrary waits for the disk, and the device still starts no
 * thread of its own. A new driver build discards the file; a new engine build starts the archive over but keeps
 * this file.
 * Returns FALSE when no directory could be prepared. */
static BOOL bc250_prepare_cache_directory(void)
{
    char path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path));
    static const char suffix[] = "\\amdgpu-wddm";
    static const char leaf[] = "\\vkd3d";

    if (!length || length + sizeof(suffix) + sizeof(leaf) > sizeof(path))
        return FALSE;
    strcat(path, suffix);
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    strcat(path, leaf);
    if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    vkd3d_set_embedder_shader_cache_path(path, true);
    return TRUE;
}

/* A command signature that changes state needs device generated commands. Without them vkd3d-proton
 * creates the signature and ExecuteIndirect then executes nothing; a driver has to refuse the signature. */
static BOOL CALLBACK bc250_set_config_defaults(PINIT_ONCE once, void *param, void **context)
{
    (void)once;
    (void)param;
    (void)context;
    if (bc250_prepare_cache_directory())
        vkd3d_config_set_embedder_defaults("fail_unsupported_state_template");
    else
        vkd3d_config_set_embedder_defaults("pipeline_library_app_cache,fail_unsupported_state_template");
    return TRUE;
}

static INIT_ONCE bc250_config_once = INIT_ONCE_STATIC_INIT;

/* libvkd3d asks the module that embeds it for the state of vkd3d-proton's test-suite debug interface
 * (d3d12core.dll implements it behind D3D12GetInterface). The engine offers no such interface, so the
 * answers are the defaults of a device that is not under test. */
bool vkd3d_debug_control_is_test_suite(void)
{
    return false;
}

bool vkd3d_debug_control_explode_on_vvl_error(void)
{
    return false;
}

bool vkd3d_debug_control_has_out_of_spec_test_behavior(VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR behavior)
{
    (void)behavior;
    return false;
}

VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS vkd3d_debug_control_get_behavior_flags(void)
{
    return 0;
}

bool vkd3d_debug_control_mute_message_id(const char *vuid)
{
    (void)vuid;
    return false;
}

static VkPhysicalDevice bc250_find_physical_device(struct vkd3d_instance *instance,
        PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr, const LUID *luid)
{
    PFN_vkGetPhysicalDeviceProperties2 pfn_vkGetPhysicalDeviceProperties2;
    PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
    VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceIDProperties id_properties;
    VkPhysicalDeviceProperties2 properties2;
    VkPhysicalDevice *vk_physical_devices;
    VkInstance vk_instance;
    uint32_t count, i;

    vk_instance = vkd3d_instance_get_vk_instance(instance);
    pfn_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)pfn_vkGetInstanceProcAddr(
            vk_instance, "vkEnumeratePhysicalDevices");
    pfn_vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)pfn_vkGetInstanceProcAddr(
            vk_instance, "vkGetPhysicalDeviceProperties2");
    if (!pfn_vkEnumeratePhysicalDevices || !pfn_vkGetPhysicalDeviceProperties2)
        return VK_NULL_HANDLE;

    if (pfn_vkEnumeratePhysicalDevices(vk_instance, &count, NULL) < 0 || !count)
        return VK_NULL_HANDLE;
    if (!(vk_physical_devices = calloc(count, sizeof(*vk_physical_devices))))
        return VK_NULL_HANDLE;

    if (pfn_vkEnumeratePhysicalDevices(vk_instance, &count, vk_physical_devices) >= 0)
    {
        for (i = 0; i < count; ++i)
        {
            memset(&id_properties, 0, sizeof(id_properties));
            id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            memset(&properties2, 0, sizeof(properties2));
            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &id_properties;
            pfn_vkGetPhysicalDeviceProperties2(vk_physical_devices[i], &properties2);

            /* V2: the LUID alone decides. A second match would be a driver bug; take the first and say so. */
            if (!id_properties.deviceLUIDValid || memcmp(id_properties.deviceLUID, luid, VK_LUID_SIZE))
                continue;
            if (properties2.properties.apiVersion < VKD3D_MIN_API_VERSION)
            {
                WARN("Adapter %s matches the LUID but is below Vulkan %u.%u.\n", properties2.properties.deviceName,
                        VK_API_VERSION_MAJOR(VKD3D_MIN_API_VERSION), VK_API_VERSION_MINOR(VKD3D_MIN_API_VERSION));
                continue;
            }
            if (vk_physical_device)
            {
                WARN("More than one physical device has LUID %08lx:%08lx; using the first.\n",
                        (unsigned long)luid->HighPart, (unsigned long)luid->LowPart);
                break;
            }
            vk_physical_device = vk_physical_devices[i];
        }
    }

    free(vk_physical_devices);
    return vk_physical_device;
}

static bool bc250_abi_served(UINT32 version)
{
    return (version >> 16) == BC250_VKD3D_ENGINE_ABI_MAJOR
            && (version & 0xffffu) <= BC250_VKD3D_ENGINE_ABI_MINOR;
}

/* The header's wait budget (V7) is the one libvkd3d applies. */
BC250_VKD3D_STATIC_ASSERT(inline_wait_budget, BC250_VKD3D_INLINE_WAIT_BUDGET_MS == VKD3D_INLINE_QUEUE_WAIT_TIMEOUT_MS);
/* So is its admission (V7). */
BC250_VKD3D_STATIC_ASSERT(inline_min_graphics_queues,
        BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES == VKD3D_INLINE_QUEUE_MIN_GRAPHICS_QUEUES);

/* Sizes, fields included, up to which a 1.1 structure is read (V7). */
#define BC250_DEVICE_CREATE_INFO_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_DEVICE_CREATE_INFO, Services) + sizeof(const BC250_VKD3D_SHELL_SERVICES *)))
#define BC250_SHELL_SERVICES_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_SHELL_SERVICES, UnbindQueue) + sizeof(void *)))
#define BC250_COMMAND_QUEUE_DESC_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_COMMAND_QUEUE_DESC, NodeMask) + sizeof(UINT32)))

/* Sizes, fields included, up to which a 1.2 structure is read (V10, V12). */
#define BC250_IMPORTED_MEMORY_SIZE_1_2 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_IMPORTED_MEMORY, Flags) + sizeof(UINT32)))
#define BC250_DEVICE_CREATE_INFO_SIZE_1_2 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_DEVICE_CREATE_INFO, InstanceMode) + sizeof(UINT32)))

/* What CreateDevice and QueryAdapterCaps (V11) make of a CreateInfo: libvkd3d's create info on the adapter, with
 * the instance it was found on. Both go through bc250_prepare_device(), so they refuse the same CreateInfo with the
 * same HRESULT. create_info points into the structure itself, which is therefore never copied. */
struct bc250_device_request
{
    struct vkd3d_device_create_info create_info;
    struct vkd3d_inline_queue_callbacks inline_callbacks;
    struct vkd3d_instance *instance;
};

static void bc250_release_request(struct bc250_device_request *request)
{
    if (request->instance)
        vkd3d_instance_decref(request->instance);
    request->instance = NULL;
}

static HRESULT bc250_prepare_device(const BC250_VKD3D_DEVICE_CREATE_INFO *info, struct bc250_device_request *request)
{
    struct vkd3d_instance_create_info instance_create_info;
    UINT32 instance_mode = BC250_VKD3D_INSTANCE_MODE_SHARED;
    const BC250_VKD3D_SHELL_SERVICES *services = NULL;
    UINT32 queue_mode = BC250_VKD3D_QUEUE_MODE_THREADED;
    VkPhysicalDevice vk_physical_device;
    HRESULT hr;

    memset(request, 0, sizeof(*request));
    /* A 1.0 shell passes the 1.0 structure: compare with its size, not with this header's. */
    if (!info || info->Size < BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_0 || !info->GetInstanceProcAddr)
        return E_INVALIDARG;
    if (!bc250_abi_served(info->AbiVersion))
        return E_NOINTERFACE;

    /* 1.1 fields: only when the structure has them and the shell requires 1.1. INLINE is 1.1: a shell that asks
     * for it while requiring 1.0 would get a THREADED device it cannot use (V3), so it fails. Other values are
     * ignored below 1.1, as a 1.0 shell does not know the field. */
    if (info->Size >= BC250_DEVICE_CREATE_INFO_SIZE_1_1)
    {
        if ((info->AbiVersion & 0xffffu) >= 1)
        {
            queue_mode = info->QueueMode;
            services = info->Services;
        }
        else if (info->QueueMode == BC250_VKD3D_QUEUE_MODE_INLINE)
        {
            WARN("The inline queue mode needs AbiVersion 1.1.\n");
            return E_INVALIDARG;
        }
    }

    switch (queue_mode)
    {
        case BC250_VKD3D_QUEUE_MODE_THREADED:
            break;

        case BC250_VKD3D_QUEUE_MODE_INLINE:
            if (!services || services->Size < BC250_SHELL_SERVICES_SIZE_1_1
                    || !services->BindQueue || !services->UnbindQueue)
            {
                WARN("The inline queue mode needs Services with BindQueue and UnbindQueue.\n");
                return E_INVALIDARG;
            }
            break;

        default:
            WARN("Unknown queue mode %u.\n", queue_mode);
            return E_INVALIDARG;
    }

    /* The 1.2 field (V12), the same way: PRIVATE below 1.2 fails, other values are ignored there. */
    if (info->Size >= BC250_DEVICE_CREATE_INFO_SIZE_1_2)
    {
        if ((info->AbiVersion & 0xffffu) >= 2)
        {
            instance_mode = info->InstanceMode;
        }
        else if (info->InstanceMode == BC250_VKD3D_INSTANCE_MODE_PRIVATE)
        {
            WARN("The private instance mode needs AbiVersion 1.2.\n");
            return E_INVALIDARG;
        }
    }
    if (instance_mode != BC250_VKD3D_INSTANCE_MODE_SHARED && instance_mode != BC250_VKD3D_INSTANCE_MODE_PRIVATE)
    {
        WARN("Unknown instance mode %u.\n", instance_mode);
        return E_INVALIDARG;
    }

    /* V6, before anything reads the configuration. */
    InitOnceExecuteOnce(&bc250_config_once, bc250_set_config_defaults, NULL, NULL);

    /* V1: no WSI extensions (present belongs to the runtime) and no VR extensions (no OpenVR/OpenXR probing
     * inside a system driver). vkd3d-proton adds the extensions it needs itself. V12: a private instance is the
     * request's own, which the device keeps (libvkd3d refuses a singleton device over it) or the query destroys. */
    memset(&instance_create_info, 0, sizeof(instance_create_info));
    instance_create_info.pfn_vkGetInstanceProcAddr = info->GetInstanceProcAddr;
    instance_create_info.private_instance = instance_mode == BC250_VKD3D_INSTANCE_MODE_PRIVATE;
    if (FAILED(hr = vkd3d_create_instance(&instance_create_info, &request->instance)))
    {
        WARN("Failed to create the vkd3d instance, hr %#x.\n", (unsigned int)hr);
        request->instance = NULL;
        return hr;
    }

    if (!(vk_physical_device = bc250_find_physical_device(request->instance, info->GetInstanceProcAddr,
            &info->AdapterLuid)))
    {
        WARN("No Vulkan physical device has LUID %08lx:%08lx.\n",
                (unsigned long)info->AdapterLuid.HighPart, (unsigned long)info->AdapterLuid.LowPart);
        bc250_release_request(request);
        return E_INVALIDARG;
    }

    request->create_info.minimum_feature_level = (D3D_FEATURE_LEVEL)info->MinimumFeatureLevel;
    request->create_info.instance = request->instance;
    request->create_info.vk_physical_device = vk_physical_device;
    request->create_info.adapter_luid = info->AdapterLuid;
    request->create_info.independent = true;

    /* V7: the shell's services have the calling convention of libvkd3d's callbacks (vkd3d.h). */
    if (queue_mode == BC250_VKD3D_QUEUE_MODE_INLINE)
    {
        request->inline_callbacks.userdata = services->Shell;
        request->inline_callbacks.pfn_bind_queue = (PFN_vkd3d_bind_queue)services->BindQueue;
        request->inline_callbacks.pfn_unbind_queue = (PFN_vkd3d_unbind_queue)services->UnbindQueue;
        request->create_info.inline_queue_callbacks = &request->inline_callbacks;
    }
    return S_OK;
}

static HRESULT APIENTRY bc250_create_device(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device)
{
    struct bc250_device_request request;
    HRESULT hr;

    if (device)
        *device = NULL;
    if (!info || !riid || !device)
        return E_INVALIDARG;
    if (FAILED(hr = bc250_prepare_device(info, &request)))
        return hr;

    hr = vkd3d_create_device(&request.create_info, riid, device);
    /* The device holds its own reference to the instance. */
    bc250_release_request(&request);

    if (FAILED(hr))
        WARN("Failed to create the device, hr %#x.\n", (unsigned int)hr);
    return hr;
}

static HRESULT APIENTRY bc250_create_command_queue(void *device, const BC250_VKD3D_COMMAND_QUEUE_DESC *desc,
        void *queueCookie, REFIID riid, void **queue)
{
    D3D12_COMMAND_QUEUE_DESC d3d12_desc;

    if (queue)
        *queue = NULL;
    if (!device || !desc || !riid || !queue || desc->Size < BC250_COMMAND_QUEUE_DESC_SIZE_1_1)
        return E_INVALIDARG;

    d3d12_desc.Type = (D3D12_COMMAND_LIST_TYPE)desc->Type;
    d3d12_desc.Priority = desc->Priority;
    d3d12_desc.Flags = (D3D12_COMMAND_QUEUE_FLAGS)desc->Flags;
    d3d12_desc.NodeMask = desc->NodeMask;

    /* E_INVALIDARG for a device in another mode or an unsupported type. */
    return vkd3d_create_inline_command_queue((ID3D12Device *)device, &d3d12_desc, queueCookie, riid, queue);
}

static HRESULT APIENTRY bc250_get_vulkan_handles(void *device, VkInstance *instance,
        VkPhysicalDevice *physical_device, VkDevice *vk_device, UINT32 *queue_family_index)
{
    ID3D12Device *d3d12_device = device;

    if (!device || !instance || !physical_device || !vk_device || !queue_family_index)
        return E_INVALIDARG;

    *instance = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(d3d12_device));
    *physical_device = vkd3d_get_vk_physical_device(d3d12_device);
    *vk_device = vkd3d_get_vk_device(d3d12_device);
    *queue_family_index = vkd3d_get_vk_direct_queue_family_index(d3d12_device);
    return S_OK;
}

static HRESULT APIENTRY bc250_create_heap_from_memory(void *device, const BC250_VKD3D_IMPORTED_MEMORY *memory,
        const struct D3D12_HEAP_DESC *heap_desc, REFIID riid, void **heap)
{
    struct vkd3d_borrowed_memory_info borrowed;

    if (heap)
        *heap = NULL;
    if (!device || !memory || !heap_desc || !riid || !heap || memory->Size < BC250_IMPORTED_MEMORY_SIZE_1_2)
        return E_INVALIDARG;
    if (memory->Flags & ~BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS)
    {
        WARN("Unknown imported memory flags %#x.\n", memory->Flags);
        return E_INVALIDARG;
    }

    /* V10: libvkd3d checks the memory against the heap description and never frees it. */
    memset(&borrowed, 0, sizeof(borrowed));
    borrowed.vk_memory = memory->Memory;
    borrowed.size = memory->AllocationSize;
    borrowed.vk_memory_type_index = memory->MemoryTypeIndex;
    borrowed.device_address = !!(memory->Flags & BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS);
    return vkd3d_create_heap_from_memory((ID3D12Device *)device, &borrowed, (const D3D12_HEAP_DESC *)heap_desc,
            riid, heap);
}

static HRESULT APIENTRY bc250_map_heap(void *heap, void **cpu_address)
{
    return vkd3d_heap_map((ID3D12Heap *)heap, cpu_address);
}

static HRESULT APIENTRY bc250_unmap_heap(void *heap)
{
    return vkd3d_heap_unmap((ID3D12Heap *)heap);
}

static HRESULT APIENTRY bc250_query_adapter_caps(const BC250_VKD3D_DEVICE_CREATE_INFO *info, UINT32 count,
        BC250_VKD3D_FEATURE_QUERY *queries)
{
    struct bc250_device_request request;
    struct vkd3d_adapter_caps *caps;
    HRESULT hr;
    UINT32 i;

    if (count && !queries)
        return E_INVALIDARG;

    /* V11: CreateDevice's checks and admission, then its capability decisions without a VkDevice. */
    if (SUCCEEDED(hr = bc250_prepare_device(info, &request)))
    {
        hr = vkd3d_create_adapter_caps(&request.create_info, &caps);
        /* The adapter caps object holds its own reference to the instance until it is destroyed. */
        bc250_release_request(&request);
    }
    if (FAILED(hr))
    {
        WARN("Failed to examine the adapter, hr %#x.\n", (unsigned int)hr);
        for (i = 0; i < count; ++i)
            queries[i].Result = hr;
        return hr;
    }

    for (i = 0; i < count; ++i)
    {
        queries[i].Result = queries[i].Reserved ? E_INVALIDARG : vkd3d_adapter_caps_check_feature_support(caps,
                (D3D12_FEATURE)queries[i].Feature, queries[i].pData, queries[i].DataSize);
    }
    vkd3d_destroy_adapter_caps(caps);
    return S_OK;
}

static void bc250_linear_image_info(const struct vkd3d_linear_image_info *in, BC250_VKD3D_LINEAR_IMAGE_INFO *out)
{
    out->MemoryTypeBits = in->memory_type_bits;
    out->Offset = in->offset;
    out->RowPitch = in->row_pitch;
    out->LayoutSize = in->layout_size;
    out->MemorySize = in->memory_size;
    out->MemoryAlignment = in->memory_alignment;
}

/* V13: on failure info keeps its Size and is zero otherwise. Size is the caller's, which may be larger than
 * this structure: what lies beyond it is not touched. */
static bool bc250_reset_linear_image_info(BC250_VKD3D_LINEAR_IMAGE_INFO *info)
{
    UINT32 size;

    if (!info || info->Size < sizeof(*info))
        return false;
    size = info->Size;
    memset(info, 0, sizeof(*info));
    info->Size = size;
    return true;
}

static HRESULT APIENTRY bc250_query_linear_image(void *device, const struct D3D12_RESOURCE_DESC1 *desc,
        BC250_VKD3D_LINEAR_IMAGE_INFO *info)
{
    struct vkd3d_linear_image_info image;
    HRESULT hr;

    if (!bc250_reset_linear_image_info(info) || !device || !desc)
        return E_INVALIDARG;
    if (SUCCEEDED(hr = vkd3d_query_linear_image((ID3D12Device *)device, (const D3D12_RESOURCE_DESC1 *)desc, &image)))
        bc250_linear_image_info(&image, info);
    return hr;
}

static HRESULT APIENTRY bc250_create_linear_placed_resource(void *device, void *heap, UINT64 heap_offset,
        const struct D3D12_RESOURCE_DESC1 *desc, UINT32 initial_state,
        const struct D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid, void **resource,
        BC250_VKD3D_LINEAR_IMAGE_INFO *info)
{
    struct vkd3d_linear_image_info image;
    HRESULT hr;

    if (resource)
        *resource = NULL;
    if (!bc250_reset_linear_image_info(info) || !device || !heap || !desc || !riid || !resource)
        return E_INVALIDARG;
    if (SUCCEEDED(hr = vkd3d_create_linear_placed_resource((ID3D12Device *)device, (ID3D12Heap *)heap, heap_offset,
            (const D3D12_RESOURCE_DESC1 *)desc, (D3D12_RESOURCE_STATES)initial_state,
            (const D3D12_CLEAR_VALUE *)optimized_clear_value, riid, resource, &image)))
        bc250_linear_image_info(&image, info);
    return hr;
}

HRESULT APIENTRY Bc250Vkd3dEngineGetFuncs(UINT32 abiVersion, BC250_VKD3D_ENGINE_FUNCS *funcs)
{
    BC250_VKD3D_ENGINE_FUNCS out;
    UINT32 size, minor;

    /* A 1.0 shell passes the 1.0 structure: compare with its size, not with this header's. */
    if (!funcs || funcs->Size < BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0)
        return E_INVALIDARG;
    if (!bc250_abi_served(abiVersion))
        return E_NOINTERFACE;

    size = funcs->Size;
    minor = abiVersion & 0xffffu;
    memset(&out, 0, sizeof(out));
    out.Size = size;
    out.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    out.CreateDevice = bc250_create_device;
    /* Only the entries of the minors the shell requires (Sizes): the others stay NULL. */
    if (minor >= 1)
        out.CreateCommandQueue = bc250_create_command_queue;
    if (minor >= 2)
    {
        out.GetVulkanHandles = bc250_get_vulkan_handles;
        out.CreateHeapFromMemory = bc250_create_heap_from_memory;
        out.MapHeap = bc250_map_heap;
        out.UnmapHeap = bc250_unmap_heap;
        out.QueryAdapterCaps = bc250_query_adapter_caps;
    }
    if (minor >= 3)
    {
        out.QueryLinearImage = bc250_query_linear_image;
        out.CreateLinearPlacedResource = bc250_create_linear_placed_resource;
    }
    /* Fill at most Size bytes: a 1.0 structure ends before CreateCommandQueue, a 1.1 structure before
     * GetVulkanHandles, a 1.2 structure before QueryLinearImage, and a larger one keeps its zeroed tail. */
    memset(funcs, 0, size);
    memcpy(funcs, &out, size < sizeof(out) ? size : sizeof(out));
    return S_OK;
}
