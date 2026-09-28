/* SPDX-License-Identifier: MIT
 *
 * bc250_vkd3d_engine.h - boundary between the BC-250 system D3D12 user-mode driver ("shell", the D3D12 UMD
 * that the Microsoft runtime opens through OpenAdapter12) and its vkd3d-proton engine ("engine",
 * amdgpu_wddm_vkd3d.dll, vkd3d-proton fork branch amdgpu-wddm/ddi-engine). The DLL was bc250vkd3d.dll before
 * its file names took the project's amdgpu_wddm prefix; code identifiers keep their BC250 names.
 *
 * Revision r1, ABI 1.0. This file in the vkd3d-proton fork is the only copy; the shell includes it from the
 * fork checkout it builds against. vkd3d-proton is LGPL-2.1 and stays a separately loaded DLL; this header
 * and the other files under libs/ddi are MIT.
 *
 * Versions. A minor version adds and never changes: an engine of minor n serves a shell that requires any
 * minor up to n. CreateInfo.AbiVersion and the argument of Bc250Vkd3dEngineGetFuncs are the version the shell
 * requires (the lowest minor whose additions it calls), not the version of the header it compiled against, so
 * a newer header does not force a newer engine.
 *   1.0  r1: Bc250Vkd3dEngineGetFuncs, CreateDevice. Bring-up and capability level only (see V3).
 *
 * C and C++. Needs windows.h and vulkan_core.h only; neither the WDK nor a D3D12 header, so the engine's
 * translation units (vkd3d-proton's own D3D12 headers) and the shell's (SDK d3d12.h plus d3d12umddi.h) can
 * both include it. Devices are returned as void * of the requested interface; with IID_ID3D12Device and its
 * successors the engine returns vkd3d-proton's ordinary D3D12 device.
 *
 * Division of work
 *
 *   shell   OpenAdapter12, the D3D12 DDI tables and handles, GetCaps (from the engine device's
 *           CheckFeatureSupport), runtime allocations and callbacks, contexts, residency, present, device-lost
 *           reporting. It provides the Vulkan entry point of hosted RADV.
 *   engine  vkd3d-proton's D3D12 device on hosted RADV, created through the shell's entry point.
 *
 * Rules
 *
 *   V1  Vulkan objects. Unlike the DXVK engine, the engine creates its own VkInstance and VkDevice through
 *       CreateInfo.GetInstanceProcAddr (vkd3d-proton's public create path) and destroys them in the device's
 *       final Release. The engine loads no Vulkan DLL. The shell's entry point may add its own structures to the
 *       pNext chains of vkCreateInstance and vkCreateDevice (the hosted contract) and must return the entry
 *       points of the same driver for every name.
 *   V2  Adapter. The engine uses the physical device whose VkPhysicalDeviceIDProperties::deviceLUID equals
 *       CreateInfo.AdapterLuid; if none does, CreateDevice fails with E_INVALIDARG and creates nothing. The
 *       device is independent: two CreateDevice calls return two devices, never a cached one (vkd3d-proton
 *       makes an exception when ENABLE_VULKAN_RENDERDOC_CAPTURE=1, because RenderDoc supports one VkDevice).
 *   V3  Threads and submission (1.0). vkd3d-proton's own threading is unchanged: its queues submit from a
 *       submission thread and it waits for fences on worker threads. That satisfies neither the runtime's fence
 *       model (work must be submitted inside ExecuteCommandLists, on the queue's WDDM context) nor the
 *       thread-scoped hosted callbacks, so 1.0 is for capability queries and engine bring-up off the runtime,
 *       not for a DDI device. A later minor adds the synchronous, context-bound queue mode.
 *   V4  Objects. The device and everything created from it are vkd3d-proton COM objects. The final Release of
 *       the device returns 0 once every child was released; the device drains the GPU then.
 *   V5  Errors. Every function returns an HRESULT; nothing is thrown across this boundary. Engine log lines go
 *       where vkd3d-proton's log goes (stderr, or the file in VKD3D_LOG_FILE); a shell log sink is a later
 *       minor.
 *   V6  Files and configuration. The engine writes no files: vkd3d-proton's disk shader cache (by default
 *       vkd3d-proton.cache in the process's working directory, with a writer thread) is off, as if VKD3D_CONFIG
 *       contained pipeline_library_app_cache; the application's own ID3D12PipelineLibrary still works. The
 *       process's VKD3D_CONFIG and other vkd3d-proton variables still apply on top, for debugging.
 */
#ifndef BC250_VKD3D_ENGINE_H
#define BC250_VKD3D_ENGINE_H

#include <windows.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC250_VKD3D_ENGINE_ABI_MAJOR 1u
#define BC250_VKD3D_ENGINE_ABI_MINOR 0u
#define BC250_VKD3D_ENGINE_ABI_VERSION ((BC250_VKD3D_ENGINE_ABI_MAJOR << 16) | BC250_VKD3D_ENGINE_ABI_MINOR)

typedef struct BC250_VKD3D_DEVICE_CREATE_INFO
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_DEVICE_CREATE_INFO) */
    UINT32 AbiVersion;                             /* the version the shell requires, see Versions */
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr; /* V1: hosted RADV's entry, possibly a shell wrapper */
    LUID AdapterLuid;                              /* V2 */
    UINT32 MinimumFeatureLevel;                    /* a D3D_FEATURE_LEVEL; CreateDevice fails below it */
} BC250_VKD3D_DEVICE_CREATE_INFO;

typedef struct BC250_VKD3D_ENGINE_FUNCS
{
    UINT32 Size;                                   /* set by the caller; the engine fills at most this much */
    UINT32 AbiVersion;                             /* filled by the engine: its own version */

    /* Creates a vkd3d-proton D3D12 device on the adapter (V1, V2) and returns the interface riid of it.
     * E_INVALIDARG for a bad argument, an unknown LUID or an adapter below MinimumFeatureLevel (vkd3d-proton's
     * own answer), E_NOINTERFACE for an AbiVersion the engine does not serve or an riid the device does not
     * implement, the device's failure otherwise. */
    HRESULT (APIENTRY *CreateDevice)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device);
} BC250_VKD3D_ENGINE_FUNCS;

/* The one export. E_NOINTERFACE when abiVersion names another major or a later minor than the engine's;
 * E_INVALIDARG when funcs is NULL or funcs->Size is smaller than the 1.0 structure. */
typedef HRESULT (APIENTRY *PFN_BC250_VKD3D_ENGINE_GET_FUNCS)(UINT32 abiVersion, BC250_VKD3D_ENGINE_FUNCS *funcs);
#define BC250_VKD3D_ENGINE_GET_FUNCS_NAME "Bc250Vkd3dEngineGetFuncs"

#ifdef __cplusplus
}
#endif

#endif /* BC250_VKD3D_ENGINE_H */
