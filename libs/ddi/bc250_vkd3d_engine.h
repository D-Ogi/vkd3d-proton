/* SPDX-License-Identifier: MIT
 *
 * bc250_vkd3d_engine.h - boundary between the BC-250 system D3D12 user-mode driver ("shell", the D3D12 UMD
 * that the Microsoft runtime opens through OpenAdapter12) and its vkd3d-proton engine ("engine",
 * amdgpu_wddm_vkd3d.dll, vkd3d-proton fork branch amdgpu-wddm/ddi-engine). The DLL was bc250vkd3d.dll before
 * its file names took the project's amdgpu_wddm prefix; code identifiers keep their BC250 names.
 *
 * Revision r2-draft, ABI 1.1: NOT FROZEN; it changes after T0, the logging-shell probe on unit A that settles
 * the fence contract of the inline queue mode. r1 (ABI 1.0) is frozen and r2 keeps it unchanged. This file in the
 * vkd3d-proton fork is the only copy; the shell includes it from the fork checkout it builds against.
 * vkd3d-proton is LGPL-2.1 and stays a separately loaded DLL; this header and the other files under libs/ddi are
 * MIT.
 *
 * Versions. A minor version adds and never changes: an engine of minor n serves a shell that requires any
 * minor up to n. CreateInfo.AbiVersion and the argument of Bc250Vkd3dEngineGetFuncs are the version the shell
 * requires (the lowest minor whose additions it calls), not the version of the header it compiled against, so
 * a newer header does not force a newer engine.
 *   1.0  r1: Bc250Vkd3dEngineGetFuncs, CreateDevice. Bring-up and capability level only (see V3).
 *   1.1  r2-draft: the inline queue mode (V7 to V9): DEVICE_CREATE_INFO.QueueMode and .Services,
 *        BC250_VKD3D_SHELL_SERVICES, BC250_VKD3D_COMMAND_QUEUE_DESC, ENGINE_FUNCS.CreateCommandQueue.
 *
 * Sizes. Every structure starts with its Size, so a shell built against an older header passes a smaller one.
 * The engine reads a field only when Size covers it (1.1 fields also need AbiVersion 1.1 or later) and fills
 * at most funcs->Size bytes. The *_SIZE_1_0 macros are the 1.0 sizes, the smallest the engine accepts: sizeof of
 * the frozen 1.0 structures (the *_1_0 types), tail padding included, as a 1.0 shell passes them.
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
 *       not for a DDI device. 1.1 adds the inline queue mode (V7); this threaded mode stays the default, also
 *       for a 1.1 shell that leaves QueueMode at BC250_VKD3D_QUEUE_MODE_THREADED.
 *   V4  Objects. The device and everything created from it are vkd3d-proton COM objects. The final Release of
 *       the device returns 0 once every child was released; the device drains the GPU then.
 *   V5  Errors. Every function returns an HRESULT; nothing is thrown across this boundary. Engine log lines go
 *       where vkd3d-proton's log goes (stderr, or the file in VKD3D_LOG_FILE); a shell log sink is a later
 *       minor.
 *   V6  Files and configuration. The engine writes no files: vkd3d-proton's disk shader cache (by default
 *       vkd3d-proton.cache in the process's working directory, with a writer thread) is off, as if VKD3D_CONFIG
 *       contained pipeline_library_app_cache; the application's own ID3D12PipelineLibrary still works. The
 *       process's VKD3D_CONFIG and other vkd3d-proton variables still apply on top, for debugging.
 *   V7  Inline queue mode (1.1, r2-draft). CreateInfo.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE.
 *       - Threads. The device starts no thread. Its Vulkan calls happen on the thread of an engine call (a
 *         function of this header or a COM method of an engine object). CreateDevice fails with E_INVALIDARG
 *         when a configuration would start one: the shader debug ring, descriptor QA, the timestamp profiler,
 *         or the disk cache of V6 enabled again.
 *       - Queues. ENGINE_FUNCS.CreateCommandQueue creates the queues; ID3D12Device::CreateCommandQueue and
 *         CreateCommandQueue1 return E_NOTIMPL. Each queue owns one VkQueue of the engine's VkDevice and never
 *         shares it. The device's internal queue (clears, uploads, sparse initialisation) holds one VkQueue of
 *         the graphics family for the device's lifetime; DIRECT queues use the others. COMPUTE and COPY queues
 *         use vkd3d-proton's families, which are the graphics family when the adapter has no other. The engine
 *         asks for up to 16 VkQueues per family, so an adapter has min(queueCount, 16) - 1 DIRECT queues (one
 *         fewer with NV_low_latency2, which keeps a queue for itself), fewer while COMPUTE or COPY queues share
 *         the family. When every VkQueue of a family is in use, CreateCommandQueue fails with E_OUTOFMEMORY.
 *       - Admission. CreateDevice fails with DXGI_ERROR_UNSUPPORTED, and an error line in the engine log, when
 *         the graphics family gives fewer than BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES (3) usable VkQueues: the
 *         internal queue and the two DIRECT queues that the shell needs at once (T0). r2 asked for 2. Hosted
 *         RADV reported 1 VkQueue per family at r2. The THREADED mode (V3) admits any graphics family.
 *         Sparse binding is available only if the graphics family supports it; the engine asks for no dedicated
 *         sparse family. A COPY queue on a transfer-only family that shares resources with the other families
 *         only by ownership transfer would make vkd3d-proton submit some of its work ("fallback" submissions) on
 *         the internal queue's context instead. Neither adapter tested so far gets there (NVIDIA shares
 *         resources concurrently, hosted RADV has one family), and the engine does not refuse it.
 *       - Binding. CreateDevice calls Services->BindQueue(Shell, NULL, q) for the internal queue before it
 *         submits anything. CreateCommandQueue calls BindQueue(Shell, queueCookie, q) before it returns. Both
 *         calls happen on the caller's thread. A failure fails the creation with BindQueue's HRESULT and leaves
 *         nothing bound.
 *       - Unbinding. The final Release of a queue submits its pending waits and waits for the queue's last
 *         submission. It then retires the queue (V8), calls UnbindQueue(Shell, queueCookie, q), and only then
 *         lets another queue have q. The device's final Release waits for the internal queue and calls
 *         UnbindQueue(Shell, NULL, q).
 *       - Submission. ExecuteCommandLists, UpdateTileMappings, CopyTileMappings, Signal and Wait of an engine
 *         queue run on the calling thread. They return after the last vkQueueSubmit2 or vkQueueBindSparse of
 *         the call, and they do not wait for that work. They may wait on the CPU for older engine work when
 *         vkd3d-proton reuses a command buffer:
 *           * the queue's initial-transition buffers, of which there are 16;
 *           * the internal queue's 16 upload and clear buffers;
 *           * freeing memory that has a clear pending.
 *       - Waits. Every CPU wait of the engine for the GPU is bounded by BC250_VKD3D_INLINE_WAIT_BUDGET_MS (10 s,
 *         longer than the Windows TDR delay): the waits above, SetEventOnCompletion(v, NULL) (V8), the final
 *         Releases, and freeing a reserved resource whose sparse initialisation is pending. Hosted RADV does not
 *         bound them itself; its fence wait loops until the fence signals. A wait that runs out marks the device
 *         removed with DXGI_ERROR_DEVICE_HUNG, a lost device marks it DXGI_ERROR_DEVICE_REMOVED
 *         (GetDeviceRemovedReason), and the wait fails: a method that returns an HRESULT returns that reason; a
 *         submission leaves out the work that needed the reused command buffer (initial transitions, pending
 *         clears) rather than overwrite a buffer in use. After the first failure every wait only polls, so a
 *         removed device returns at once instead of spending another budget. A wait for another thread's CPU
 *         Signal (V8) is not a GPU wait and has no bound, as in D3D12.
 *       - Fences (r2-draft; T0 decides whether the runtime or the engine owns a queue's fences). vkd3d-proton's
 *         ID3D12Fence keeps working without threads:
 *           * queue Signal and Wait;
 *           * ID3D12Fence::Signal, GetCompletedValue and SetEventOnCompletion (V8);
 *           * waits across queues, which become a GPU wait on the signalling queue's timeline.
 *         These do not work:
 *           * A queue Wait for a value that no Signal, from a queue or the CPU, has reached or queued when Wait
 *             is called, or no longer has because a CPU Signal rewinds the fence during the call. It returns
 *             E_NOTIMPL and queues nothing, because a wait before its signal would block the caller.
 *           * Shared fences: CreateFence with D3D12_FENCE_FLAG_SHARED, and OpenSharedHandle of a fence. They
 *             return E_INVALIDARG, because their event wait needs a thread.
 *           * SetEventOnMultipleFenceCompletion with WAIT_ANY and a NULL event. It returns E_NOTIMPL.
 *       - Present belongs to the runtime. The queues do not expose IDXGIVkSwapChainFactory.
 *       - Void methods that fail report through ID3D12Device::GetDeviceRemovedReason, because vkd3d-proton
 *         marks the device removed.
 *   V8  Retirement (1.1, r2-draft). In inline mode the engine learns that the GPU has finished only by reading
 *       its own timeline semaphores inside engine calls. It retires completed work there: command-allocator
 *       references, resources released while in use, sparse-binding and upload references, and fence signals.
 *       Retirement points are:
 *         * the start of every queue operation of V7;
 *         * ID3D12CommandAllocator::Reset, before it counts the allocator's submissions still in flight;
 *         * ID3D12Fence::GetCompletedValue and SetEventOnCompletion, and SetEventOnMultipleFenceCompletion
 *           through them;
 *         * the final Release of a queue or of the device.
 *       The results:
 *         * GetCompletedValue returns the value that the GPU has completed by the time of the call.
 *         * SetEventOnCompletion(v, NULL) waits on the GPU timeline of the earliest pending signal of v and
 *           returns once the fence reaches v, within the budget of V7. If that timeline is reached and the
 *           signal still does not retire, it marks the device removed and returns DXGI_ERROR_DEVICE_REMOVED
 *           rather than wait again. With no pending signal, it waits for another thread to signal.
 *         * SetEventOnCompletion(v, event) sets the event at once if the fence already reached v. Otherwise the
 *           event is set by the first engine call that observes v, not when the GPU gets there (V9).
 *         * Memory that the application releases while the GPU still uses it is freed in a later engine call.
 *       Retirement can free Vulkan memory and run vkd3d-proton destructors, so it also happens only inside
 *       engine calls. It runs under one device-wide lock, so that fence signals complete across queues in the
 *       order the GPU finished them; the destructors run under it too. Retirement itself only polls, except
 *       where a destructor waits within the budget of V7 (memory with a clear pending, a reserved resource
 *       with its sparse initialisation pending). The engine waits for the GPU in those destructors, in the
 *       submission waits of V7, in SetEventOnCompletion(v, NULL) and in final Releases.
 *   V9  Events (1.1, r2-draft). No engine thread sets an event: an event of SetEventOnCompletion or
 *       SetEventOnMultipleFenceCompletion that the fence has not reached at registration is set by a later
 *       engine call (V8). The shell must never block on such an event on the only thread that makes engine
 *       calls; WaitForSingleObject(event, INFINITE) there deadlocks. It waits instead with a NULL event (V8,
 *       bounded), polls GetCompletedValue, or blocks only while another thread keeps making engine calls.
 *       (Not implemented: handing the event to the kernel through a D3DKMT wait with hAsyncEvent, which would
 *       set it with no engine call.)
 */
#ifndef BC250_VKD3D_ENGINE_H
#define BC250_VKD3D_ENGINE_H

#include <windows.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC250_VKD3D_ENGINE_ABI_MAJOR 1u
#define BC250_VKD3D_ENGINE_ABI_MINOR 1u
#define BC250_VKD3D_ENGINE_ABI_VERSION ((BC250_VKD3D_ENGINE_ABI_MAJOR << 16) | BC250_VKD3D_ENGINE_ABI_MINOR)

#define BC250_VKD3D_QUEUE_MODE_THREADED 0u         /* V3; also the mode when the 1.1 fields are absent */
#define BC250_VKD3D_QUEUE_MODE_INLINE   1u         /* V7, 1.1 */

#define BC250_VKD3D_INLINE_WAIT_BUDGET_MS 10000u   /* V7: the longest CPU wait for the GPU in INLINE */
#define BC250_VKD3D_INLINE_MIN_GRAPHICS_QUEUES 3u  /* V7: INLINE admission, usable graphics-family VkQueues */

/* Services the shell provides to one engine device (1.1, V7). The engine calls them only on the thread of an
 * engine call, possibly on several threads at once for different queues. */
typedef struct BC250_VKD3D_SHELL_SERVICES
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_SHELL_SERVICES) */
    void *Shell;                                   /* passed back unchanged */
    /* Give queue, a VkQueue of the engine's VkDevice that no other engine queue uses, a WDDM context of its
     * own. queueCookie is the value passed to CreateCommandQueue, or NULL for the engine's internal queue. */
    HRESULT (APIENTRY *BindQueue)(void *shell, void *queueCookie, VkQueue queue);
    /* queue is idle; the engine submits nothing to it before the next BindQueue. */
    void (APIENTRY *UnbindQueue)(void *shell, void *queueCookie, VkQueue queue);
} BC250_VKD3D_SHELL_SERVICES;

typedef struct BC250_VKD3D_DEVICE_CREATE_INFO
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_DEVICE_CREATE_INFO) */
    UINT32 AbiVersion;                             /* the version the shell requires, see Versions */
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr; /* V1: hosted RADV's entry, possibly a shell wrapper */
    LUID AdapterLuid;                              /* V2 */
    UINT32 MinimumFeatureLevel;                    /* a D3D_FEATURE_LEVEL; CreateDevice fails below it */
    /* 1.1: read only when Size covers them and AbiVersion is 1.1 or later. One exception: a Size that covers
     * them, AbiVersion 1.0 and QueueMode INLINE is E_INVALIDARG, because INLINE is 1.1; below 1.1 other
     * values are ignored and the device is THREADED. */
    UINT32 QueueMode;                              /* BC250_VKD3D_QUEUE_MODE_*; other values: E_INVALIDARG */
    const BC250_VKD3D_SHELL_SERVICES *Services;    /* INLINE: required, BindQueue and UnbindQueue non-NULL */
} BC250_VKD3D_DEVICE_CREATE_INFO;

/* The frozen 1.0 (r1) layouts, for their sizes only. A 1.0 shell passes sizeof of the 1.0 structure, which
 * includes its tail padding: 32 bytes on x64, where the 1.1 field QueueMode (offset 28) lies in that padding.
 * The engine therefore reads the 1.1 fields only when Size covers Services as well. */
typedef struct BC250_VKD3D_DEVICE_CREATE_INFO_1_0
{
    UINT32 Size;
    UINT32 AbiVersion;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    LUID AdapterLuid;
    UINT32 MinimumFeatureLevel;
} BC250_VKD3D_DEVICE_CREATE_INFO_1_0;

#define BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_0 ((UINT32)sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0))

/* The members of D3D12_COMMAND_QUEUE_DESC, which this header cannot include (see above). */
typedef struct BC250_VKD3D_COMMAND_QUEUE_DESC
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_COMMAND_QUEUE_DESC) */
    UINT32 Type;                                   /* D3D12_COMMAND_LIST_TYPE: DIRECT, COMPUTE or COPY */
    INT32 Priority;                                /* D3D12_COMMAND_QUEUE_PRIORITY; recorded, not used */
    UINT32 Flags;                                  /* D3D12_COMMAND_QUEUE_FLAGS */
    UINT32 NodeMask;                               /* 0 or 1 */
} BC250_VKD3D_COMMAND_QUEUE_DESC;

typedef struct BC250_VKD3D_ENGINE_FUNCS
{
    UINT32 Size;                                   /* set by the caller; the engine fills at most this much */
    UINT32 AbiVersion;                             /* filled by the engine: its own version */

    /* Creates a vkd3d-proton D3D12 device on the adapter (V1, V2) and returns the interface riid of it.
     * E_INVALIDARG for a bad argument, an unknown LUID or an adapter below MinimumFeatureLevel (vkd3d-proton's
     * own answer), E_NOINTERFACE for an AbiVersion the engine does not serve or an riid the device does not
     * implement, the device's failure otherwise. */
    HRESULT (APIENTRY *CreateDevice)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device);

    /* 1.1, V7. Creates a queue of device, an engine device in BC250_VKD3D_QUEUE_MODE_INLINE, on a VkQueue of
     * its own, binds it through Services->BindQueue(Shell, queueCookie, ...) and returns the interface riid of
     * the queue (IID_ID3D12CommandQueue and successors). E_INVALIDARG for a bad argument or a device in another
     * mode, E_OUTOFMEMORY when every VkQueue of the family is in use, BindQueue's failure otherwise; nothing is
     * created on failure. queueCookie is opaque to the engine. device must be the pointer that CreateDevice
     * returned for IID_ID3D12Device or a successor (vkd3d-proton gives them all the same pointer); the engine
     * cannot check it, and any other pointer, such as another engine object or a runtime's device, is
     * undefined behaviour. */
    HRESULT (APIENTRY *CreateCommandQueue)(void *device, const BC250_VKD3D_COMMAND_QUEUE_DESC *desc,
            void *queueCookie, REFIID riid, void **queue);
} BC250_VKD3D_ENGINE_FUNCS;

typedef struct BC250_VKD3D_ENGINE_FUNCS_1_0
{
    UINT32 Size;
    UINT32 AbiVersion;
    HRESULT (APIENTRY *CreateDevice)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device);
} BC250_VKD3D_ENGINE_FUNCS_1_0;

#define BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0 ((UINT32)sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0))

/* Compile-time checks, in C and C++: the 1.1 structures extend the 1.0 ones, and the 1.0 sizes are the frozen
 * ones (offsetof comes from vulkan_core.h, through vk_platform.h and stddef.h). */
#define BC250_VKD3D_STATIC_ASSERT(name, e) typedef char BC250_VKD3D_STATIC_ASSERT_##name[(e) ? 1 : -1]
BC250_VKD3D_STATIC_ASSERT(create_info_1_0_prefix,
        offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, AbiVersion) == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, AbiVersion)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, GetInstanceProcAddr)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, GetInstanceProcAddr)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, AdapterLuid) == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, AdapterLuid)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, MinimumFeatureLevel)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, MinimumFeatureLevel)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, Services) >= sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0));
BC250_VKD3D_STATIC_ASSERT(engine_funcs_1_0_prefix,
        offsetof(BC250_VKD3D_ENGINE_FUNCS_1_0, AbiVersion) == offsetof(BC250_VKD3D_ENGINE_FUNCS, AbiVersion)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS_1_0, CreateDevice) == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateDevice)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateCommandQueue) == sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0));
#ifdef _WIN64
BC250_VKD3D_STATIC_ASSERT(sizes_1_0_x64,
        sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0) == 32 && sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0) == 16);
#endif

/* The one export. E_NOINTERFACE when abiVersion names another major or a later minor than the engine's;
 * E_INVALIDARG when funcs is NULL or funcs->Size is smaller than the 1.0 structure
 * (BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0). */
typedef HRESULT (APIENTRY *PFN_BC250_VKD3D_ENGINE_GET_FUNCS)(UINT32 abiVersion, BC250_VKD3D_ENGINE_FUNCS *funcs);
#define BC250_VKD3D_ENGINE_GET_FUNCS_NAME "Bc250Vkd3dEngineGetFuncs"

#ifdef __cplusplus
}
#endif

#endif /* BC250_VKD3D_ENGINE_H */
