/* SPDX-License-Identifier: MIT
 *
 * bc250_vkd3d_engine.h - boundary between the BC-250 system D3D12 user-mode driver ("shell", the D3D12 UMD
 * that the Microsoft runtime opens through OpenAdapter12) and its vkd3d-proton engine ("engine",
 * amdgpu_wddm_vkd3d.dll, vkd3d-proton fork branch amdgpu-wddm/ddi-engine). The DLL was bc250vkd3d.dll before
 * its file names took the project's amdgpu_wddm prefix; code identifiers keep their BC250 names.
 *
 * Revision r5-draft, ABI 1.3: NOT FROZEN. r5 adds 1.3, linear images (V13), and changes nothing of 1.2.
 * r4 adds the instance mode (V12) to 1.2; the minor stays 1.2 because 1.2
 * is a draft, so an r3 engine does not know the field (V12 says how the shell tells). r3 adds 1.2 (imported memory
 * and the adapter query, V10 and V11) and raises the admission of the inline queue mode (V7) to three VkQueues.
 * 1.1 stays a draft until T0, the logging-shell probe on unit A that settles the fence contract of the inline
 * queue mode, and 1.2 with it. r1 (ABI 1.0) is frozen, and r2 to r4 keep it unchanged. This file in the
 * vkd3d-proton fork is the only copy; the shell includes it from the fork checkout it builds against. vkd3d-proton
 * is LGPL-2.1 and stays a separately loaded DLL; this header and the other files under libs/ddi are MIT.
 *
 * Versions. A minor version adds and never changes: an engine of minor n serves a shell that requires any
 * minor up to n. CreateInfo.AbiVersion and the argument of Bc250Vkd3dEngineGetFuncs are the version the shell
 * requires (the lowest minor whose additions it calls), not the version of the header it compiled against, so
 * a newer header does not force a newer engine.
 *   1.0  r1: Bc250Vkd3dEngineGetFuncs, CreateDevice. Bring-up and capability level only (see V3).
 *   1.1  r2-draft: the inline queue mode (V7 to V9): DEVICE_CREATE_INFO.QueueMode and .Services,
 *        BC250_VKD3D_SHELL_SERVICES, BC250_VKD3D_COMMAND_QUEUE_DESC, ENGINE_FUNCS.CreateCommandQueue.
 *   1.2  r3-draft: imported memory (V10) and adapter capabilities (V11): ENGINE_FUNCS.GetVulkanHandles,
 *        CreateHeapFromMemory, MapHeap, UnmapHeap and QueryAdapterCaps, BC250_VKD3D_IMPORTED_MEMORY,
 *        BC250_VKD3D_FEATURE_QUERY. r4-draft: the instance mode (V12), DEVICE_CREATE_INFO.InstanceMode.
 *   1.3  r5-draft: linear images (V13): ENGINE_FUNCS.QueryLinearImage and CreateLinearPlacedResource,
 *        BC250_VKD3D_LINEAR_IMAGE_INFO.
 *
 * Sizes. Every structure starts with its Size, so a shell built against an older header passes a smaller one.
 * The engine reads a field only when Size covers it (fields of minor n also need AbiVersion 1.n or later). It
 * fills at most funcs->Size bytes of the function table, and of those only the entries of the minors up to the one
 * that abiVersion names; later entries are NULL, so a shell that requires 1.1 finds no 1.2 function even in a
 * 1.2-sized table. The *_SIZE_1_0 macros are the 1.0 sizes, the smallest the engine accepts: sizeof of the frozen
 * 1.0 structures (the *_1_0 types), tail padding included, as a 1.0 shell passes them. The *_SIZE_1_1 macros are
 * the sizes of the 1.1 layouts (the *_1_1 types), which 1.2 extends without changing them.
 *
 * C and C++. Needs windows.h and vulkan_core.h only; neither the WDK nor a D3D12 header, so the engine's
 * translation units (vkd3d-proton's own D3D12 headers) and the shell's (SDK d3d12.h plus d3d12umddi.h) can
 * both include it. D3D12 structures appear as pointers to their struct tags (struct D3D12_HEAP_DESC), which both
 * header sets declare. Devices are returned as void * of the requested interface; with IID_ID3D12Device and its
 * successors the engine returns vkd3d-proton's ordinary D3D12 device.
 *
 * Division of work
 *
 *   shell   OpenAdapter12, the D3D12 DDI tables and handles, GetCaps (from QueryAdapterCaps, V11, since GetCaps
 *           comes before any device), runtime allocations and callbacks, the memory of heaps (V10), contexts,
 *           residency, present, device-lost reporting. It provides the Vulkan entry point of hosted RADV.
 *   engine  vkd3d-proton's D3D12 device on hosted RADV, created through the shell's entry point.
 *
 * Rules
 *
 *   V1  Vulkan objects. Unlike the DXVK engine, the engine creates its own VkInstance and VkDevice through
 *       CreateInfo.GetInstanceProcAddr (vkd3d-proton's public create path) and destroys them in the device's
 *       final Release; the VkInstance is shared by the live engine devices unless CreateInfo asks for a private
 *       one (V12), and a shared one goes with the last of them. The engine loads no Vulkan DLL. The shell's entry
 *       point may add its own structures to the pNext chains of vkCreateInstance and vkCreateDevice (the hosted
 *       contract) and must return the entry points of the same driver for every name. From 1.2, GetVulkanHandles
 *       hands the objects to the shell for the memory of V10.
 *   V2  Adapter. The engine uses the physical device whose VkPhysicalDeviceIDProperties::deviceLUID equals
 *       CreateInfo.AdapterLuid; if none does, CreateDevice fails with E_INVALIDARG and creates nothing. The
 *       device is independent: two CreateDevice calls return two devices, never a cached one (vkd3d-proton
 *       makes an exception when ENABLE_VULKAN_RENDERDOC_CAPTURE=1, because RenderDoc supports one VkDevice; an
 *       INLINE (V7) or PRIVATE (V12) CreateDevice or QueryAdapterCaps then fails with E_INVALIDARG).
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
 *   V10 Imported memory (1.2, r3-draft). GetVulkanHandles returns the engine device's VkInstance,
 *       VkPhysicalDevice and VkDevice, and the queue family of its DIRECT queues (in INLINE, also of the internal
 *       queue). They stay the engine's and valid until the device's final Release; the shell uses the VkDevice
 *       only to allocate, import, query and free the memory it gives CreateHeapFromMemory, which makes an
 *       ID3D12Heap over that memory.
 *       - Ownership. The memory stays the shell's. The engine never frees it, clears it, zeroes it or changes its
 *         priority or residency, and never turns memory it allocated itself into such a heap. Its only Vulkan
 *         calls on the VkDeviceMemory are binds of the heap's buffer and of placed resources, vkMapMemory once
 *         when it creates a CPU-visible heap and vkUnmapMemory once when it destroys that heap.
 *       - The memory, BC250_VKD3D_IMPORTED_MEMORY: a whole VkDeviceMemory of AllocationSize bytes and type
 *         MemoryTypeIndex on the engine's VkDevice, not a dedicated allocation, and not mapped by the shell while
 *         the heap exists (the engine maps a CPU-visible heap's memory, and Vulkan allows one mapping per
 *         VkDeviceMemory). Flags has BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS exactly when the memory was
 *         allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, which a heap that allows buffers needs. The type
 *         must be one the engine picks for an engine heap of the same properties and flags (its own memory type
 *         masks for buffers, sampled images and render-target or depth images), host-visible for a CPU-visible
 *         heap. The engine checks Size, Flags, a non-null Memory, MemoryTypeIndex, SizeInBytes against
 *         AllocationSize, the type, and that the device address of the heap's buffer is aligned to the heap's
 *         alignment, at most 64 KiB, because it cannot pad memory it did not allocate. It cannot see whether the
 *         memory belongs to its VkDevice, is dedicated or is mapped: those are caller errors.
 *       - The heap description: D3D12's rules for CreateHeap. Types DEFAULT, UPLOAD, READBACK, CUSTOM and, where
 *         D3D12_FEATURE_D3D12_OPTIONS16 reports it, GPU_UPLOAD. Flags DENY_BUFFERS, DENY_RT_DS_TEXTURES,
 *         DENY_NON_RT_DS_TEXTURES and ALLOW_SHADER_ATOMICS; CREATE_NOT_RESIDENT, CREATE_NOT_ZEROED and
 *         TOOLS_USE_MANUAL_WRITE_TRACKING are accepted and ignored, since residency and contents are the shell's.
 *         SHARED, SHARED_CROSS_ADAPTER, ALLOW_DISPLAY, HARDWARE_PROTECTED, ALLOW_WRITE_WATCH and unknown bits are
 *         refused.
 *       - Tiers and categories. The resource categories are buffers, non-RT/DS textures and RT/DS textures. On
 *         D3D12_RESOURCE_HEAP_TIER_1 a heap allows exactly one of them, on TIER_2 any non-empty set. The engine
 *         reports TIER_2 when bufferImageGranularity is at most 64 KiB (OPTIONS.ResourceHeapTier, through
 *         CheckFeatureSupport and V11), and its CreateDevice fails when no memory type holds all three
 *         categories at once. A CPU-visible heap (UPLOAD, READBACK, GPU_UPLOAD or a CPU-visible CUSTOM heap) must
 *         allow buffers and holds buffers only: the engine puts no texture in CPU-visible memory and refuses to
 *         place one on such a heap.
 *       - Placed resources. CreatePlacedResource, CreatePlacedResource1 and CreatePlacedResource2 on such a heap
 *         behave as on an engine heap: the resource uses the heap's memory at HeapOffset, which must be a
 *         multiple of the resource's placement alignment and keep it within SizeInBytes, and it holds a reference
 *         on the heap. Resources at overlapping offsets alias as on any heap. A texture whose memoryTypeBits
 *         exclude MemoryTypeIndex is refused (the type check above uses representative images).
 *       - MapHeap and UnmapHeap work on every engine heap whose memory the engine keeps mapped: CPU-visible heaps
 *         that allow buffers, from CreateHeapFromMemory or ID3D12Device::CreateHeap. MapHeap returns the CPU
 *         address of heap offset 0; a buffer placed at offset X maps at that address plus X, the pointer its
 *         ID3D12Resource::Map returns. Calls are counted, and on memory that is not host-coherent MapHeap
 *         invalidates and UnmapHeap flushes the heap's range. The address stays valid until the heap is
 *         destroyed, whatever the count. heap is an interface pointer of the heap (ID3D12Heap, ID3D12Heap1);
 *         another engine object gives E_INVALIDARG, and a pointer that is not a COM object is undefined
 *         behaviour.
 *       - Errors. Every refusal above is E_INVALIDARG and creates nothing; so are MapHeap on a heap without an
 *         address and UnmapHeap without a MapHeap.
 *       - Destruction. The heap is destroyed in the last of its own final Release and the final Releases of the
 *         resources placed on it. d3d12_heap_Release, d3d12_heap_decref, d3d12_heap_destroy, vkd3d_free_memory
 *         and vkd3d_memory_allocation_free destroy the heap's VkBuffer, remove its GPU virtual address range and,
 *         for a CPU-visible heap, call vkUnmapMemory; they do not call vkFreeMemory. A placed resource's final
 *         Release (d3d12_resource_Release, d3d12_resource_decref, d3d12_resource_destroy) destroys its VkImage
 *         and its views at once: vkd3d-proton's retention of released resources until their submissions finish
 *         (DEFER_RESOURCE_DESTRUCTION application profiles, resources named FSR3UPSCALER or FSR4UPSCALER, GPU
 *         clears in flight) is off for resources on imported heaps. Neither Release waits for the GPU. Command
 *         allocators and descriptor heaps may keep Vulkan views of such resources, but no reference to them or
 *         to the heap; those views are never used unless the application uses a descriptor of a destroyed
 *         resource, which D3D12 forbids.
 *       - What the final Release guarantees: once the heap and every resource placed on it have returned from
 *         their final Release, the engine makes no further Vulkan call on the memory and submits no further GPU
 *         work that uses it. It guarantees nothing about work already submitted. The engine submits no work of
 *         its own on imported memory, so every GPU access to it comes from ExecuteCommandLists (including the
 *         initial layout transitions of the resources, which go with the command lists that use them) and
 *         UpdateTileMappings on the engine's queues.
 *       - The safe order before vkFreeMemory is the shell's; the engine adds no deferral:
 *           1. wait until every queue that used the heap or its resources has completed that work: a fence value
 *              that the queue signalled after its last such submission is reached (GetCompletedValue or
 *              SetEventOnCompletion, V8), or an equivalent wait on the queue's timeline;
 *           2. release the heap and every resource placed on it;
 *           3. vkFreeMemory.
 *         Any other order is a caller error: releasing a resource still in use by the GPU (which D3D12 forbids,
 *         and which the retention above does not cover here) or freeing the memory while the heap, a placed
 *         resource or work using it still exists (VUID-vkFreeMemory-memory-00677). The engine neither detects it
 *         nor survives it.
 *   V11 Adapter capabilities (1.2, r3-draft). QueryAdapterCaps(info, count, queries) answers CheckFeatureSupport
 *       queries for the device that CreateDevice(info) would create, before it exists: the runtime's GetCaps comes
 *       before CreateDevice.
 *       - Same policy. It checks info exactly as CreateDevice does (Sizes, AbiVersion, QueueMode, Services, the
 *         LUID, MinimumFeatureLevel, the refusals and the admission of V7) and fails with the same HRESULTs. It
 *         then runs the capability decisions through the functions CreateDevice uses (vkd3d_create_adapter_caps:
 *         the physical device's extensions, features and properties with vkd3d-proton's clamps and workarounds,
 *         the queue selection of the queue mode, the memory, descriptor-buffer and bindless decisions, and
 *         d3d12_device_caps_init) and answers through d3d12_device_check_feature_support, the body of
 *         ID3D12Device::CheckFeatureSupport. It never answers from raw Vulkan features.
 *       - Footprint. It takes vkd3d-proton's instance as CreateDevice does (V12): the one that live engine devices
 *         share, or a new VkInstance that it destroys before it returns; always a new one when PRIVATE. It
 *         creates no VkDevice and no queue, allocates no GPU memory, starts no thread and calls no Services.
 *       - Queries. Each BC250_VKD3D_FEATURE_QUERY is one CheckFeatureSupport call: Feature, pData and DataSize
 *         as there, with the input members of *pData filled in. Result receives the HRESULT that a device made
 *         with info returns, and *pData its data. Features that read the state of a device
 *         (PLACED_RESOURCE_SUPPORT_INFO, QUERY_META_COMMAND) and features vkd3d-proton does not answer give
 *         DXGI_ERROR_UNSUPPORTED and leave *pData alone; a non-zero Reserved gives E_INVALIDARG.
 *       - Return value. S_OK once the adapter was examined, whatever the Results. Otherwise the failure, with
 *         every Result set to it and nothing written to any pData. count may be 0, with queries NULL, which only
 *         checks info and the admission.
 *       - Equality. For every feature it answers, Result and data equal what CheckFeatureSupport returns on a
 *         device that CreateDevice made from the same info in the same process and environment (VKD3D_CONFIG and
 *         the defaults of V6 apply to both). CreateDevice can still fail after QueryAdapterCaps succeeded, in the
 *         steps that need a VkDevice: vkCreateDevice, memory, the legacy bindless descriptor set layouts, and the
 *         check that a memory type backs heap tier 2. One answer can differ, on NVIDIA only: when vkCreateDevice
 *         fails with VK_ERROR_INITIALIZATION_FAILED, CreateDevice retries without the NVX extensions, which can
 *         change the descriptor-buffer decision; QueryAdapterCaps cannot know that vkCreateDevice would fail.
 *   V12 Instance mode (1.2, r4-draft). CreateInfo.InstanceMode picks the VkInstance of a CreateDevice or a
 *       QueryAdapterCaps.
 *       - SHARED (0, also the mode when the field is not read): vkd3d-proton's rule. While any engine device
 *         lives, CreateDevice and QueryAdapterCaps take the live devices' VkInstance and make no vkCreateInstance;
 *         the engine then calls their GetInstanceProcAddr on a VkInstance that another call's entry point created.
 *       - PRIVATE (1): CreateDevice creates a VkInstance of the device's own, destroyed with the device (V4);
 *         QueryAdapterCaps creates one and destroys it before it returns. No other device or query gets it,
 *         whatever their modes. The engine calls the GetInstanceProcAddr of such a device only inside its
 *         CreateDevice (of a query, inside QueryAdapterCaps), on the calling thread, with NULL or that VkInstance,
 *         and every Vulkan object of the device descends from it (GetVulkanHandles returns it). A shell whose
 *         entry point binds per-device state to the VkInstance (hosted RADV binds its runtime identity there)
 *         sets PRIVATE on every CreateDevice and QueryAdapterCaps, in either queue mode.
 *       - Reading. InstanceMode needs a Size that covers it and AbiVersion 1.2; values other than SHARED and
 *         PRIVATE are E_INVALIDARG. Below 1.2, PRIVATE is E_INVALIDARG, since the shell would not get what it
 *         asked for, and other values are ignored (SHARED). PRIVATE refuses RenderDoc's singletons (V2).
 *       - Threads. In both modes the engine creates and destroys VkInstances one at a time in the process
 *         (vkd3d-proton's instance lock): two CreateDevice calls never run vkCreateInstance at once.
 *       - Older engines. An r3 engine reads no InstanceMode and gives SHARED. The shell pins the engine build; it
 *         can also check that two live PRIVATE devices return two VkInstances from GetVulkanHandles.
 *   V13 Linear images (1.3, r5-draft). An image with VK_IMAGE_TILING_LINEAR exists only because the shell
 *       called CreateLinearPlacedResource: no resource description, layout value or heap flag selects one, and
 *       the device's own CreatePlacedResource and GetResourceAllocationInfo are unchanged.
 *       - Shape. A 2D colour image with one mip level, one layer and one sample, without depth-stencil use, on a
 *         heap without CPU access. Anything else is E_INVALIDARG. A format that the device does not support
 *         with linear tiling and the usage the description implies is E_NOTIMPL; the engine never falls back to
 *         another tiling.
 *       - Order. QueryLinearImage answers before any memory exists, from an image that it creates unbound and
 *         destroys before it returns; the shell sizes its allocation and describes the surface from the answer.
 *         CreateLinearPlacedResource places the image on an engine heap (CreateHeapFromMemory, V10) and returns
 *         the same structure for the bound image. The engine does not compare the two: the shell does, and
 *         releases the resource when they differ.
 *       - Sizes. LayoutSize is the subresource's size, MemorySize and MemoryAlignment are the image's memory
 *         requirements, MemoryTypeBits its memory types. None of them is the size of the shell's allocation.
 *       - Layout. The image is used in VK_IMAGE_LAYOUT_GENERAL, as every image of vkd3d-proton whose tiling is
 *         linear. InitialState and the clear value are those of CreatePlacedResource.
 */
#ifndef BC250_VKD3D_ENGINE_H
#define BC250_VKD3D_ENGINE_H

#include <windows.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC250_VKD3D_ENGINE_ABI_MAJOR 1u
#define BC250_VKD3D_ENGINE_ABI_MINOR 3u
#define BC250_VKD3D_ENGINE_ABI_VERSION ((BC250_VKD3D_ENGINE_ABI_MAJOR << 16) | BC250_VKD3D_ENGINE_ABI_MINOR)

#define BC250_VKD3D_QUEUE_MODE_THREADED 0u         /* V3; also the mode when the 1.1 fields are absent */
#define BC250_VKD3D_QUEUE_MODE_INLINE   1u         /* V7, 1.1 */

#define BC250_VKD3D_INSTANCE_MODE_SHARED  0u       /* V12; also the mode when the 1.2 field is not read */
#define BC250_VKD3D_INSTANCE_MODE_PRIVATE 1u       /* V12, 1.2 r4 */

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
    /* 1.2 (r4): read only when Size covers it and AbiVersion is 1.2 or later, with the exception of V12. */
    UINT32 InstanceMode;                           /* BC250_VKD3D_INSTANCE_MODE_*; other values: E_INVALIDARG */
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

/* The 1.1 layout, which r4 extends with InstanceMode; a shell built against r2 or r3 passes its sizeof. */
typedef struct BC250_VKD3D_DEVICE_CREATE_INFO_1_1
{
    UINT32 Size;
    UINT32 AbiVersion;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    LUID AdapterLuid;
    UINT32 MinimumFeatureLevel;
    UINT32 QueueMode;
    const BC250_VKD3D_SHELL_SERVICES *Services;
} BC250_VKD3D_DEVICE_CREATE_INFO_1_1;

#define BC250_VKD3D_DEVICE_CREATE_INFO_SIZE_1_1 ((UINT32)sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1))

/* The members of D3D12_COMMAND_QUEUE_DESC, which this header cannot include (see above). */
typedef struct BC250_VKD3D_COMMAND_QUEUE_DESC
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_COMMAND_QUEUE_DESC) */
    UINT32 Type;                                   /* D3D12_COMMAND_LIST_TYPE: DIRECT, COMPUTE or COPY */
    INT32 Priority;                                /* D3D12_COMMAND_QUEUE_PRIORITY; recorded, not used */
    UINT32 Flags;                                  /* D3D12_COMMAND_QUEUE_FLAGS */
    UINT32 NodeMask;                               /* 0 or 1 */
} BC250_VKD3D_COMMAND_QUEUE_DESC;

/* 1.2, V10: allocated with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; required when the heap allows buffers. */
#define BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS 0x1u

/* 1.2, V10. Memory the shell allocated or imported on the engine's VkDevice and keeps. */
typedef struct BC250_VKD3D_IMPORTED_MEMORY
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_IMPORTED_MEMORY) */
    VkDeviceMemory Memory;                         /* a whole allocation, not dedicated, not mapped by the shell */
    VkDeviceSize AllocationSize;                   /* its VkMemoryAllocateInfo::allocationSize */
    UINT32 MemoryTypeIndex;                        /* its VkMemoryAllocateInfo::memoryTypeIndex */
    UINT32 Flags;                                  /* BC250_VKD3D_IMPORTED_MEMORY_FLAG_*; other bits: E_INVALIDARG */
} BC250_VKD3D_IMPORTED_MEMORY;

/* 1.2, V11. One CheckFeatureSupport call of QueryAdapterCaps. */
typedef struct BC250_VKD3D_FEATURE_QUERY
{
    UINT32 Feature;                                /* a D3D12_FEATURE */
    UINT32 DataSize;                               /* the size of *pData, as CheckFeatureSupport expects it */
    void *pData;                                   /* input members filled in; receives the answer */
    HRESULT Result;                                /* filled by the engine */
    UINT32 Reserved;                               /* 0 */
} BC250_VKD3D_FEATURE_QUERY;

/* 1.3, V13. A linear image: its one subresource and its memory requirements. */
typedef struct BC250_VKD3D_LINEAR_IMAGE_INFO
{
    UINT32 Size;                                   /* sizeof(BC250_VKD3D_LINEAR_IMAGE_INFO), set by the caller */
    UINT32 MemoryTypeBits;                         /* VkMemoryRequirements::memoryTypeBits */
    UINT64 Offset;                                 /* VkSubresourceLayout::offset */
    UINT64 RowPitch;                               /* VkSubresourceLayout::rowPitch, bytes */
    UINT64 LayoutSize;                             /* VkSubresourceLayout::size */
    UINT64 MemorySize;                             /* VkMemoryRequirements::size */
    UINT64 MemoryAlignment;                        /* VkMemoryRequirements::alignment */
} BC250_VKD3D_LINEAR_IMAGE_INFO;

/* The D3D12 structures of 1.2 and 1.3 by their struct tags (see C and C++ above). */
struct D3D12_HEAP_DESC;
struct D3D12_RESOURCE_DESC1;
struct D3D12_CLEAR_VALUE;

typedef struct BC250_VKD3D_ENGINE_FUNCS
{
    UINT32 Size;                                   /* set by the caller; the engine fills at most this much */
    UINT32 AbiVersion;                             /* filled by the engine: its own version */

    /* Creates a vkd3d-proton D3D12 device on the adapter (V1, V2) and returns the interface riid of it.
     * E_INVALIDARG for a bad argument, an unknown LUID or an adapter below MinimumFeatureLevel (vkd3d-proton's
     * own answer), E_NOINTERFACE for an AbiVersion the engine does not serve or an riid the device does not
     * implement, DXGI_ERROR_UNSUPPORTED for an INLINE device the adapter does not admit (V7), the device's
     * failure otherwise. */
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

    /* 1.2, V10. The engine device's Vulkan objects and the queue family of its DIRECT queues. E_INVALIDARG, and
     * nothing written, when a pointer is NULL. device as for CreateCommandQueue, in either queue mode. */
    HRESULT (APIENTRY *GetVulkanHandles)(void *device, VkInstance *instance, VkPhysicalDevice *physicalDevice,
            VkDevice *vkDevice, UINT32 *queueFamilyIndex);

    /* 1.2, V10. Creates an ID3D12Heap of device over memory, which stays the shell's, and returns the interface
     * riid of it (IID_ID3D12Heap, IID_ID3D12Heap1). heapDesc is a D3D12_HEAP_DESC. E_INVALIDARG for anything V10
     * refuses, E_NOINTERFACE for an riid the heap does not implement, E_OUTOFMEMORY; nothing is created on
     * failure. device as for CreateCommandQueue, in either queue mode. */
    HRESULT (APIENTRY *CreateHeapFromMemory)(void *device, const BC250_VKD3D_IMPORTED_MEMORY *memory,
            const struct D3D12_HEAP_DESC *heapDesc, REFIID riid, void **heap);

    /* 1.2, V10. The CPU address of offset 0 of an engine heap whose memory the engine keeps mapped, and the
     * matching unmap. E_INVALIDARG for other heaps, for objects that are not engine heaps and for UnmapHeap
     * without MapHeap. */
    HRESULT (APIENTRY *MapHeap)(void *heap, void **cpuAddress);
    HRESULT (APIENTRY *UnmapHeap)(void *heap);

    /* 1.2, V11. Answers count CheckFeatureSupport queries for the device that CreateDevice(info) would create,
     * without creating one. S_OK once the adapter was examined (each query has its Result); otherwise
     * CreateDevice's failure for info, in every Result as well. */
    HRESULT (APIENTRY *QueryAdapterCaps)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, UINT32 count,
            BC250_VKD3D_FEATURE_QUERY *queries);

    /* 1.3, V13. What a linear image of desc (a D3D12_RESOURCE_DESC1) is on device. E_INVALIDARG for a NULL
     * pointer, an info->Size below the structure's or a description outside V13's shape; E_NOTIMPL when the
     * device has no such image. On failure info keeps its Size and is zero otherwise. device as for
     * CreateCommandQueue, in either queue mode. */
    HRESULT (APIENTRY *QueryLinearImage)(void *device, const struct D3D12_RESOURCE_DESC1 *desc,
            BC250_VKD3D_LINEAR_IMAGE_INFO *info);

    /* 1.3, V13. CreatePlacedResource1 of device with a linear image: heap is an engine heap of the device,
     * initialState a D3D12_RESOURCE_STATES value. Returns the interface riid of the resource and, in info, the
     * bound image. Failures as QueryLinearImage and as CreatePlacedResource1; nothing is created on failure. */
    HRESULT (APIENTRY *CreateLinearPlacedResource)(void *device, void *heap, UINT64 heapOffset,
            const struct D3D12_RESOURCE_DESC1 *desc, UINT32 initialState,
            const struct D3D12_CLEAR_VALUE *optimizedClearValue, REFIID riid, void **resource,
            BC250_VKD3D_LINEAR_IMAGE_INFO *info);
} BC250_VKD3D_ENGINE_FUNCS;

typedef struct BC250_VKD3D_ENGINE_FUNCS_1_0
{
    UINT32 Size;
    UINT32 AbiVersion;
    HRESULT (APIENTRY *CreateDevice)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device);
} BC250_VKD3D_ENGINE_FUNCS_1_0;

#define BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0 ((UINT32)sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0))

/* The 1.1 layout of the function table, which 1.2 extends; a 1.1 shell passes its sizeof. */
typedef struct BC250_VKD3D_ENGINE_FUNCS_1_1
{
    UINT32 Size;
    UINT32 AbiVersion;
    HRESULT (APIENTRY *CreateDevice)(const BC250_VKD3D_DEVICE_CREATE_INFO *info, REFIID riid, void **device);
    HRESULT (APIENTRY *CreateCommandQueue)(void *device, const BC250_VKD3D_COMMAND_QUEUE_DESC *desc,
            void *queueCookie, REFIID riid, void **queue);
} BC250_VKD3D_ENGINE_FUNCS_1_1;

#define BC250_VKD3D_ENGINE_FUNCS_SIZE_1_1 ((UINT32)sizeof(BC250_VKD3D_ENGINE_FUNCS_1_1))

/* The 1.2 layout of the function table ends with QueryAdapterCaps; a 1.2 shell passes this size. */
#define BC250_VKD3D_ENGINE_FUNCS_SIZE_1_2 \
        ((UINT32)(offsetof(BC250_VKD3D_ENGINE_FUNCS, QueryAdapterCaps) + sizeof(void *)))

/* Compile-time checks, in C and C++: each minor's structures extend the previous ones, and the sizes of 1.0 and
 * 1.1 are fixed (offsetof comes from vulkan_core.h, through vk_platform.h and stddef.h). */
#define BC250_VKD3D_STATIC_ASSERT(name, e) typedef char BC250_VKD3D_STATIC_ASSERT_##name[(e) ? 1 : -1]
BC250_VKD3D_STATIC_ASSERT(create_info_1_0_prefix,
        offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, AbiVersion) == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, AbiVersion)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, GetInstanceProcAddr)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, GetInstanceProcAddr)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, AdapterLuid) == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, AdapterLuid)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0, MinimumFeatureLevel)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, MinimumFeatureLevel)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, Services) >= sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0));
BC250_VKD3D_STATIC_ASSERT(create_info_1_1_prefix,
        offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1, MinimumFeatureLevel)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, MinimumFeatureLevel)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1, QueueMode)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, QueueMode)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1, Services)
                == offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, Services)
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, InstanceMode) == sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1));
BC250_VKD3D_STATIC_ASSERT(engine_funcs_1_0_prefix,
        offsetof(BC250_VKD3D_ENGINE_FUNCS_1_0, AbiVersion) == offsetof(BC250_VKD3D_ENGINE_FUNCS, AbiVersion)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS_1_0, CreateDevice) == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateDevice)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateCommandQueue) == sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0));
BC250_VKD3D_STATIC_ASSERT(engine_funcs_1_1_prefix,
        offsetof(BC250_VKD3D_ENGINE_FUNCS_1_1, CreateDevice) == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateDevice)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS_1_1, CreateCommandQueue)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateCommandQueue)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, GetVulkanHandles) == sizeof(BC250_VKD3D_ENGINE_FUNCS_1_1));
BC250_VKD3D_STATIC_ASSERT(engine_funcs_1_2_order,
        offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateHeapFromMemory)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, GetVulkanHandles) + sizeof(void *)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, MapHeap)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateHeapFromMemory) + sizeof(void *)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, UnmapHeap) == offsetof(BC250_VKD3D_ENGINE_FUNCS, MapHeap) + sizeof(void *)
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, QueryAdapterCaps)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, UnmapHeap) + sizeof(void *));
BC250_VKD3D_STATIC_ASSERT(engine_funcs_1_3_order,
        offsetof(BC250_VKD3D_ENGINE_FUNCS, QueryLinearImage) == BC250_VKD3D_ENGINE_FUNCS_SIZE_1_2
        && offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateLinearPlacedResource)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, QueryLinearImage) + sizeof(void *)
        && sizeof(BC250_VKD3D_ENGINE_FUNCS)
                == offsetof(BC250_VKD3D_ENGINE_FUNCS, CreateLinearPlacedResource) + sizeof(void *));
#ifdef _WIN64
BC250_VKD3D_STATIC_ASSERT(sizes_1_0_x64,
        sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_0) == 32 && sizeof(BC250_VKD3D_ENGINE_FUNCS_1_0) == 16);
BC250_VKD3D_STATIC_ASSERT(sizes_1_1_x64,
        sizeof(BC250_VKD3D_DEVICE_CREATE_INFO_1_1) == 40 && sizeof(BC250_VKD3D_ENGINE_FUNCS_1_1) == 24
        && sizeof(BC250_VKD3D_SHELL_SERVICES) == 32 && sizeof(BC250_VKD3D_COMMAND_QUEUE_DESC) == 20);
BC250_VKD3D_STATIC_ASSERT(sizes_1_2_x64,
        BC250_VKD3D_ENGINE_FUNCS_SIZE_1_2 == 64 && sizeof(BC250_VKD3D_IMPORTED_MEMORY) == 32
        && offsetof(BC250_VKD3D_IMPORTED_MEMORY, Memory) == 8 && offsetof(BC250_VKD3D_IMPORTED_MEMORY, Flags) == 28
        && sizeof(BC250_VKD3D_FEATURE_QUERY) == 24 && offsetof(BC250_VKD3D_FEATURE_QUERY, Result) == 16
        && sizeof(BC250_VKD3D_DEVICE_CREATE_INFO) == 48
        && offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, InstanceMode) == 40);
BC250_VKD3D_STATIC_ASSERT(sizes_1_3_x64,
        sizeof(BC250_VKD3D_ENGINE_FUNCS) == 80 && sizeof(BC250_VKD3D_LINEAR_IMAGE_INFO) == 48
        && offsetof(BC250_VKD3D_LINEAR_IMAGE_INFO, Offset) == 8
        && offsetof(BC250_VKD3D_LINEAR_IMAGE_INFO, MemoryAlignment) == 40);
#endif

/* The one export. E_NOINTERFACE when abiVersion names another major or a later minor than the engine's;
 * E_INVALIDARG when funcs is NULL or funcs->Size is smaller than the 1.0 structure
 * (BC250_VKD3D_ENGINE_FUNCS_SIZE_1_0). Otherwise it zeroes funcs->Size bytes and fills, within them, Size,
 * AbiVersion (the engine's) and the entries of the minors up to abiVersion's (see Sizes). */
typedef HRESULT (APIENTRY *PFN_BC250_VKD3D_ENGINE_GET_FUNCS)(UINT32 abiVersion, BC250_VKD3D_ENGINE_FUNCS *funcs);
#define BC250_VKD3D_ENGINE_GET_FUNCS_NAME "Bc250Vkd3dEngineGetFuncs"

#ifdef __cplusplus
}
#endif

#endif /* BC250_VKD3D_ENGINE_H */
