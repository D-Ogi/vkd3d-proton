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

/* V6: no disk cache. vkd3d-proton's default keeps vkd3d-proton.cache in the process's working directory and a
 * writer thread; a system driver loaded into every D3D12 process must not. Applications' own
 * ID3D12PipelineLibrary caches keep working. */
static BOOL CALLBACK bc250_set_config_defaults(PINIT_ONCE once, void *param, void **context)
{
    (void)once;
    (void)param;
    (void)context;
    vkd3d_config_set_embedder_defaults("pipeline_library_app_cache");
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

/* Sizes, fields included, up to which a 1.1 structure is read (V7). */
#define BC250_DEVICE_CREATE_INFO_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_DEVICE_CREATE_INFO, Services) + sizeof(const BC250_VKD3D_SHELL_SERVICES *)))
#define BC250_SHELL_SERVICES_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_SHELL_SERVICES, UnbindQueue) + sizeof(void *)))
#define BC250_COMMAND_QUEUE_DESC_SIZE_1_1 \
        ((UINT32)(FIELD_OFFSET(BC250_VKD3D_COMMAND_QUEUE_DESC, NodeMask) + sizeof(UINT32)))

static HRESULT APIENTRY bc250_create_device(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device)
{
    struct vkd3d_instance_create_info instance_create_info;
    struct vkd3d_device_create_info device_create_info;
    struct vkd3d_inline_queue_callbacks inline_callbacks;
    const BC250_VKD3D_SHELL_SERVICES *services = NULL;
    UINT32 queue_mode = BC250_VKD3D_QUEUE_MODE_THREADED;
    struct vkd3d_instance *instance;
    VkPhysicalDevice vk_physical_device;
    HRESULT hr;

    if (device)
        *device = NULL;
    /* A 1.0 shell passes the 1.0 structure: compare with its size, not with this header's. */
    if (!info || !riid || !device || info->Size < BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_0 || !info->GetInstanceProcAddr)
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

    InitOnceExecuteOnce(&bc250_config_once, bc250_set_config_defaults, NULL, NULL);

    /* V1: no WSI extensions (present belongs to the runtime) and no VR extensions (no OpenVR/OpenXR probing
     * inside a system driver). vkd3d-proton adds the extensions it needs itself. */
    memset(&instance_create_info, 0, sizeof(instance_create_info));
    instance_create_info.pfn_vkGetInstanceProcAddr = info->GetInstanceProcAddr;
    if (FAILED(hr = vkd3d_create_instance(&instance_create_info, &instance)))
    {
        WARN("Failed to create the vkd3d instance, hr %#x.\n", (unsigned int)hr);
        return hr;
    }

    if (!(vk_physical_device = bc250_find_physical_device(instance, info->GetInstanceProcAddr, &info->AdapterLuid)))
    {
        WARN("No Vulkan physical device has LUID %08lx:%08lx.\n",
                (unsigned long)info->AdapterLuid.HighPart, (unsigned long)info->AdapterLuid.LowPart);
        vkd3d_instance_decref(instance);
        return E_INVALIDARG;
    }

    memset(&device_create_info, 0, sizeof(device_create_info));
    device_create_info.minimum_feature_level = (D3D_FEATURE_LEVEL)info->MinimumFeatureLevel;
    device_create_info.instance = instance;
    device_create_info.vk_physical_device = vk_physical_device;
    device_create_info.adapter_luid = info->AdapterLuid;
    device_create_info.independent = true;

    /* V7: the shell's services have the calling convention of libvkd3d's callbacks (vkd3d.h). */
    if (queue_mode == BC250_VKD3D_QUEUE_MODE_INLINE)
    {
        inline_callbacks.userdata = services->Shell;
        inline_callbacks.pfn_bind_queue = (PFN_vkd3d_bind_queue)services->BindQueue;
        inline_callbacks.pfn_unbind_queue = (PFN_vkd3d_unbind_queue)services->UnbindQueue;
        device_create_info.inline_queue_callbacks = &inline_callbacks;
    }

    hr = vkd3d_create_device(&device_create_info, riid, device);
    /* The device holds its own reference to the instance. */
    vkd3d_instance_decref(instance);

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

HRESULT APIENTRY Bc250Vkd3dEngineGetFuncs(UINT32 abiVersion, BC250_VKD3D_ENGINE_FUNCS *funcs)
{
    BC250_VKD3D_ENGINE_FUNCS out;
    UINT32 size;

    /* A 1.0 shell passes the 1.0 structure: compare with its size, not with this header's. */
    if (!funcs || funcs->Size < BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0)
        return E_INVALIDARG;
    if (!bc250_abi_served(abiVersion))
        return E_NOINTERFACE;

    size = funcs->Size;
    memset(&out, 0, sizeof(out));
    out.Size = size;
    out.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    out.CreateDevice = bc250_create_device;
    out.CreateCommandQueue = bc250_create_command_queue;
    /* Fill at most Size bytes: a 1.0 structure ends before CreateCommandQueue, a larger one keeps its
     * zeroed tail. */
    memset(funcs, 0, size);
    memcpy(funcs, &out, size < sizeof(out) ? size : sizeof(out));
    return S_OK;
}
